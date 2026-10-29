#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Board recipe for the ShengHidInput component (drivers/ShengHidInput/NativeHidInput.cpp), replacing UniXNU's.

UniXNU's recipe qualifies the class against a prelinked reference candidate that only the sheng build produces. This board
boots the same pinned kernelcache, so the original IOHIDDevice vtable was read once from this board's own prelinked kernel,
after the same checks passed, into scripts/data/hid-device-abi.json; this recipe binds against that table instead.

Output (build/sheng-hid-input): OriginalHidDeviceABI.h, NativeHidInput.late.o (static constructor removed from
__mod_init_func and exported, so the board probe registers the class after IOHIDFamily has loaded), hid-original-bindings.o
and libShengHidInput.a. build-native-legacy-display.py links the two objects into the force-loaded late display archive.
"""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sheng-hid-input'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho


def require(value, why):
    if not value:
        raise SystemExit('ShengHidInput: ' + why)


def tool(name):
    found = shutil.which(name)
    if found:
        return found
    try:
        prefix = subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()
        candidate = Path(prefix) / 'bin' / name
        if candidate.is_file():
            return str(candidate)
    except (OSError, subprocess.CalledProcessError):
        pass
    for candidate in (f'/opt/homebrew/opt/llvm/bin/{name}', f'/usr/local/opt/llvm/bin/{name}'):
        if Path(candidate).is_file():
            return candidate
    raise SystemExit(f'ShengHidInput: {name} not found')


abi = json.loads((ROOT / 'scripts/data/hid-device-abi.json').read_text())
methods = [int(value, 16) for value in abi['methods']]
require(len(methods) == 207 and abi['instance_bytes'] == 208, 'unexpected recorded IOHIDDevice ABI')
vptr = int(abi['vptr'], 16)

# The same opaque header as UniXNU's recipe: only the slots NativeHidInput overrides or calls are named.
header = '''// Generated from the recorded original IOHIDDevice ABI (scripts/data/hid-device-abi.json); opaque private storage only.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
class IOHIDDevice : public IOService {
 OSDeclareAbstractStructors(IOHIDDevice)
 unsigned char originalPrivate[0xd0-sizeof(IOService)];
public:
 using IOService::setProperty;
 IOReturn Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 bool setProperty(const OSSymbol *,OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
'''
for i in range(168, 207):
    if i == 171:
        header += ' virtual OSString *newTransportString() const;\n'
    elif i == 173:
        header += ' virtual OSString *newProductString() const;\n'
    elif i == 180:
        header += ' virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const=0;\n'
    elif i == 181:
        header += ' virtual IOReturn handleReport(IOMemoryDescriptor *,unsigned,IOOptionBits);\n'
    else:
        header += f' virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque{i}();\n'
header += '};\nstatic_assert(sizeof(IOHIDDevice)==0xd0,"Original device instance size");\n'
(OUT / 'OriginalHidDeviceABI.h').write_text(header)

# Compile with the recorded IOKit flags, as build-arm-driver.py does.
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                    .read_text().strip().rstrip(','))
source = ROOT / 'drivers/ShengHidInput/NativeHidInput.cpp'
obj = OUT / 'NativeHidInput.o'
command, index, arguments = [], 0, record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command.extend(['-o', str(obj)])
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W') and value not in ('-MD', '-MP'):
        command.append(value)
    index += 1
    if value in ('-MF', '-MJ'):
        index += 1
command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-unknown-warning-option',
            '-Wno-missing-designated-field-initializers', '-Wno-overloaded-virtual', '-Wno-deprecated-declarations',
            '-I', str(OUT), '-I', str(ROOT / '.cache/xnu-work/osfmk')]
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(OUT / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-4000:])

# The compiled vtable: which slots NativeHidInput owns and which original IOHIDDevice methods it inherits.
raw = obj.read_bytes()
image = macho(raw)
st = next(p for c, p, _ in image['commands'] if c == 2)
sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
symbols = []
for i in range(count):
    ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
    symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
vtable = next(va for name, va, _ in symbols if name == '__ZTV14NativeHidInput')
relocations = {}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va'] <= vtable < sec['va'] + sec['size']:
            ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
            for i in range(rc):
                offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[sec['va'] + offset] = symbols[bits & 0xffffff][0]
compiled = [relocations.get(vtable + 16 + i * 8) for i in range(207)]
require(all(compiled) and vtable + 16 + 207 * 8 not in relocations, 'the compiled vtable is not exactly 207 slots')
bindings = {
    '__ZN11IOHIDDeviceC2EPK11OSMetaClass': int(abi['constructor'], 16),
    '__ZN11IOHIDDeviceD2Ev': int(abi['destructor'], 16),
    '__ZN11IOHIDDevice10gMetaClassE': int(abi['metaclass'], 16),
    '__ZTV11IOHIDDevice': vptr - 16,
}
owned = []
for i, name in enumerate(compiled):
    if name.startswith(('__ZN14NativeHidInput', '__ZNK14NativeHidInput')):
        owned.append(i)
    elif '11IOHIDDevice' in name:
        bindings[name] = methods[i]
require(owned == [0, 1, 7, 14, 86, 87, 111, 171, 173, 180], f'unexpected NativeHidInput overrides {owned}')
undefined = {name for name, _, kind in symbols if kind == 1 and '11IOHIDDevice' in name}
require(undefined <= set(bindings), f'unbound IOHIDDevice imports {sorted(undefined - set(bindings))}')

asm = OUT / 'hid-original-bindings.S'
asm.write_text(''.join(f'.globl {name}\n.set {name}, {address:#x}\n' for name, address in sorted(bindings.items())))
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', str(asm), '-o',
                str(OUT / 'hid-original-bindings.o')], check=True)
late = OUT / 'NativeHidInput.late.o'
subprocess.run([tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func',
                '--globalize-symbol=__GLOBAL__sub_I_NativeHidInput.cpp', str(obj), str(late)], check=True)
archive = OUT / 'libShengHidInput.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(late), str(OUT / 'hid-original-bindings.o')], check=True)
(OUT / 'abi-audit.json').write_text(json.dumps({'owned_slots': owned, 'bindings': {k: hex(v) for k, v in bindings.items()}},
                                               indent=2) + '\n')
print(f'ShengHidInput: owned slots {owned}, {len(bindings)} original bindings, {archive}')
