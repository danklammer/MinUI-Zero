#!/usr/bin/env python3
# Patch the muOS-derived GPT for the MinUI Zero h700 image.
#
# The Allwinner trick this preserves (READ from the real card, 2026-08-05): the partition entry
# array is only 8 entries (1KB, bytes 1024..2048), ending well BEFORE boot0 at byte 8192 — that
# is how a standard-looking GPT and eGON boot0 share the first sectors. A stock 128-entry table
# would overwrite boot0 and brick the boot. Never "normalize" this table with standard tools.
#
# Usage: gpt.py <image> <total_sectors> <p5_last_lba> <p6_first_lba> <p6_last_lba>
# The image must already contain the verbatim preamble (whose LBA1/LBA2 carry the muOS primary
# GPT); entries 0-3 (spare/boot-resource/env/boot) are preserved untouched.
import sys, struct, zlib

img_path, total, p5_last, p6_first, p6_last = sys.argv[1], *map(int, sys.argv[2:6])

f = open(img_path, 'r+b')
f.seek(512)
hdr = bytearray(f.read(512))
assert hdr[:8] == b'EFI PART', 'no GPT in preamble'
entry_lba, n_entries, entry_sz = struct.unpack('<QII', hdr[72:88])
assert n_entries == 8 and entry_sz == 128, (n_entries, entry_sz)

f.seek(entry_lba * 512)
entries = bytearray(f.read(n_entries * entry_sz))

def set_range(idx, first, last):
    off = idx * entry_sz
    entries[off+32:off+48] = struct.pack('<QQ', first, last)

set_range(4, 319488, p5_last)      # rootfs
set_range(5, p6_first, p6_last)    # roms

entries_crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF

def build_header(cur, backup, first_use, last_use, e_lba):
    h = bytearray(hdr)
    struct.pack_into('<QQQQ', h, 24, cur, backup, first_use, last_use)
    struct.pack_into('<Q', h, 72, e_lba)
    struct.pack_into('<I', h, 88, entries_crc)
    struct.pack_into('<I', h, 16, 0)
    crc = zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF
    struct.pack_into('<I', h, 16, crc)
    return h

backup_lba = total - 1
backup_entries_lba = total - 1 - ((n_entries * entry_sz + 511) // 512)  # 2 sectors of entries

# primary: header at LBA1, entries stay at their muOS location (LBA2)
f.seek(512)
f.write(build_header(1, backup_lba, hdr and struct.unpack('<Q', hdr[40:48])[0], p6_last, entry_lba))
f.seek(entry_lba * 512)
f.write(entries)

# backup: entries then header at the image tail
f.seek(backup_entries_lba * 512)
f.write(entries)
f.seek(backup_lba * 512)
f.write(build_header(backup_lba, 1, struct.unpack('<Q', hdr[40:48])[0], p6_last, backup_entries_lba))
f.truncate((total) * 512)
f.close()
print(f"GPT patched: p5 ends {p5_last}, p6 {p6_first}..{p6_last}, backup at {backup_lba}")
print("NOTE: when dd'd to a card LARGER than the image, the backup GPT is not at the card's")
print("end — the kernel warns and uses the primary; fix later on-device if desired.")
