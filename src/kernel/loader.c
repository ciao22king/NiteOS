#include <loader.h>
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
            memcpy((u8 *)phys_to_virt(phys & PAGE_MASK) + off, data + copied, chunk);
            copied += chunk;
        }
    }
    return 0;
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

struct elf64_phdr {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

#define PT_LOAD    1
#define PT_INTERP  3
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
    if (eh->type != 2 /* ET_EXEC */) {
        /* ET_DYN would need relocation; report clearly instead of mis-executing. */
        klog_warn("ELF: only ET_EXEC is supported (type=%u)", eh->type);
        return -1;
    }

    /* A dynamically linked executable needs an interpreter we do not have. */
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(image + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type == PT_INTERP) {
            klog_err("ELF: %s is dynamically linked (needs PT_INTERP); "
                     "build a static binary", p->name);
            return -1;
        }
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
        if (loader_map_segment(p, ph->vaddr, image + ph->offset, (u32)ph->filesz,
                               (u32)ph->memsz, (ph->flags & PF_W) != 0,
                               (ph->flags & PF_X) != 0) != 0)
            return -1;
        loaded++;
    }
    if (!loaded) return -1;

    p->entry = eh->entry;
    p->kind = PROC_KIND_LINUX;
    klog_info("ELF", "loaded %s, entry %p, %d segment(s)", p->name,
              (void *)eh->entry, loaded);
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
                        *slot += base;
                    }
                    done += block_sz;
                }
            }
        }
    }

    p->entry = base + opt->entry_rva;
    p->kind = PROC_KIND_WIN32;
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
