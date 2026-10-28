#!/usr/bin/env python3
"""Capture the sirius XNU USB console (CDC-ACM over DWC3).

The kernel streams its console mirror ring on a bulk IN endpoint, so the host sees the boot log
from the very first line once the gadget enumerates. The port is a plain CDC-ACM tty, so no USB
library is needed: termios raw mode and a read loop are enough.

    sudo python3 scripts/capture-usb-console.py --wait 120 --duration 60

Without --port the script prefers a /dev/serial/by-id link that names the sirius console and
otherwise takes the first /dev/ttyACM* that appears. It needs root (or dialout) because the tty is
root:dialout.

    sudo python3 scripts/capture-usb-console.py --follow --output boot.log

--follow survives re-enumeration: when the read returns end-of-file or the port disappears the
script waits for the port again and resumes appending to the same log.

On Windows the inbox usbser.sys driver binds the console as a COM port (USB\\VID_1209&PID_0001,
serial SIRIUS01) without any driver install, and the script finds it under that hardware ID:

    python scripts/capture-usb-console.py --follow --output boot.log

The port is opened with plain Win32 file calls and never configured: the gadget does not answer
the CDC SET_LINE_CODING request, so SetCommState (and pyserial, which always calls it) fails with
error 121 while reads and writes work.
"""
import argparse
import glob
import os
import sys
import time

try:
    import select
    import termios
    import tty
except ImportError:  # Windows: the port is a COM port, read through kernel32
    import ctypes
    import winreg
    from ctypes import wintypes
    termios = None


def find_port():
    if termios is None:
        # usbser.sys records the COM name of each enumerated port in the device's registry key.
        key = r'SYSTEM\CurrentControlSet\Enum\USB\VID_1209&PID_0001'
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as device:
                for index in range(winreg.QueryInfoKey(device)[0]):
                    instance = winreg.EnumKey(device, index)
                    try:
                        with winreg.OpenKey(device, instance + r'\Device Parameters') as params:
                            name = winreg.QueryValueEx(params, 'PortName')[0]
                    except OSError:
                        continue
                    # The key outlives the device; only a port that is mapped now is present.
                    if 'SIRIUS' in instance.upper() and name in Console.present_ports():
                        return name
        except OSError:
            pass
        return None
    # A by-id link names the device even when several CDC ports are attached; the console calls
    # itself "sirius XNU USB console", so the interface name carries SIRIUS01.
    for candidate in sorted(glob.glob('/dev/serial/by-id/*')):
        if 'SIRIUS' in candidate.upper() or 'sirius' in candidate:
            return candidate
    for candidate in sorted(glob.glob('/dev/ttyACM*')):
        return candidate
    return None


def wait_for_port(timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        port = find_port()
        if port is not None:
            return port
        time.sleep(0.2)
    return None


class Console:
    """The open console port: a raw termios tty on Linux, an unconfigured COM handle on Windows."""

    @staticmethod
    def present_ports():
        """COM names Windows currently maps (HARDWARE\\DEVICEMAP\\SERIALCOMM)."""
        names = set()
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DEVICEMAP\SERIALCOMM') as comm:
                for index in range(winreg.QueryInfoKey(comm)[1]):
                    names.add(winreg.EnumValue(comm, index)[1])
        except OSError:
            pass
        return names

    def __init__(self, port):
        if termios is None:
            self.kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
            self.kernel32.CreateFileW.restype = wintypes.HANDLE
            handle = self.kernel32.CreateFileW('\\\\.\\' + port, 0xC0000000, 0, None, 3, 0, None)
            if handle == wintypes.HANDLE(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
            self.handle = handle
            # Reads return what arrived within 500 ms; writes give up after 1 s. These are local
            # to the driver and send nothing to the device, unlike SetCommState.
            timeouts = (wintypes.DWORD * 5)(0xFFFFFFFF, 0xFFFFFFFF, 500, 0, 1000)
            self.kernel32.SetCommTimeouts(self.handle, ctypes.byref(timeouts))
            self.buffer = ctypes.create_string_buffer(4096)
            return
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        attrs[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE)
        attrs[2] |= termios.CS8
        attrs[2] &= ~termios.CRTSCTS
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def write(self, data):
        if termios is None:
            done = wintypes.DWORD()
            if not self.kernel32.WriteFile(self.handle, data, len(data), ctypes.byref(done), None):
                raise ctypes.WinError(ctypes.get_last_error())
        else:
            os.write(self.fd, data)

    def read(self):
        """Up to 4096 bytes; b'' after half a second of silence, None at end-of-file."""
        if termios is None:
            done = wintypes.DWORD()
            if not self.kernel32.ReadFile(self.handle, self.buffer, 4096, ctypes.byref(done), None):
                raise ctypes.WinError(ctypes.get_last_error())  # the device went away
            return self.buffer.raw[:done.value]
        ready, _, _ = select.select([self.fd], [], [], 0.5)
        if not ready:
            return b''
        return os.read(self.fd, 4096) or None

    def close(self):
        if termios is None:
            self.kernel32.CloseHandle(self.handle)
        else:
            os.close(self.fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default=None,
                        help='CDC-ACM device, default: the SIRIUS01 port (first /dev/ttyACM*, or its COM port on Windows)')
    parser.add_argument('--wait', type=float, default=60.0, help='seconds to wait for the port')
    parser.add_argument('--duration', type=float, default=0.0,
                        help='seconds from the start of the script (the wait for the port included), 0 = until Ctrl-C')
    parser.add_argument('--output', default='out/sirius-usb-console.log', help='log file (bytes appended)')
    parser.add_argument('--follow', action='store_true',
                        help='keep waiting for the port after EOF or re-enumeration')
    parser.add_argument('--match', action='append', default=[],
                        help='stop as soon as the captured bytes contain this text (repeatable)')
    parser.add_argument('--match-window', type=float, default=4096.0,
                        help='how many trailing bytes the --match patterns are tested against')
    parser.add_argument('--timeline', default=None,
                        help='also write each distinct line once, prefixed with the seconds since the script '
                             'started at which it first arrived; start the script right after the reboot')
    parser.add_argument('--idle-stop', type=float, default=0.0,
                        help='stop once no new line has arrived for this many seconds (0 = never); the eMMC '
                             'heartbeat adds one every two seconds while the kernel runs, so silence means it stopped')
    args = parser.parse_args()
    started = time.time()
    # The driver replays its whole ring on every poke, so a line arrives many times; only its first
    # arrival says when it was written (within a poke or two once the console is up).
    timeline = open(args.timeline, 'a', encoding='utf-8') if args.timeline else None
    track_lines = timeline is not None or args.idle_stop > 0
    seen_lines = set()
    partial = b''
    last_new_line = None

    port = args.port or wait_for_port(args.wait)
    if port is None:
        print(f'no CDC-ACM port appeared within {args.wait:.0f}s', file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(args.output) or '.', exist_ok=True)
    # The driver replays its whole ring buffer as soon as the host sends any byte,
    # because otherwise the log is only handed over once, at enumeration.
    total = 0
    # Counted from the script's start, not from the port: a boot takes about a minute to bring the
    # console up, and a time limit that started only then let a capture run that much longer.
    end = started + args.duration if args.duration else None
    with open(args.output, 'ab') as log:
        try:
            next_poke = 0.0
            while end is None or time.time() < end:
                # Keep asking: the driver replays its ring on any host byte, and the
                # request is only served while its pump is running.
                if termios is not None and time.time() >= next_poke:
                    try:
                        poke = open(port, 'wb', buffering=0)
                        poke.write(b'\n')
                        poke.close()
                    except (OSError, TypeError):
                        pass
                    next_poke = time.time() + 1.0
                if port is None:
                    if not args.follow:
                        break
                    port = wait_for_port(args.wait)
                    if port is None:
                        break
                print(f'capturing {port} -> {args.output}', file=sys.stderr)
                try:
                    console = Console(port)
                except OSError as error:
                    print(f'{port}: {error}', file=sys.stderr)
                    port = None
                    time.sleep(1.0)
                    continue
                next_poke = 0.0
                tail = b''
                try:
                    while end is None or time.time() < end:
                        # Any byte makes the driver replay its ring buffer, and the
                        # request only succeeds while its pump is alive.
                        if time.time() >= next_poke:
                            try:
                                console.write(b'\n')
                            except OSError:
                                pass
                            next_poke = time.time() + 1.0
                        if (args.idle_stop > 0 and last_new_line is not None and
                                time.time() - last_new_line >= args.idle_stop):
                            print(f'no new line for {args.idle_stop:.0f}s, stopping after {total} bytes',
                                  file=sys.stderr)
                            return 0
                        chunk = console.read()
                        if chunk is None:
                            break
                        if not chunk:
                            continue
                        log.write(chunk)
                        log.flush()
                        total += len(chunk)
                        if track_lines:
                            lines = (partial + chunk).split(b'\n')
                            partial = lines.pop()
                            for line in lines:
                                line = line.rstrip(b'\r')
                                if line and line not in seen_lines:
                                    seen_lines.add(line)
                                    last_new_line = time.time()
                                    if timeline is not None:
                                        timeline.write(f'{last_new_line - started:8.1f} '
                                                       f'{line.decode("utf-8", "replace")}\n')
                            if timeline is not None:
                                timeline.flush()
                        sys.stdout.write(chunk.decode('utf-8', 'replace'))
                        sys.stdout.flush()
                        tail = (tail + chunk)[-int(args.match_window):] if args.match else b''
                        if args.match and any(pat.encode() in tail for pat in args.match):
                            print(f'matched, stopping after {total} bytes', file=sys.stderr)
                            return 0
                except OSError:
                    pass  # the device went away (reboot, re-enumeration)
                finally:
                    console.close()
                port = None if args.follow else port
                if not args.follow:
                    break
        except KeyboardInterrupt:
            pass
    if timeline is not None:
        timeline.close()
    print(f'\ncaptured {total} bytes', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
