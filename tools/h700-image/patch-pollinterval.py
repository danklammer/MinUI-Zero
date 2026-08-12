#!/usr/bin/env python3
# Permanently apply the input-lag DTB patch (gpio_keys poll-interval 20ms -> 5ms + toc1 checksum
# fix) to (a) the raw-36mb boot-chain part, so every future build ships it, and (b) any already
# built image passed as argv. Offsets/algorithm verified on-device 2026-08-10 (dtb_patch_apply.py).
import struct, gzip, shutil, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
VAL_OFF   = 18033020          # abs offset of poll-interval be32 value
HEAD      = 16793600          # toc1 head (name[16] magic add_sum ...)
VALID_LEN = 1310720
STAMP     = 0x5F0A6C39
NEW_MS    = 5

def patch_buf(buf, label):
    cur = struct.unpack_from(">I", buf, VAL_OFF)[0]
    if cur == NEW_MS:
        print(f"{label}: already patched (poll={NEW_MS})")
        return False
    assert cur == 20, f"{label}: expected 20 at {VAL_OFF}, got {cur}"
    struct.pack_into(">I", buf, VAL_OFF, NEW_MS)
    tmp = bytearray(buf[HEAD:HEAD+VALID_LEN])
    struct.pack_into("<I", tmp, 20, STAMP)
    s = 0
    for off in range(0, VALID_LEN & ~3, 4):
        s = (s + struct.unpack_from("<I", tmp, off)[0]) & 0xFFFFFFFF
    struct.pack_into("<I", buf, HEAD+20, s)
    # verify
    tmp2 = bytearray(buf[HEAD:HEAD+VALID_LEN])
    stored = struct.unpack_from("<I", tmp2, 20)[0]
    struct.pack_into("<I", tmp2, 20, STAMP)
    s2 = 0
    for off in range(0, VALID_LEN & ~3, 4):
        s2 = (s2 + struct.unpack_from("<I", tmp2, off)[0]) & 0xFFFFFFFF
    val = struct.unpack_from(">I", buf, VAL_OFF)[0]
    ok = (val == NEW_MS and s2 == stored)
    print(f"{label}: poll={val} checksum stored={stored:#x} recomputed={s2:#x} -> {'OK' if ok else 'FAIL'}")
    if not ok:
        sys.exit(1)
    return True

# (a) the boot-chain part
part = os.path.join(HERE, "parts", "raw-36mb.img.gz")
buf = bytearray(gzip.open(part, "rb").read())
if patch_buf(buf, "parts/raw-36mb"):
    shutil.copyfile(part, part + ".pre-pollpatch")
    with gzip.open(part, "wb", compresslevel=6) as f:
        f.write(bytes(buf))
    print(f"parts/raw-36mb.img.gz rewritten (backup: raw-36mb.img.gz.pre-pollpatch)")

# (b) any built image(s) passed on argv
for img in sys.argv[1:]:
    with open(img, "r+b") as f:
        head = bytearray(f.read(20 * 1024 * 1024))
        if patch_buf(head, os.path.basename(img)):
            f.seek(0)
            f.write(head)
            print(f"{img} patched in place")
