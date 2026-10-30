#ifndef NEO_LOADER_H
#define NEO_LOADER_H

#include <nitekernel.h>
#include <process.h>

/* Format detection and loading. `loader_load` sniffs the image and dispatches
   to the right backend; the backends map the image into p->space and set
   p->entry. Return 0 on success. */

int loader_load(struct process *p, const u8 *image, u32 size);

/* Backends (each returns 0 if it recognises and maps the image). */
int loader_elf(struct process *p, const u8 *image, u32 size);
int loader_pe(struct process *p, const u8 *image, u32 size);
int loader_lash(struct process *p, const u8 *image, u32 size);

/* Image format classes returned by image_detect(). */
#define IMG_UNKNOWN  (-1)
#define IMG_ELF       0
#define IMG_PE        1
#define IMG_LASH_BIN  2
#define IMG_SCRIPT    3
#define IMG_APPIMAGE  4

/* Detect the image format. Returns an IMG_* constant. */
int image_detect(const u8 *image, u32 size);

/* For a type-2 AppImage, return where the squashfs payload starts. */
int appimage_payload_offset(const u8 *image, u32 size, u32 *out_off, u32 *out_len);

/* Map `len` bytes of the image at `vaddr` with the given permissions and copy
   the file contents in. Used by all backends. */
int loader_map_segment(struct process *p, u64 vaddr, const u8 *data, u32 filesz,
                       u32 memsz, int writable, int executable);

#endif
