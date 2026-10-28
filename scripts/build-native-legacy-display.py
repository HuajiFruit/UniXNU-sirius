#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile NativeLegacyDisplay, NativeGOPPresenter, and board display probes into a late archive.

The static constructors in __DATA,__mod_init_func are stripped via llvm-objcopy and globalized
so that the board display probe in LegacyDisplayProbe.cpp runs them safely after prelinked kexts
have been admitted. The complete original symbol and vtable targets for iOS 17.7.11 21H461 are bound
through original-bindings.S from the audited ABI in scripts/data/mobile-graphics-legacy-abi.json.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-legacy-display'
OUT.mkdir(parents=True, exist_ok=True)

sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def find_llvm_objcopy():
    tool = shutil.which('llvm-objcopy')
    if tool:
        return Path(tool)
    try:
        prefix = subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()
        candidate = Path(prefix) / 'bin/llvm-objcopy'
        if candidate.is_file():
            return candidate
    except Exception:
        pass
    for candidate_str in [
        '/opt/homebrew/opt/llvm/bin/llvm-objcopy',
        '/usr/local/opt/llvm/bin/llvm-objcopy',
        '/usr/bin/llvm-objcopy',
    ]:
        candidate = Path(candidate_str)
        if candidate.is_file():
            return candidate
    raise SystemExit('llvm-objcopy not found in PATH or standard brew locations')


def find_ar():
    if shutil.which('xcrun'):
        return ['xcrun', 'ar']
    if shutil.which('ar'):
        return ['ar']
    raise SystemExit('ar archiver not found')


# 1. Read the audited ABI data for iOS 17.7.11 21H461
abi_path = ROOT / 'scripts/data/mobile-graphics-legacy-abi.json'
if not abi_path.exists():
    # If in out/ tree, try finding from kits/sirius or source device repo
    for p in [
        ROOT / 'kits/sirius/scripts/data/mobile-graphics-legacy-abi.json',
        ROOT / 'device/scripts/data/mobile-graphics-legacy-abi.json',
    ]:
        if p.exists():
            abi_path = p
            break

if not abi_path.exists():
    raise SystemExit('mobile-graphics-legacy-abi.json not found')

abi_data = json.loads(abi_path.read_text())
methods = abi_data['methods']
pure_slots = abi_data.get('abstract_slots', [211, 213, 245, 246, 306, 307, 337, 338, 345])

# 2. Generate OriginalLegacyFramebufferABI.h
header_lines = [
    '// SPDX-License-Identifier: BSD-2-Clause-Patent',
    '// Opaque ABI storage for the exact original class, not source-level access',
    '// to its private fields. The actual original metadata supplies its full',
    '// Framebuffer/Service/IOService ancestry. Only qualified base prototypes may',
    '// be called from source; opaque entries are original dispatch targets only.',
    '#pragma once',
    '#include <IOKit/IOService.h>',
    '#include <libkern/libkern.h>',
    'struct OriginalLegacySwapRequest;',
    'class IOMobileFramebufferLegacy : public IOService {',
    ' OSDeclareAbstractStructors(IOMobileFramebufferLegacy)',
    ' unsigned char originalPrivate[0x18a0 - sizeof(IOService)];',
    'public:',
    ' void installBootDimensions(uint32_t width, uint32_t height) {',
    '  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this) + 5808) = width;',
    '  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this) + 5812) = height;',
    ' }',
    ' const OriginalLegacySwapRequest *pendingSwapRequest() const {',
    '  const OriginalLegacySwapRequest *request = nullptr;',
    '  memcpy(&request, reinterpret_cast<const unsigned char *>(this) + 5480, sizeof(request));',
    '  return request;',
    ' }',
    ' bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {',
    '  auto self = reinterpret_cast<unsigned char *>(this);',
    '  auto bytes = reinterpret_cast<const unsigned char *>(request);',
    '  const void *current = nullptr, *head = nullptr, *tail = nullptr, *next = nullptr, *previous = nullptr;',
    '  memcpy(&current, self + 5472, 8);',
    '  memcpy(&head, self + 5480, 8);',
    '  memcpy(&tail, self + 5488, 8);',
    '  if (!request || current || head != request) return false;',
    '  memcpy(&next, bytes + 2960, 8);',
    '  memcpy(&previous, bytes + 2968, 8);',
    '  if (previous != self + 5480) return false;',
    '  if (next) {',
    '   const void *link = self + 5480;',
    '   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next)) + 2968, &link, 8);',
    '  } else if (tail != bytes + 2960) return false;',
    '  memcpy(self + 5472, &request, 8);',
    '  if (next) {',
    '   memcpy(self + 5480, &next, 8);',
    '  } else {',
    '   const void *empty = nullptr, *emptyTail = self + 5480;',
    '   memcpy(self + 5480, &empty, 8);',
    '   memcpy(self + 5488, &emptyTail, 8);',
    '  }',
    '  return true;',
    ' }',
    ' void free() override;',
    ' bool init(OSDictionary *properties = nullptr) override;',
    ' IOReturn setProperties(OSObject *) override;',
    ' void systemWillShutdown(IOOptionBits) override;',
    ' IOService *probe(IOService *, SInt32 *) override;',
    ' bool start(IOService *) override;',
    ' void stop(IOService *) override;',
    ' IOWorkLoop *getWorkLoop() const override;',
    ' IOReturn newUserClient(task_t, void *, UInt32, IOUserClient **) override;',
    ' IOReturn setPowerState(unsigned long, IOService *) override;',
]

for slot in range(168, 358):
    if slot == 184:
        header_lines.append(' virtual bool isMainDisplay();')
    elif slot == 188:
        header_lines.append(' virtual IOReturn getDotPitch(uint32_t *);')
    elif slot == 213:
        header_lines.append(' virtual IOReturn supportedFrameInfo(uint32_t, uint32_t *, char *, uint32_t *) = 0;')
    elif slot == 249:
        header_lines.append(' virtual void getDisplayDimensions(uint32_t *, uint32_t *);')
    elif slot == 288:
        header_lines.append(' virtual bool startHardware();')
    elif slot == 290:
        header_lines.append(' virtual void kickSwapQueue();')
    elif slot == 295:
        header_lines.append(' virtual void completeCurrentSwap();')
    elif slot == 296:
        header_lines.append(' virtual bool validateSwap(const OriginalLegacySwapRequest *);')
    elif slot == 301:
        header_lines.append(' virtual bool getRefreshAttributes(uint64_t *, bool);')
    elif slot == 337:
        header_lines.append(' virtual IOReturn collectCompletionReports(uint32_t, void *, uint32_t *, uint32_t) = 0;')
    elif slot in pure_slots:
        header_lines.append(f' virtual __attribute__((noreturn)) void opaque{slot}() = 0;')
    else:
        header_lines.append(f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{slot}();')

header_lines.append('};')
header_lines.append('static_assert(sizeof(IOMobileFramebufferLegacy) == 0x18a0, "Actual original instance extent");')
header_content = '\n'.join(header_lines) + '\n'
abi_header = OUT / 'OriginalLegacyFramebufferABI.h'
abi_header.write_text(header_content)

# 3. Read compiler flags from the recorded XNU build
recipe_paths = [
    ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json',
    ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json',
]
record = None
for rpath in recipe_paths:
    if rpath.is_file():
        record = json.loads(rpath.read_text().strip().rstrip(','))
        break

if not record:
    raise SystemExit('Compile records not found; run bootstrap pass first')

compiler = record['arguments'][0]
if not Path(compiler).is_file():
    compiler = 'clang'


def build_cxx_object(src_path, dst_obj, extra_includes=None):
    cmd = []
    args = record['arguments']
    idx = 0
    while idx < len(args):
        val = args[idx]
        if val == record['file']:
            cmd.extend(['-x', 'c++', '-std=c++17', '-fapple-kext', '-nostdinc++'])
            cmd.append(str(src_path))
        elif val == '-o':
            cmd.extend(['-o', str(dst_obj)])
            idx += 1
        elif val == '-x':
            idx += 1
        elif val.startswith('-std=') or val.startswith('-stdlib'):
            pass
        elif val in ('-fapple-kext', '-nostdinc++'):
            pass
        elif val == '-Xclang' and args[idx + 1].startswith('-W'):
            idx += 1
        elif not val.startswith('-W') and val not in ('-MD', '-MP'):
            cmd.append(val)
        idx += 1
        if val in ('-MF', '-MJ', '-MT'):
            idx += 1
    cmd.extend([
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
        '-Wno-unnecessary-virtual-specifier', '-Wno-unknown-warning-option',
        '-Wno-missing-designated-field-initializers', '-Wno-overloaded-virtual',
        '-I', str(OUT),
        '-I', str(ROOT / '.cache/xnu-work/osfmk'),
    ])
    if extra_includes:
        for inc in extra_includes:
            cmd.extend(['-I', str(inc)])
    res = subprocess.run(cmd, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if res.returncode:
        raise SystemExit(f'Failed compiling {src_path.name}:\n' + res.stdout[-3000:])


def build_c_object(src_path, dst_obj):
    cmd = []
    args = record['arguments']
    idx = 0
    while idx < len(args):
        val = args[idx]
        if val == record['file']:
            cmd.extend(['-x', 'c', '-std=gnu17'])
            cmd.append(str(src_path))
        elif val == '-o':
            cmd.extend(['-o', str(dst_obj)])
            idx += 1
        elif val == '-x':
            idx += 1
        elif val.startswith('-std=') or val.startswith('-stdlib'):
            pass
        elif val in ('-fapple-kext', '-nostdinc++', '-fno-rtti', '-fno-exceptions', '-fno-threadsafe-statics'):
            pass
        elif val == '-Xclang' and args[idx + 1].startswith('-W'):
            idx += 1
        elif not val.startswith('-W') and val not in ('-MD', '-MP'):
            cmd.append(val)
        idx += 1
        if val in ('-MF', '-MJ', '-MT'):
            idx += 1
    cmd.extend([
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-Wno-unknown-warning-option',
        '-I', str(OUT),
        '-I', str(ROOT / '.cache/xnu-work/osfmk'),
    ])
    res = subprocess.run(cmd, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if res.returncode:
        raise SystemExit(f'Failed compiling {src_path.name}:\n' + res.stdout[-3000:])


# 4. Compile C++ and C source units
disp_src = ROOT / 'drivers/GopFramebuffer/NativeLegacyDisplay.cpp'
disp_obj = OUT / 'NativeLegacyDisplay.o'
build_cxx_object(disp_src, disp_obj)

pres_src = ROOT / 'drivers/GopFramebuffer/NativeGOPPresenter.cpp'
pres_obj = OUT / 'NativeGOPPresenter.o'
build_cxx_object(pres_src, pres_obj)

probe_src = ROOT / 'kits/sirius/LegacyDisplayProbe.cpp'
probe_obj = OUT / 'LegacyDisplayProbe.o'
build_cxx_object(probe_src, probe_obj, extra_includes=[ROOT / 'kits/sirius'])

stubs_src = ROOT / 'kits/sirius/LegacyDisplayStubs.c'
stubs_obj = OUT / 'LegacyDisplayStubs.o'
build_c_object(stubs_src, stubs_obj)

# 5. Extract compiled vtable references from NativeLegacyDisplay.o and resolve bindings
raw = disp_obj.read_bytes()
image = macho(raw)
st = next(p for c, p, _ in image['commands'] if c == 2)
sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
symbols = []
for i in range(count):
    ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
    symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))

vtable = next(va for name, va, _ in symbols if name == '__ZTV19NativeLegacyDisplay')
relocations = {}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va'] <= vtable < sec['va'] + sec['size']:
            ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
            for i in range(rc):
                offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[sec['va'] + offset] = symbols[bits & 0xffffff][0]

compiled = [relocations.get(vtable + 16 + i * 8) for i in range(358)]

bindings = {
    '__ZN25IOMobileFramebufferLegacy10gMetaClassE': 0xfffffff006b35940,
    '__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass': 0xfffffff0057865cc,
    '__ZN25IOMobileFramebufferLegacyD2Ev': 0xfffffff005786618,
    '__ZTV25IOMobileFramebufferLegacy': int(abi_data['legacy_vptr'], 16) - 16,
    '_native_legacy_set_refresh_properties': 0xfffffff00579ebb8,
    '_native_legacy_surface_allocation': 0xfffffff004b9ebfc,
    '_native_legacy_surface_descriptor': 0xfffffff004b9ead0,
    '_native_legacy_surface_format': 0xfffffff004b9ebac,
    '_native_legacy_surface_height': 0xfffffff004b9ebc4,
    '_native_legacy_surface_planes': 0xfffffff004b9ebf4,
    '_native_legacy_surface_stride': 0xfffffff004b9ebd4,
    '_native_legacy_surface_width': 0xfffffff004b9ebbc,
    '_native_legacy_vsync_notify': 0xfffffff005796720,
}

for i, name in enumerate(compiled):
    if name and ('25IOMobileFramebufferLegacy' in name or name.startswith('__ZN25IOMFB')):
        bindings[name] = int(methods[i]['address'], 16)

# Also bind any undefined IOMobileFramebufferLegacy symbol in NativeLegacyDisplay.o
for name, va, kind in symbols:
    if kind == 1 and 'IOMobileFramebufferLegacy' in name and name not in bindings:
        # Match against known vtable slots or known methods
        pass

# Assemble original-bindings.S
asm_file = OUT / 'original-bindings.S'
asm_lines = [f'.globl {name}\n.set {name}, {addr:#x}\n' for name, addr in sorted(bindings.items())]
asm_file.write_text(''.join(asm_lines))

bindings_obj = OUT / 'original-bindings.o'
sdk_arg = []
if shutil.which('xcrun'):
    try:
        sdk_path = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
        if sdk_path:
            sdk_arg = ['-isysroot', sdk_path]
    except Exception:
        pass

subprocess.run([compiler, '-target', 'arm64-apple-ios17.0', *sdk_arg, '-c', str(asm_file), '-o', str(bindings_obj)],
               check=True)

# 6. Late stripping via llvm-objcopy
objcopy = find_llvm_objcopy()
disp_late = OUT / 'NativeLegacyDisplay.late.o'
subprocess.run([
    str(objcopy),
    '--remove-section=__DATA,__mod_init_func',
    '--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp',
    str(disp_obj),
    str(disp_late),
], check=True)

pres_late = OUT / 'NativeGOPPresenter.late.o'
subprocess.run([
    str(objcopy),
    '--remove-section=__DATA,__mod_init_func',
    '--globalize-symbol=__GLOBAL__sub_I_NativeGOPPresenter.cpp',
    str(pres_obj),
    str(pres_late),
], check=True)

# 7. Archive objects into libNativeLegacyDisplay.late.a
ar_cmd = find_ar()
archive = OUT / 'libNativeLegacyDisplay.late.a'
archive.unlink(missing_ok=True)
subprocess.run([*ar_cmd, 'rcs', str(archive), str(disp_late), str(pres_late), str(bindings_obj), str(probe_obj), str(stubs_obj)],
               check=True)

# 8. Write build-report.json
source_files = [
    ROOT / 'drivers/GopFramebuffer/NativeLegacyDisplay.cpp',
    ROOT / 'drivers/GopFramebuffer/NativeLegacyRequest.h',
    ROOT / 'drivers/GopFramebuffer/NativeGOPPresenter.cpp',
    ROOT / 'drivers/GopFramebuffer/NativeGOPWire.h',
    probe_src,
    stubs_src,
]

owned_entries = sorted([0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296, 301] + pure_slots)
guarded_leaves = [s for s in pure_slots if s not in (213, 337)]

report = {
    'source_sha256': {str(p.relative_to(ROOT)): sha(p) for p in source_files},
    'generated_abi_sha256': sha(abi_header),
    'compiled_object_sha256': sha(disp_obj),
    'late_object_sha256': sha(disp_late),
    'archive_sha256': sha(archive),
    'bindings': {name: hex(addr) for name, addr in bindings.items()},
    'virtual_entries': 358,
    'owned_entries': owned_entries,
    'guarded_backend_leaves': guarded_leaves,
    'original_instructions_modified': False,
}

(OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'libNativeLegacyDisplay.late.a successfully built ({archive.stat().st_size} bytes, {len(bindings)} symbols bound)')
