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

Observed output:

```
neo:10.0.2.15> ping 10.0.2.2
PING 10.0.2.2 (10.0.2.2):
  reply from 10.0.2.2          (x4)

neo:10.0.2.15> dns example.com
example.com -> 172.66.147.243

neo:10.0.2.15> fetch http://example.com/
[STORE] GET http://example.com/ -> 172.66.147.243:80/
downloaded 577 bytes
sha256 25ddf2c883e0d1958ea971d279a7e4f0fd446724ee3db7db19dadabd4a62e484

neo:10.0.2.15> write t.txt abc
wrote 4 bytes to t.txt
neo:10.0.2.15> cat t.txt
abc
```

### Does NOT work

These were tested with binaries produced by a normal toolchain, not with
purpose-built test programs.

| Case | Result |
| --- | --- |
| Static glibc ELF, 758 KB | Page fault, `CR2=0x7ffffff00000`, error `0x4` |
| Dynamic ELF / AppImage (`ET_DYN` + `PT_INTERP`) | Loader refuses: `only ET_EXEC is supported (type=3)` |
| mingw PE importing `KERNEL32.dll` | Page fault, `CR2=0x28000c090`, error `0x6` |
| 9 KB static ELF, PE with no imports, `.lash` script | Run correctly |

What that means in practice:

* **AppImages do not run.** Every real AppImage is `ET_DYN` with `PT_INTERP`.
  The loader rejects both. There is no ELF relocation step, no dynamic
  linker, and no user-space libc, so even if `ET_DYN` were accepted the
  program could not resolve its symbols.
* **Real Windows `.exe` files do not run.** The PE loader maps sections and
  applies base relocations, but nothing reads the import directory or
  populates the IAT. Only a PE with zero imports works. There is no Win32
  syscall layer, so an imported `KERNEL32.dll` call could not be serviced
  anyway.
* **Large programs do not run.** The user address space and stack are small;
  the 9 KB test ELF loads 120 bytes of data, while a real one needs a full
  glibc image plus heap. The fault address `0x7ffffff00000` is the top of the
  current user stack mapping.
* **`.lashbin` needs pre-positioned code.** The payload must be linked to run
  at `0x400000`; there is no relocation pass, so a normally compiled
  position-independent object is not usable.

The ring-3 machinery itself — page tables with `PAGE_USER` at every level,
the `int 0x80` gate, `iretq` to ring 3 with a proper `SS` RPL, and a clean
return to the shell — is correct and tested. The gap is the runtime around
it: no syscall personality, no relocation, no libc, no import resolution, no
demand paging.

## Building

Requires `gcc`, `nasm`, `ld`, `grub-mkrescue` and `python3`.

```sh
make                              # build/nitekernel.elf, .iso, driver store
python3 tools/build_testapps.py   # test programs and build/testapps/apps.img
make run                          # boot in QEMU
```

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
run hello.lashbin
run hello.lash
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
