# NiteKernel

A from-scratch x86-64 bare-metal kernel. It boots under GRUB via Multiboot2,
brings up its own memory management, interrupts, drivers and networking, and
can drop to ring 3 to run small executables.

Everything below was observed on a real boot. The **Status** section is
explicit about what does *not* work yet, and it is worth reading first: the
kernel is a working system, but its executable-compatibility story is much
narrower than "it runs ELF and PE".

## Status

### Works, verified in QEMU

* Boots via Multiboot2 to an interactive serial shell
* GDT/TSS, IDT, 8259 PIC remap, PIT at 100 Hz
* PMM bitmap allocator, paging, per-process address spaces, kernel heap
* CPUID, PCI enumeration, RTC, ATA/ATAPI, PS/2 keyboard
* Bochs VBE linear framebuffer with a rendered text console
* e1000 NIC: ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS
* HTTP/1.0 GET over the real internet, with SHA-256 of the response
* `NEOFS` on-disk filesystem: mount, list, read, write, delete
* First-boot driver acquisition: PCI IDs matched against a driver store, the
  vendor packages fetched and hash-verified
* `report` prints a full PC component report
* `lash` shell: pipes, `>` / `>>`, `&&` / `||`, `$?` / `$$`
* Runs an unmodified static GNU `bash` as the interactive shell when
  `bash` is present on the NEOFS volume (optional input; see Building)
* `autorun.lash` executes by itself at boot

### Executable formats

All five were exercised on a boot with binaries from normal toolchains, not
with purpose-built test programs.

| Format | Program | Result |
| --- | --- | --- |
| `.lash` | `autorun.lash` | runs unattended at boot, before the shell |
| Static ELF64 `ET_EXEC` | glibc static, 758 KB | runs, prints, exits 0 |
| Static PIE `ET_DYN` | `pie`, 795 KB | runs, relocated to its base |
| Linux AppImage (type 2) | `hello.appimage` | detected, runtime extracted and run |
| Windows PE32+ `.exe` | `mingw -O2 -static`, 244 KB | runs, `printf` works, returns 7 |

Observed:

```
[autorun] executing autorun.lash
[autorun.lash] NiteKernel booted, auto-executing .lash
[shell] starting GNU bash; type 'exit' for the kernel shell
bash-5.2#

nite:10.0.2.15> run real_static
REAL STATIC OK
nite:10.0.2.15> run pie
static PIE ok: 42
nite:10.0.2.15> run real_win.exe
real mingw PE .exe on NiteKernel
[real_win.exe exited with code 7]
```

### How the formats are supported

* **ELF64** — the loader maps every `PT_LOAD`, applies no relocations for
  `ET_EXEC` and rebases `ET_DYN` images to a free base. The Linux personality
  answers the syscalls a static glibc needs (`brk`, `mmap`, `arch_prctl`,
  `pselect6`, `fstat`, termios).
* **AppImage** — a type-2 AppImage is an ELF runtime with an `AI\x02` marker
  and a squashfs payload. The marker is found, the offset and length are
  checked against the image, and the runtime ELF is executed like any other.
* **Windows PE32+** — the import directory is parsed and every import is bound
  to a trampoline that traps into the kernel, where the KERNEL32/msvcrt
  emulation runs. The program itself stays at ring 3. A TEB is installed at
  `GS:0` for the CRT's stack and TLS queries, and the data imports the CRT
  dereferences are bound to the address of the variable.
* **`.lash`** — a native script the kernel interprets in-process, with no
  shell process in between. `autorun.lash` on the volume is executed on boot.

### Does NOT work

| Case | Result |
| --- | --- |
| Windows GUI subsystem (`.exe` with `WinMain`) | not started; only the console subsystem is wired up |
| Dynamic ELF needing a shared `libc.so` | loader refuses: no dynamic linker, no user-space libc |
| Windows APIs beyond the console CRT surface | unbound imports degrade to a no-op stub that returns 0 |

The Windows personality covers what the mingw console CRT imports. A program
that calls an API outside that set links, runs, and silently gets 0 back; the
API surface is enumerated in `src/kernel/win32.c`.

## Building

The kernel needs `gcc`, `nasm`, `ld`, `grub-mkrescue` (from `grub-pc-bin`,
plus `xorriso` and `mtools` which it shells out to) and `python3`.

```sh
make                              # build/nitekernel.elf, .iso, driver store
python3 tools/build_testapps.py   # test programs and build/testapps/apps.img
make run                          # boot in QEMU
```

`build_testapps.py` additionally needs `x86_64-w64-mingw32-gcc` for the
Windows tests, and `qemu-system-x86_64` to run anything. One input is
optional:

* `tools/testapps/bash` — a static GNU bash (optional). If present, it is
  included in the NEOFS image so the kernel can run an unmodified shell.
  Fetch it with:
  `apt-get download bash-static && dpkg-deb -x bash-static_*.deb /tmp/b`
  and copy `/tmp/b/usr/bin/bash-static` to `tools/testapps/bash`.

The AppImage test (`hello.appimage`) is always built from the ELF test program
by patching the ELF header with the `AI\x02` type-2 marker.

To exercise the loaders, attach the NEOFS image:

```sh
qemu-system-x86_64 -cdrom build/nitekernel.iso -m 512M -display none \
    -serial stdio -drive file=build/testapps/apps.img,format=raw,if=ide
```

Then, from the shell:

```
ls
run hello_elf
run hello_pe.exe
run hello.lash
run real_static
run pie
run real_win.exe
run hello.appimage
report
```

## Shell commands

```
help              this list
report            full PC component report
cpu               CPU details
mem               memory usage
pci               enumerate the PCI bus
gfx               graphics/framebuffer info
rtc               current date and time
ata               ATA/ATAPI devices
lsblk             block devices
net               network status
ifconfig <ip>/<mask> [gw] [dns]   set a static address
dhcp              request an address via DHCP
ping <host|ip>    ICMP echo
dns <host>        resolve a hostname
fetch <url>       HTTP GET (prints size + sha256)
store             list the driver store
drivers           installed driver database
install           run first-boot driver acquisition
fs                filesystem status
ls                list files on the NEOFS volume
cat <file>        print a file
write <f> <text>  create/overwrite a file
rm <file>         delete a file
uptime            ticks since boot
echo <text>       print text
clear             clear the screen
reboot            reset the machine
halt              stop the CPU
poweroff          stop the CPU (alias for halt)
trace [on|off]    toggle syscall tracing
run <file>        execute an ELF/PE/.lash program
exec <file>       alias for run
lash <file>       run a .lash script
bash / sh / lash  built-in lash interpreter (bash-compatible subset;
                  `bash -c '<cmd>'` runs an inline line)
```

## Layout

```
include/        public headers
src/arch/       GDT, IDT, PIC, PIT, ISRs, ring-3 trampoline
src/boot/       Multiboot2 entry, linker script, GRUB config
src/drivers/    ATA, RTC, e1000, keyboard, Bochs VBE graphics
src/fs/         NEOFS and the driver store
src/hal/        CPUID, PCI, device registry
src/kernel/     kmain, console, syscalls, loaders, shell, lash, processes, win32
src/lib/        string and SHA-256
src/mm/         PMM, VMM, heap
src/net/        ARP/IP/ICMP/UDP/TCP, DHCP, DNS, HTTP
tools/          host-side build helpers and test programs
```

## License

See [LICENSE](LICENSE).
