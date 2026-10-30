# NiteKernel build system
# Produces build/nitekernel.elf, build/nitekernel.iso, and the host-side driver store.

CC      := gcc
AS      := nasm
LD      := ld
PYTHON  := python3

BUILD   := build
ISO_DIR := $(BUILD)/isodir
OUT     := $(BUILD)/nitekernel.elf
ISO     := $(BUILD)/nitekernel.iso

CFLAGS  := -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-stack-check \
           -fno-lto -fno-pic -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -mcmodel=kernel -Wall -Wextra -Wno-unused-parameter -Wno-address-of-packed-member \
           -Iinclude -Isrc -g -MMD -MP
LDFLAGS := -T src/boot/linker.ld -nostdlib -z max-page-size=0x1000 -z noexecstack

C_SRCS   := $(shell find src -name '*.c') $(BUILD)/drivers/driver_store_gen.c
ASM_SRCS := $(shell find src -name '*.asm')
C_OBJS   := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
ASM_OBJS := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS))
OBJS     := $(ASM_OBJS) $(C_OBJS)

DRIVERS  := $(BUILD)/drivers
DISK     := $(BUILD)/disk.img
STORE_SRC := $(DRIVERS)/driver_store_gen.c

.PHONY: all iso clean run run-vga run-gui drivers disk mirror

all: $(ISO) $(DRIVERS)/store.json

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf64 $< -o $@

$(OUT): $(OBJS) src/boot/linker.ld $(STORE_SRC)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo "  kernel size: $$(stat -c%s $@) bytes"

$(ISO): $(OUT) src/boot/grub.cfg
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(OUT) $(ISO_DIR)/boot/nitekernel.elf
	cp src/boot/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(ISO_DIR) >/dev/null 2>&1
	@echo "  iso size:    $$(stat -c%s $(ISO)) bytes"

# Host-side driver store: index the vendor driver packages under tools/payloads
# so the kernel can match them against the PCI hardware it discovers, then
# download and verify them on first boot.
$(DRIVERS)/store.json $(STORE_SRC): tools/build_store.py $(wildcard tools/payloads/*)
	@mkdir -p $(DRIVERS)
	$(PYTHON) tools/build_store.py $(DRIVERS)

drivers: $(DRIVERS)/store.json

# Blank hard disk for the NEOFS driver database (created once, never wiped).
$(DISK):
	@mkdir -p $(BUILD)
	qemu-img create -f raw $(DISK) 16M >/dev/null
	@echo "  disk image:  $(DISK)"

disk: $(DISK)

# Serve the driver store to the guest (10.0.2.2:8080 under QEMU user net).
mirror: $(DRIVERS)/store.json
	$(PYTHON) tools/mirror_server.py $(DRIVERS) 8080

# Plain serial console (headless verification)
run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -serial stdio -display none \
	  -no-reboot -no-shutdown

# Headless with a NIC and a blank disk: exercises DHCP + driver acquisition.
run-net: $(ISO) $(DISK)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -serial stdio -display none \
	  -drive file=$(DISK),format=raw,if=ide -netdev user,id=n0 -device e1000,netdev=n0 \
	  -no-reboot -no-shutdown

# VGA + serial, no window (screenshot-able via monitor)
run-vga: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -serial stdio -display none -vga std \
	  -no-reboot -no-shutdown

# Full graphical window
run-gui: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -vga std -serial stdio

clean:
	rm -rf $(BUILD)

-include $(C_OBJS:.o=.d)
