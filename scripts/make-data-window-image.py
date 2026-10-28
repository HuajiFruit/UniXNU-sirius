#!/usr/bin/env python3
"""Write the persistent Data container into userdata behind the root image, as a fastboot sparse image.

The Data volume of this board lives in the userdata partition right after the SHNGROOT root image,
at the next MiB (package/NativeEmmc.cpp, NativeEmmc::prepareDataWindow), as an APFS container the
Data seed workflow grew to its final size. The output is an Android sparse image for

    fastboot -S 512M flash userdata data-window.simg

that leaves the root image untouched (one DONT_CARE chunk up to the container) and writes the
container whole: its nonzero blocks as RAW chunks and its zero blocks as FILL chunks, because a zero
block of the container (a free-space bitmap, say) has to read back as zeros, not as what the
partition held before.

    python3 scripts/make-data-window-image.py --root userdata-root.img --container owner-data.cdr \\
        --output data-window.simg
"""
import argparse
import struct
import sys
from pathlib import Path

BLOCK = 4096
MIB = 1 << 20
SPARSE_MAGIC = 0xED26FF3A
RAW, FILL, DONT_CARE = 0xCAC1, 0xCAC2, 0xCAC3
RAW_LIMIT = 16 * MIB // BLOCK          # blocks per RAW chunk
# board.toml [userdata]: first_lba 0x1080000, last_lba 0x747BFDE, 512-byte blocks.
PARTITION_BYTES = (0x747BFDE - 0x1080000 + 1) * 512


def root_image_bytes(path):
    """The image size the SHNGROOT manifest records (u64 at offset 16)."""
    head = path.open('rb').read(24)
    if head[:8] != b'SHNGROOT':
        sys.exit(f'{path}: no SHNGROOT manifest')
    return struct.unpack_from('<Q', head, 16)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, required=True, help='the userdata-root.img flashed to the phone')
    parser.add_argument('--container', type=Path, required=True, help='the grown APFS Data container (raw)')
    parser.add_argument('--output', type=Path, required=True, help='the sparse image to write')
    args = parser.parse_args()

    offset = -(-root_image_bytes(args.root) // MIB) * MIB
    size = args.container.stat().st_size
    with args.container.open('rb') as handle:
        head = handle.read(BLOCK)
    block_size = struct.unpack_from('<I', head, 36)[0]
    blocks = struct.unpack_from('<Q', head, 40)[0]
    if head[32:36] != b'NXSB' or block_size != BLOCK or blocks * BLOCK != size:
        sys.exit(f'{args.container}: not an APFS container of its own file size ({blocks} x {block_size} vs {size})')
    if offset % BLOCK or offset + size > PARTITION_BYTES:
        sys.exit(f'the container ({size} bytes at {offset}) does not fit userdata ({PARTITION_BYTES} bytes)')

    chunks = []        # (type, blocks, payload): payload is a file offset for RAW
    def add(kind, count, payload=None):
        if chunks and chunks[-1][0] == kind and kind == FILL:
            last = chunks.pop()
            count += last[1]
        elif chunks and chunks[-1][0] == kind == RAW and chunks[-1][1] + count <= RAW_LIMIT:
            last = chunks.pop()
            count += last[1]
            payload = last[2]
        chunks.append((kind, count, payload))

    chunks.append((DONT_CARE, offset // BLOCK, None))
    zero = bytes(BLOCK)
    with args.container.open('rb') as handle:
        for index in range(size // BLOCK):
            data = handle.read(BLOCK)
            if data == zero:
                add(FILL, 1)
            else:
                add(RAW, 1, index * BLOCK)

    total = sum(count for _, count, _ in chunks)
    with args.output.open('wb') as out, args.container.open('rb') as source:
        out.write(struct.pack('<IHHHHIIII', SPARSE_MAGIC, 1, 0, 28, 12, BLOCK, total, len(chunks), 0))
        for kind, count, payload in chunks:
            if kind == RAW:
                out.write(struct.pack('<HHII', RAW, 0, count, 12 + count * BLOCK))
                source.seek(payload)
                out.write(source.read(count * BLOCK))
            elif kind == FILL:
                out.write(struct.pack('<HHII', FILL, 0, count, 16))
                out.write(struct.pack('<I', 0))
            else:
                out.write(struct.pack('<HHII', DONT_CARE, 0, count, 12))
    raw = sum(count for kind, count, _ in chunks if kind == RAW) * BLOCK
    print(f'Data container {size} bytes at userdata offset {offset} (LBA {0x1080000 + offset // 512}): '
          f'{raw} bytes of data, {len(chunks)} chunks, image {args.output.stat().st_size} bytes')


if __name__ == '__main__':
    main()
