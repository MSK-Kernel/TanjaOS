#!/usr/bin/env python3
"""wadstrip.py IN.wad OUT.wad

Rewrites an IWAD without its sound and music lumps.  TanjaOS has no audio
driver, so DS*/DP* (sound effects, PC speaker) and D_* (music) are dead
weight - for Freedoom this shrinks the image from ~29 MB to a few MB.
The lump directory is kept intact (stripped lumps become empty), so every
lump number and name lookup in the engine still works.
"""
import struct
import sys

def is_audio(name):
    n = name.upper()
    return n.startswith(("DS", "DP", "D_"))

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    ident, numlumps, infotableofs = struct.unpack_from("<4sii", data, 0)
    if ident not in (b"IWAD", b"PWAD"):
        sys.exit("not a WAD file")

    lumps = []
    for i in range(numlumps):
        pos, size, raw = struct.unpack_from("<ii8s", data, infotableofs + 16 * i)
        name = raw.split(b"\0", 1)[0].decode("ascii", "replace")
        lumps.append((name, raw, data[pos:pos + size] if size > 0 else b""))

    out = bytearray(b"\0" * 12)
    directory = []
    kept = dropped = 0
    for name, raw, blob in lumps:
        if blob and is_audio(name):
            blob = b""
            dropped += 1
        else:
            kept += 1
        pos = len(out) if blob else 0
        directory.append((pos, len(blob), raw))
        out += blob
        while len(out) % 4:
            out += b"\0"

    infotable = len(out)
    for pos, size, raw in directory:
        out += struct.pack("<ii8s", pos, size, raw)
    out[0:12] = struct.pack("<4sii", ident, numlumps, infotable)
    open(sys.argv[2], "wb").write(out)
    print(f"wadstrip: {len(data)/1e6:.1f} MB -> {len(out)/1e6:.1f} MB "
          f"({dropped} audio lumps dropped, {kept} kept)")

main()
