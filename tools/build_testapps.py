#!/usr/bin/env python3
"""Build the NiteKernel test images and pack them into a NEOFS disk image.

Produces, in build/testapps/:
  hello_elf        freestanding ELF64
  hello_pe.exe     freestanding PE32+ (Windows)
  hello.lash       lash script
  hello.lashbin    native .lash binary container
  hello.appimage   a type-2 AppImage (ELF runtime + squashfs marker)
  apps.img         16 MiB NEOFS-ready disk containing all of the above

The disk image is formatted by the kernel on first boot, so we only need to
copy the files in with the same on-disk layout neofs.c uses.
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "build", "testapps")
SRC = os.path.join(HERE, "testapps")

BLOCK = 512
DIR_BLOCK = 1
DATA_START = 2
NAME_MAX = 24
MAGIC = b"NEOFS1"


def build_elf():
    subprocess.check_call([
        "gcc", "-nostdlib", "-static", "-no-pie", "-ffreestanding",
        "-fno-stack-protector", "-Wl,-e,_start", "-Wl,--build-id=none",
        "-o", os.path.join(OUT, "hello_elf"), os.path.join(SRC, "hello_elf.c"),
    ])
    return os.path.join(OUT, "hello_elf")


def build_pe():
    out = os.path.join(OUT, "hello_pe.exe")
    subprocess.check_call([
        "x86_64-w64-mingw32-gcc", "-nostdlib", "-ffreestanding",
        "-e", "neo_entry", "-Wl,--subsystem,console",
        "-o", out, os.path.join(SRC, "hello_pe.c"),
    ])
    return out


def build_lashbin():
    """Assemble a tiny native payload and wrap it in the LASHBIN container.

    Code and message live in one contiguous .text blob, so the payload's
    RIP-relative `lea rsi, [rel msg]` resolves at the load address without
    any relocation.
    """
    asm = os.path.join(OUT, "lashbin_payload.asm")
    obj = os.path.join(OUT, "lashbin_payload.o")
    text = os.path.join(OUT, "lashbin_text.bin")
    with open(asm, "w") as f:
        f.write(
            "BITS 64\n"
            "global _start\n"
            "section .text\n"
            "_start:\n"
            "    mov rax, 1\n"          # SYS_WRITE
            "    mov rdi, 1\n"
            "    lea rsi, [rel msg]\n"
            "    mov rdx, msglen\n"
            "    int 0x80\n"
            "    mov rax, 0\n"          # SYS_EXIT
            "    mov rdi, 3\n"
            "    int 0x80\n"
            "    hlt\n"
            "msg: db 'hello from a native .lash binary on NiteKernel', 10\n"
            "msglen equ $ - msg\n"
        )
    subprocess.check_call(["nasm", "-f", "elf64", "-o", obj, asm])
    subprocess.check_call(["objcopy", "-O", "binary", "-j", ".text", obj, text])
    code = open(text, "rb").read()

    checksum = 2166136261
    for b in code:
        checksum ^= b
        checksum = (checksum * 16777619) & 0xFFFFFFFF

    load_addr = 0x400000
    hdr = struct.pack("<8sIIIIIQII", b"LASHBIN\0", 1, 0, 0, len(code),
                      0, load_addr, checksum, 0)
    out = os.path.join(OUT, "hello.lashbin")
    with open(out, "wb") as f:
        f.write(hdr)
        f.write(code)
    return out


def build_appimage():
    """A type-2 AppImage: an ELF runtime with the AI\\x02 magic at offset 8."""
    runtime = build_elf()
    blob = bytearray(open(runtime, "rb").read())
    # Patch the ELF e_ident padding (bytes 8..15) with the AppImage magic, which
    # is exactly where the type-2 format stores it.
    blob[8:11] = b"AI\x02"
    out = os.path.join(OUT, "hello.appimage")
    with open(out, "wb") as f:
        f.write(blob)
    return out


def pack_disk(files):
    """Write a NEOFS volume directly (super + dir + data).

    Layout matches src/fs/neofs.c:
      block 0      struct neofs_super  (8 + 4*8 = 40 bytes, checksum at 36)
      block 1      directory (16 entries of 32 bytes each)
      block 2..    file data, contiguous
      entry        char name[24]; u32 size; u32 first_block;
    """
    total_blocks = 32768
    img = bytearray(total_blocks * BLOCK)

    entries = []
    next_free = DATA_START
    for name, path in files:
        data = open(path, "rb").read()
        nblocks = (len(data) + BLOCK - 1) // BLOCK
        entries.append((name, len(data), next_free, data))
        next_free += nblocks
        if next_free >= total_blocks:
            raise SystemExit("disk too small for %s" % name)

    def fnv1a_super(sb):
        """FNV-1a over the superblock with the 4 checksum bytes skipped
        entirely (not merely zeroed) - matching neofs_checksum() in neofs.c."""
        h = 2166136261
        for i, b in enumerate(sb):
            if 36 <= i < 40:
                continue
            h ^= b
            h = (h * 16777619) & 0xFFFFFFFF
        return h

    sb = struct.pack("<8sIIIIIIII", MAGIC, 1, BLOCK, total_blocks,
                     DIR_BLOCK, DATA_START, next_free, len(entries), 0)
    checksum = fnv1a_super(sb)
    sb = struct.pack("<8sIIIIIIII", MAGIC, 1, BLOCK, total_blocks,
                     DIR_BLOCK, DATA_START, next_free, len(entries), checksum)
    img[0:len(sb)] = sb

    for i, (name, size, first, data) in enumerate(entries):
        off = DIR_BLOCK * BLOCK + i * 32
        nm = name.encode()[:NAME_MAX - 1]
        img[off:off + len(nm)] = nm
        img[off + NAME_MAX:off + NAME_MAX + 4] = struct.pack("<I", size)
        img[off + NAME_MAX + 4:off + NAME_MAX + 8] = struct.pack("<I", first)
        img[first * BLOCK:first * BLOCK + len(data)] = data

    out = os.path.join(OUT, "apps.img")
    with open(out, "wb") as f:
        f.write(img)
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    elf = build_elf()
    pe = build_pe()
    lashbin = build_lashbin()
    appimage = build_appimage()
    script = os.path.join(OUT, "hello.lash")
    subprocess.check_call(["cp", os.path.join(SRC, "hello.lash"), script])

    files = [
        ("hello_elf", elf),
        ("hello_pe.exe", pe),
        ("hello.lash", script),
        ("hello.lashbin", lashbin),
        ("hello.appimage", appimage),
    ]
    disk = pack_disk(files)
    print("built:")
    for name, path in files:
        print("  %-16s %8d bytes" % (name, os.path.getsize(path)))
    print("  %-16s %8d bytes" % ("apps.img", os.path.getsize(disk)))


if __name__ == "__main__":
    main()
