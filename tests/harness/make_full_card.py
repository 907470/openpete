#!/usr/bin/env python3
"""Write a full PS1 memory card: a formatted card with every block taken.

Usage: make_full_card.py SRC DST [COUNT]

SRC is a formatted 131072-byte card image (starts with "MC"); DST is SRC
with directory frames 1..COUNT (default 15, every block) replaced by
one-block saves of a foreign product code, BASLUS-00000Fnn, and blocks
1..COUNT replaced by an "SC" title header naming FOREIGN nn, the rest of
each block zero. Each directory frame holds state 0x51 (in use, first
block), size 8192, next-block link 0xFFFF and the XOR checksum of its
first 127 bytes in byte 127. The broken-sector list and the write-test
frame (block 0 frame 63) are copied from SRC unchanged.

The output depends only on SRC and COUNT, so a recipe that boots it
regenerates it from the blank formatted card instead of committing it.
"""
import sys

CARD_SIZE = 131072
BLOCK_SIZE = 8192
FRAME_SIZE = 128


def main(argv):
    """Write DST from SRC; return the process exit status."""
    if len(argv) not in (3, 4):
        sys.stderr.write(__doc__)
        return 64
    src, dst = argv[1], argv[2]
    count = int(argv[3]) if len(argv) == 4 else 15
    if not 1 <= count <= 15:
        sys.stderr.write('make_full_card: COUNT must be 1..15\n')
        return 64
    with open(src, 'rb') as f:
        card = bytearray(f.read())
    if len(card) != CARD_SIZE or card[:2] != b'MC':
        sys.stderr.write('make_full_card: %s is not a formatted %d-byte card\n'
                         % (src, CARD_SIZE))
        return 2
    for n in range(1, count + 1):
        frame = bytearray(FRAME_SIZE)
        frame[0:4] = (0x51).to_bytes(4, 'little')
        frame[4:8] = BLOCK_SIZE.to_bytes(4, 'little')
        frame[8:10] = (0xFFFF).to_bytes(2, 'little')
        name = b'BASLUS-00000F%02d' % n
        frame[10:10 + len(name)] = name
        x = 0
        for b in frame[:FRAME_SIZE - 1]:
            x ^= b
        frame[FRAME_SIZE - 1] = x
        card[n * FRAME_SIZE:(n + 1) * FRAME_SIZE] = frame
        block = bytearray(BLOCK_SIZE)
        block[0:2] = b'SC'
        block[2] = 0x11  # icon display flag: one static frame
        block[3] = 1     # block count of the save
        title = ('FOREIGN %02d' % n).encode('ascii')
        block[4:4 + len(title)] = title
        card[n * BLOCK_SIZE:(n + 1) * BLOCK_SIZE] = block
    with open(dst, 'wb') as f:
        f.write(card)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
