# NiteKernel

A from-scratch x86-64 bare-metal kernel. It boots under GRUB via Multiboot2,
brings up its own memory management, interrupt handling, drivers and
networking, and drops to ring 3 to run real executables.

## What it runs

| Format | Extension | Path |
| --- | --- | --- |
| ELF64 / AppImage | `.appimage` | ring-3 loader |
| Windows PE32+ | `.exe` | ring-3 loader |
| Native NiteKernel binary | `.lashbin` | `LASHBIN` container |
| Native script | `.lash` | interpreted by `lash` |

The shell is `lash`, a bash-compatible subset that also understands `bash`,
`sh`, `run` and `exec`. It supports pipes, redirection (`>` and `>>`),
`&&` / `||`, and the `$?` and `$$` variables.

## Features

* Multiboot2 boot, GDT/TSS, IDT, 8259 PIC remap, PIT at 100 Hz
* Physical bitmap allocator, paging, per-process address spaces, kernel heap
* ACPI-free hardware discovery: CPUID, PCI enumeration, RTC, ATA/ATAPI
* Bochs VBE linear framebuffer with a rendered text console
* e1000 NIC, ARP/IPv4/ICMP/UDP/TCP, DHCP, DNS, HTTP client with SHA-256
* `NEOFS`, a small on-disk filesystem (512-byte blocks, FNV-1a checksummed
  superblock)
* First-boot driver acquisition: PCI IDs are matched against a driver store
  and the vendor packages (Mesa DRI/GLX) are fetched and hash-verified
* A `report` command that prints a full PC component report

## Building

Requires `gcc`, `nasm`, `ld`, `grub-mkrescue` and `python3`.

```sh
make            # build/nitekernel.elf, build/nitekernel.iso, driver store
make run        # boot in QEMU with the serial console
python3 tools/build_testapps.py   # build the test programs and apps.img
```

`make run` boots `build/nitekernel.iso`. Attach a NEOFS disk image to try the
loaders:

```sh
qemu-system-x86_64 -cdrom build/nitekernel.iso -m 512M -display none \
    -serial stdio -drive file=build/testapps/apps.img,format=raw,if=ide
```

Then, from the shell:

```
run hello_elf
run hello_pe.exe
run hello.lashbin
run hello.lash
report
```

## Layout

```
include/        public headers
src/arch/       GDT, IDT, PIC, PIT, ISRs, ring-3 trampoline
src/boot/       Multiboot2 entry, linker script, GRUB config
src/drivers/    ATA, RTC, e1000, keyboard, Bochs VBE graphics
src/fs/         NEOFS and the driver store
src/hal/        CPUID, PCI, device registry
src/kernel/     kmain, console, syscalls, loaders, shell, lash, processes
src/lib/        string and SHA-256
src/mm/         PMM, VMM, heap
src/net/        ARP/IP/ICMP/UDP/TCP, DHCP, DNS, HTTP
tools/          host-side build helpers and test programs
```

## License

See [LICENSE](LICENSE).
