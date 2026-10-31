#include <loader.h>
#include <win32.h>
#include <console.h>
#include <string.h>
#include <mm.h>

/* Shared helpers for the image loaders. */

int loader_map_segment(struct process *p, u64 vaddr, const u8 *data, u32 filesz,
                       u32 memsz, int writable, int executable)
{
    u64 flags = PAGE_PRESENT | PAGE_USER | (writable ? PAGE_WRITE : 0);
    if (!executable) flags |= PAGE_NX;

    u64 start = ALIGN_DOWN(vaddr, PAGE_SIZE);
    u64 end = ALIGN_UP(vaddr + (memsz ? memsz : filesz), PAGE_SIZE);

    for (u64 va = start; va < end; va += PAGE_SIZE) {
        u64 phys = pmm_alloc_page();
        if (!phys) return -1;
        if (vmm_space_map(p->space, va, phys, flags) != 0) {
            pmm_free_page(phys);
            return -1;
        }
    }

    /* Copy the file-backed part, honouring page alignment. */
    if (data && filesz) {
        u64 copied = 0;
        while (copied < filesz) {
            u64 va = vaddr + copied;
            u64 page = ALIGN_DOWN(va, PAGE_SIZE);
            u64 off = va - page;
            u64 chunk = PAGE_SIZE - off;
            if (chunk > filesz - copied) chunk = filesz - copied;
            u64 phys = vmm_space_get_phys(p->space, page);
            if (!phys) return -1;
            memcpy((u8 *)phys_to_virt(phys & PTE_ADDR_MASK) + off, data + copied, chunk);
            copied += chunk;
        }
    }

    /* Zero the zero-fill tail (memsz > filesz). The PMM does not clear pages,
       so without this .bss holds whatever the previous owner left behind and a
       C runtime reading an unset function pointer jumps into garbage. */
    if (memsz > filesz) {
        u64 pos = vaddr + filesz;
        u64 end = vaddr + memsz;
        while (pos < end) {
            u64 page = ALIGN_DOWN(pos, PAGE_SIZE);
            u64 off = pos - page;
            u64 chunk = PAGE_SIZE - off;
            if (chunk > end - pos) chunk = end - pos;
            u64 phys = vmm_space_get_phys(p->space, page);
            if (!phys) return -1;
            memset((u8 *)phys_to_virt(phys & PTE_ADDR_MASK) + off, 0, chunk);
            pos += chunk;
        }
    }
    return 0;
}

/* Read/write a 64-bit word in the target address space, resolving the page as
   we go. Relocation slots are spread across mapped segments. */
static u64 elf_read_u64(struct process *p, u64 vaddr)
{
    u64 phys = vmm_space_get_phys(p->space, vaddr);
    if (!phys) return 0;
    return *(const u64 *)((u8 *)phys_to_virt(phys & PTE_ADDR_MASK) +
                          (vaddr & (PAGE_SIZE - 1)));
}

static void elf_write_u64(struct process *p, u64 vaddr, u64 value)
{
    u64 phys = vmm_space_get_phys(p->space, vaddr);
    if (!phys) return;
    *(u64 *)((u8 *)phys_to_virt(phys & PTE_ADDR_MASK) +
             (vaddr & (PAGE_SIZE - 1))) = value;
}

/* ---------------- ELF64 ---------------- */

struct elf64_hdr {
    u8  ident[16];
    u16 type, machine;
    u32 version;
    u64 entry;
    u64 phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

static int elf_apply_relative_relocs(struct process *p, const u8 *image, u32 size,
                                     const struct elf64_hdr *eh, u64 bias);

#define PT_LOAD    1
#define PT_INTERP  3
#define PT_DYNAMIC 2
#define PF_X 1
#define PF_W 2
#define PF_R 4

int loader_elf(struct process *p, const u8 *image, u32 size)
{
    if (size < sizeof(struct elf64_hdr)) return -1;
    const struct elf64_hdr *eh = (const struct elf64_hdr *)image;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F') return -1;
    if (eh->ident[4] != 2 /* ELFCLASS64 */ || eh->machine != 62 /* x86-64 */) return -1;
    if (eh->type != 2 /* ET_EXEC */ && eh->type != 3 /* ET_DYN */) {
        klog_warn("ELF: unsupported object type %u", eh->type);
        return -1;
    }
    int is_dyn = (eh->type == 3);

    /* A dynamically linked executable needs an interpreter we do not have.
       ET_DYN without PT_INTERP is a static PIE and is handled below. */
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(image + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type == PT_INTERP) {
            klog_err("ELF: %s is dynamically linked (needs PT_INTERP); "
                     "build a static binary", p->name);
            return -1;
        }
    }

    /* The load base is the address file offset 0 is mapped at. AT_PHDR must be
       base + e_phoff, not the raw file offset, or a PIE startup computes a
       bogus load bias from the wrong "program headers". */
    u64 vbase = (u64)-1;
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(image + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        u64 v = ALIGN_DOWN(ph->vaddr, PAGE_SIZE);
        if (v < vbase) vbase = v;
    }
    if (vbase == (u64)-1) return -1;

    /* ET_EXEC is placed where its p_vaddr says. ET_DYN has no fixed address,
       so pick a base in the mmap region and add it to every virtual address. */
    u64 bias = 0;
    if (is_dyn) {
        /* Total span of the loadable segments decides how much room to reserve. */
        u64 top = 0;
        for (int i = 0; i < eh->phnum; i++) {
            const struct elf64_phdr *ph =
                (const struct elf64_phdr *)(image + eh->phoff + (u64)i * eh->phentsize);
            if (ph->type != PT_LOAD) continue;
            u64 e = ALIGN_UP(ph->vaddr + ph->memsz, PAGE_SIZE);
            if (e > top) top = e;
        }
        u64 base = process_reserve_image(p, vbase, top - vbase);
        if (!base) {
            klog_err("ELF: no room to place the PIE image");
            return -1;
        }
        bias = base - vbase;
    }

    int loaded = 0;
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(image + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        if (ph->offset + ph->filesz > size) {
            klog_err("ELF: segment %d extends past the file", i);
            return -1;
        }
        if (loader_map_segment(p, bias + ph->vaddr, image + ph->offset,
                               (u32)ph->filesz, (u32)ph->memsz,
                               (ph->flags & PF_W) != 0,
                               (ph->flags & PF_X) != 0) != 0)
            return -1;
        loaded++;
    }
    if (!loaded) return -1;

    if (is_dyn && elf_apply_relative_relocs(p, image, size, eh, bias) != 0) {
        klog_err("ELF: %s could not be relocated", p->name);
        return -1;
    }

    p->entry = bias + eh->entry;
    p->base  = bias + vbase;
    p->phdr  = bias + vbase + eh->phoff;
    p->phnum = eh->phnum;
    p->kind = PROC_KIND_LINUX;
    klog_info("ELF", "loaded %s (%s), entry %p, %d segment(s)", p->name,
              is_dyn ? "PIE" : "exec", (void *)p->entry, loaded);
    return 0;
}

/* Apply R_X86_64_RELATIVE entries from the PT_DYNAMIC segment. This is the
   whole relocation story a static PIE needs: every slot is set to bias+addend,
   with no symbol lookup. */
static int elf_apply_relative_relocs(struct process *p, const u8 *image, u32 size,
                                     const struct elf64_hdr *eh, u64 bias)
{
    const u8 *dyn = NULL;
    u64 dynsz = 0;
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(image + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type == PT_DYNAMIC) { dyn = image + ph->offset; dynsz = ph->filesz; }
    }
    if (!dyn) return 0;                       /* nothing to relocate */

    u64 rela = 0, relasz = 0, relaent = 24, relr = 0, relrsz = 0, relrent = 8;
    for (u64 o = 0; o + 16 <= dynsz; o += 16) {
        u64 tag = *(const u64 *)(dyn + o);
        u64 val = *(const u64 *)(dyn + o + 8);
        if (tag == 0) break;                  /* DT_NULL */
        switch (tag) {
        case 7:  rela = val; break;           /* DT_RELA    */
        case 8:  relasz = val; break;         /* DT_RELASZ  */
        case 9:  relaent = val; break;        /* DT_RELAENT */
        case 35: relr = val; break;           /* DT_RELR    */
        case 36: relrsz = val; break;         /* DT_RELRSZ  */
        case 37: relrent = val; break;        /* DT_RELRENT */
        default: break;
        }
    }
    if (relaent == 0) relaent = 24;

    /* DT_RELA is a file offset for a PIE; add the bias to reach the slot. */
    for (u64 o = 0; rela && o + relaent <= relasz; o += relaent) {
        const u64 *e = (const u64 *)(image + rela + o);
        u64 r_offset = e[0];
        u64 r_info   = e[1];
        u64 r_addend = e[2];
        if ((r_info & 0xFFFFFFFFu) != 8 /* R_X86_64_RELATIVE */) continue;
        elf_write_u64(p, bias + r_offset, bias + r_addend);
    }

    /* DT_RELR: a bitmap of relative slots, 8 bytes per word. */
    if (relr && relrsz) {
        u64 where = 0;
        for (u64 o = 0; o + relrent <= relrsz; o += relrent) {
            u64 entry = *(const u64 *)(image + relr + o);
            if ((entry & 1) == 0) {
                where = entry;
                elf_write_u64(p, bias + where, bias + elf_read_u64(p, bias + where));
                where += 8;
            } else {
                for (int b = 1; b < 64; b++)
                    if (entry & (1ULL << b))
                        elf_write_u64(p, bias + where + (b - 1) * 8,
                                      bias + elf_read_u64(p, bias + where + (b - 1) * 8));
                where += 63 * 8;
            }
        }
    }
    return 0;
}

/* ---------------- PE32+ (Windows .exe) ---------------- */

struct pe_dos_hdr {
    u16 magic;          /* 'MZ' */
    u8  pad[58];
    u32 lfanew;
} __attribute__((packed));

struct pe_coff_hdr {
    u16 machine;
    u16 nsections;
    u32 timestamp;
    u32 symtab;
    u32 nsymbols;
    u16 opt_size;
    u16 characteristics;
} __attribute__((packed));

struct pe_opt64 {
    u16 magic;          /* 0x20B = PE32+ */
    u8  linker[2];
    u32 code_size, init_size, uninit_size;
    u32 entry_rva, base_of_code;
    u64 image_base;
    u32 sect_align, file_align;
    u16 os_major, os_minor, img_major, img_minor, subsys_major, subsys_minor;
    u32 win32ver;
    u32 image_size, headers_size, checksum;
    u16 subsystem, dll_characteristics;
    u64 stack_reserve, stack_commit, heap_reserve, heap_commit;
    u32 loader_flags, num_dirs;
} __attribute__((packed));

struct pe_section {
    char name[8];
    u32 vsize, vaddr, rawsize, rawptr;
    u32 reloc_ptr, line_ptr;
    u16 nreloc, nline;
    u32 characteristics;
} __attribute__((packed));

#define PE_SECT_READ   0x40000000u
#define PE_SECT_WRITE  0x80000000u
#define PE_SECT_EXEC   0x20000000u
#define PE_DIR_BASERELOC 5

/* ---------------- PE imports ---------------- */

static u8 *pe_rva_ptr(const u8 *image, u32 size, const struct pe_section *secs,
                      int nsec, u32 rva)
{
    for (int i = 0; i < nsec; i++) {
        const struct pe_section *s = &secs[i];
        if (rva >= s->vaddr && rva < s->vaddr + (s->vsize ? s->vsize : s->rawsize)) {
            u32 off = s->rawptr + (rva - s->vaddr);
            return off < size ? (u8 *)image + off : NULL;
        }
    }
    return NULL;
}

static int pe_write_u64(struct process *p, u64 va, u64 value)
{
    u64 phys = vmm_space_get_phys(p->space, va);
    if (!phys) return -1;
    u64 *slot = (u64 *)((u8 *)phys_to_virt(phys & PAGE_MASK) + (va & (PAGE_SIZE - 1)));
    *slot = value;
    return 0;
}

/* Returns how many imports the image declares. A PE with none is a
   freestanding program that talks to the kernel over int 0x80 itself, so it
   keeps the native ABI; only an image that imports KERNEL32/msvcrt gets the
   Win32 personality. */
static int pe_bind_imports(struct process *p, const u8 *image, u32 size, u64 base,
                           const struct pe_section *secs, int nsec,
                           const struct pe_opt64 *opt)
{
    if (opt->num_dirs < 2) return 0;
    const u32 *dd = (const u32 *)((const u8 *)opt + sizeof(struct pe_opt64));
    u32 imp_rva = dd[1 * 2], imp_sz = dd[1 * 2 + 1];
    if (!imp_rva || !imp_sz) return 0;

    u8 *desc = pe_rva_ptr(image, size, secs, nsec, imp_rva);
    if (!desc) return 0;

    /* The stubs live in a freshly mapped user page, so the personality has to
       be initialised before the IAT can point at them. */
    win32_init();

    int bound = 0, total = 0;
    for (u32 off = 0; off + 20 <= imp_sz; off += 20) {
        u32 oft = *(const u32 *)(desc + off + 0);
        u32 name_rva = *(const u32 *)(desc + off + 12);
        u32 iat = *(const u32 *)(desc + off + 16);
        if (oft == 0 && name_rva == 0 && iat == 0) break;

        u32 thunk_rva = oft ? oft : iat;
        const char *dll = name_rva ? (const char *)pe_rva_ptr(image, size, secs, nsec, name_rva) : NULL;
        if (!dll) continue;

        for (u32 i = 0;; i++) {
            u8 *th = pe_rva_ptr(image, size, secs, nsec, thunk_rva + i * 8);
            if (!th) break;
            u64 ent = *(const u64 *)th;
            if (ent == 0) break;
            total++;
            const char *sym = NULL;
            if (!(ent >> 63)) {
                u8 *nm = pe_rva_ptr(image, size, secs, nsec, (u32)ent);
                if (nm) sym = (const char *)(nm + 2);      /* skip the 2-byte hint */
            }
            u64 stub = sym ? win32_bind_import(sym) : 0;
            if (stub) { pe_write_u64(p, base + iat + i * 8, stub); bound++; }
        }
    }
    klog_info("PE", "imports: %d of %d bound to kernel stubs", bound, total);
    return total;
}

int loader_pe(struct process *p, const u8 *image, u32 size)
{
    if (size < sizeof(struct pe_dos_hdr)) return -1;
    const struct pe_dos_hdr *dos = (const struct pe_dos_hdr *)image;
    if (dos->magic != 0x5A4D) return -1;            /* 'MZ' */
    if (dos->lfanew + sizeof(u32) > size) return -1;
    if (memcmp(image + dos->lfanew, "PE\0\0", 4) != 0) return -1;

    u32 peoff = dos->lfanew + 4;
    const struct pe_coff_hdr *coff = (const struct pe_coff_hdr *)(image + peoff);
    if (coff->machine != 0x8664) {                  /* AMD64 */
        klog_err("PE: only x86-64 images are supported (machine=0x%04x)", coff->machine);
        return -1;
    }
    const struct pe_opt64 *opt =
        (const struct pe_opt64 *)(image + peoff + sizeof(struct pe_coff_hdr));
    if (opt->magic != 0x20B) {
        klog_err("PE: only PE32+ (64-bit) images are supported");
        return -1;
    }

    u64 base = opt->image_base;
    /* We always map at the image's preferred base, so the relocation delta is
       zero. Adding `base` to a relocation slot would double-count the address
       the linker already baked in (that is exactly what broke the mingw CRT:
       a refptr holding 0x14000c090 became 0x28000c090). */
    i64 reloc_delta = (i64)base - (i64)opt->image_base;
    u32 secoff = peoff + sizeof(struct pe_coff_hdr) + coff->opt_size;
    if (secoff + (u32)coff->nsections * sizeof(struct pe_section) > size) return -1;

    const struct pe_section *secs = (const struct pe_section *)(image + secoff);

    /* Headers first so relocations can patch anything in the image. */
    if (loader_map_segment(p, base, image, opt->headers_size, opt->headers_size,
                           0, 0) != 0) return -1;

    for (int i = 0; i < coff->nsections; i++) {
        const struct pe_section *s = &secs[i];
        if (!(s->characteristics & PE_SECT_READ) && s->vsize == 0) continue;
        u32 rawsz = s->rawsize;
        if (s->rawptr + rawsz > size) rawsz = 0;    /* truncated / zero-filled */
        u64 va = base + s->vaddr;
        u32 memsz = s->vsize ? s->vsize : rawsz;
        if (loader_map_segment(p, va, rawsz ? image + s->rawptr : NULL, rawsz, memsz,
                               (s->characteristics & PE_SECT_WRITE) != 0,
                               (s->characteristics & PE_SECT_EXEC) != 0) != 0)
            return -1;
    }

    /* Apply base relocations so the image can run at its preferred base. */
    u32 ndirs = opt->num_dirs;
    if (ndirs > PE_DIR_BASERELOC) {
        /* The data directory array begins immediately after the optional
           header (offset 112 in PE32+). */
        const u32 *dd = (const u32 *)((const u8 *)opt + sizeof(struct pe_opt64));
        u32 reloc_rva = dd[PE_DIR_BASERELOC * 2];
        u32 reloc_sz  = dd[PE_DIR_BASERELOC * 2 + 1];
        if (reloc_rva && reloc_sz) {
            /* Locate the .reloc block in file terms via its section. */
            const u8 *rel = NULL;
            for (int i = 0; i < coff->nsections; i++) {
                const struct pe_section *s = &secs[i];
                if (reloc_rva >= s->vaddr && reloc_rva < s->vaddr + s->vsize) {
                    rel = image + s->rawptr + (reloc_rva - s->vaddr);
                    break;
                }
            }
            if (rel) {
                u32 done = 0;
                while (done + 8 <= reloc_sz) {
                    u32 page_rva = *(const u32 *)(rel + done);
                    u32 block_sz = *(const u32 *)(rel + done + 4);
                    if (block_sz < 8) break;
                    for (u32 o = 8; o + 2 <= block_sz; o += 2) {
                        u16 entry = *(const u16 *)(rel + done + o);
                        u16 type = entry >> 12;
                        u16 off = entry & 0x0FFF;
                        if (type == 0) continue;         /* ABSOLUTE: padding */
                        if (type != 10) {                /* DIR64 */
                            klog_warn("PE: unsupported relocation type %u", type);
                            continue;
                        }
                        u64 target = base + page_rva + off;
                        u64 phys = vmm_space_get_phys(p->space, target);
                        if (!phys) continue;
                        u64 *slot = (u64 *)((u8 *)phys_to_virt(phys & PAGE_MASK) +
                                            (target & (PAGE_SIZE - 1)));
                        *slot += (u64)reloc_delta;
                    }
                    done += block_sz;
                }
            }
        }
    }

    /* Bind the import address table. Every Windows API call goes through the
       IAT, so this is the only place the kernel gets to interpose. */
    int imports = pe_bind_imports(p, image, size, base, secs, coff->nsections, opt);

    p->entry = base + opt->entry_rva;
    p->kind = imports ? PROC_KIND_WIN32 : PROC_KIND_NATIVE;
    klog_info("PE", "loaded %s, image base %p, entry %p, %u section(s)", p->name,
              (void *)base, (void *)p->entry, coff->nsections);
    return 0;
}

/* ---------------- LASH: the native NiteKernel container ---------------- */

/* Two shapes share the .lash extension:
   - a text script (optionally with a #! line) run by the built-in interpreter;
   - a binary container with the LASHBIN magic, mapped and run in ring 3. */

#define LASH_MAGIC "LASHBIN\0"

struct lash_hdr {
    char magic[8];
    u32 version;
    u32 flags;
    u32 entry_off;      /* entry point, relative to load_addr */
    u32 code_size;
    u32 data_size;
    u64 load_addr;
    u32 checksum;       /* FNV-1a over the code blob */
    u32 reserved;
} __attribute__((packed));

int loader_lash(struct process *p, const u8 *image, u32 size)
{
    if (size < sizeof(struct lash_hdr)) return -1;
    const struct lash_hdr *h = (const struct lash_hdr *)image;
    if (memcmp(h->magic, LASH_MAGIC, 8) != 0) return -1;
    if (h->version != 1) {
        klog_err("LASH: unsupported container version %u", h->version);
        return -1;
    }
    u32 total = h->code_size + h->data_size;
    if (sizeof(*h) + total > size) {
        klog_err("LASH: truncated container");
        return -1;
    }

    const u8 *code = image + sizeof(*h);
    u32 sum = 2166136261u;
    for (u32 i = 0; i < h->code_size; i++) { sum ^= code[i]; sum *= 16777619u; }
    if (sum != h->checksum) {
        klog_err("LASH: checksum mismatch (expected %08x, got %08x)", h->checksum, sum);
        return -1;
    }

    if (loader_map_segment(p, h->load_addr, code, h->code_size, h->code_size,
                           0, 1) != 0)
        return -1;
    if (h->data_size &&
        loader_map_segment(p, h->load_addr + ALIGN_UP(h->code_size, PAGE_SIZE),
                           code + h->code_size, h->data_size, h->data_size,
                           1, 0) != 0)
        return -1;

    p->entry = h->load_addr + h->entry_off;
    p->kind = PROC_KIND_NATIVE;
    klog_info("LASH", "loaded %s, entry %p, code %u bytes", p->name,
              (void *)p->entry, h->code_size);
    return 0;
}

/* ---------------- AppImage ---------------- */

/* A type-2 AppImage is an ELF runtime followed by a squashfs payload; the
   runtime finds the payload by reading its own /proc/self/exe size. Without
   that filesystem we run the embedded runtime ELF directly and pass it the
   payload offset through argv, which is what the loader would have done. */
#define APPIMAGE_MAGIC_OFF 8
static const u8 appimage_magic[3] = { 'A', 'I', 0x02 };

int appimage_payload_offset(const u8 *image, u32 size, u32 *out_off, u32 *out_len);

int appimage_payload_offset(const u8 *image, u32 size, u32 *out_off, u32 *out_len)
{
    if (size < 16) return -1;
    if (memcmp(image + APPIMAGE_MAGIC_OFF, appimage_magic, 3) != 0) return -1;
    /* Bytes 8..10 are the magic; the ELF header sits at offset 0 and the
       payload begins at the first 4-byte aligned offset past the ELF. */
    const struct elf64_hdr *eh = (const struct elf64_hdr *)image;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E') return -1;
    u64 end = eh->phoff + (u64)eh->phnum * eh->phentsize;
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(image + eh->phoff + (u64)i * eh->phentsize);
        if (ph->offset + ph->filesz > end) end = ph->offset + ph->filesz;
    }
    u32 off = (u32)ALIGN_UP(end, 4096);
    if (off >= size) return -1;
    *out_off = off;
    *out_len = size - off;
    return 0;
}

/* ---------------- dispatch ---------------- */

int image_detect(const u8 *image, u32 size)
{
    if (size >= 4 && image[0] == 0x7F && image[1] == 'E' &&
        image[2] == 'L' && image[3] == 'F') {
        if (size >= 12 && memcmp(image + APPIMAGE_MAGIC_OFF, appimage_magic, 3) == 0)
            return IMG_APPIMAGE;
        return IMG_ELF;
    }
    if (size >= 2 && image[0] == 'M' && image[1] == 'Z') return IMG_PE;
    if (size >= 8 && memcmp(image, LASH_MAGIC, 8) == 0) return IMG_LASH_BIN;
    if (size >= 2 && image[0] == '#' && image[1] == '!') return IMG_SCRIPT;
    /* Heuristic: printable text is treated as a script. */
    u32 probe = size < 256 ? size : 256;
    int printable = 1;
    for (u32 i = 0; i < probe; i++) {
        u8 c = image[i];
        if (c == '\t' || c == '\n' || c == '\r') continue;
        if (c < 0x20 || c > 0x7E) { printable = 0; break; }
    }
    if (printable && probe > 0) return IMG_SCRIPT;
    return IMG_UNKNOWN;
}

int loader_load(struct process *p, const u8 *image, u32 size)
{
    switch (image_detect(image, size)) {
    case IMG_ELF:
    case IMG_APPIMAGE:
        /* The AppImage runtime is itself an ELF we can run. */
        return loader_elf(p, image, size);
    case IMG_PE:
        return loader_pe(p, image, size);
    case IMG_LASH_BIN:
        return loader_lash(p, image, size);
    case IMG_SCRIPT:
        klog_err("process: %s is a script; run it from the shell", p->name);
        return -1;
    default:
        klog_err("process: unrecognised image format for %s", p->name);
        return -1;
    }
}
