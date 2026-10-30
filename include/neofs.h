#ifndef NEO_NEOFS_H
#define NEO_NEOFS_H

#include <nitekernel.h>
#include <blockdev.h>

#define NEOFS_MAGIC       "NEOFS1"
#define NEOFS_BLOCK_SIZE  512
#define NEOFS_MAX_FILES   32
#define NEOFS_NAME_MAX    24
#define NEOFS_MAX_FILE_SIZE (2 * 1024 * 1024)

struct neofs_entry {
    char name[NEOFS_NAME_MAX];
    u32  size;
    u32  first_block;
};

struct neofs_super {
    char magic[8];
    u32  version;
    u32  block_size;
    u32  total_blocks;
    u32  dir_block;
    u32  data_start;
    u32  next_free_block;
    u32  file_count;
    u32  checksum;
};

/* Returns 0 on success. `bd` is retained as the backing device. */
int  neofs_format(const struct block_device *bd);
int  neofs_mount(const struct block_device *bd);
int  neofs_mounted(void);
const char *neofs_device_name(void);

int  neofs_write_file(const char *name, const void *data, u32 size);
int  neofs_read_file(const char *name, void *buf, u32 max, u32 *out_size);
int  neofs_delete_file(const char *name);
int  neofs_file_exists(const char *name);
/* Size in bytes, or -1 if the file does not exist. */
long neofs_file_size(const char *name);

typedef void (*neofs_list_cb)(const struct neofs_entry *e, void *ctx);
void neofs_list(neofs_list_cb cb, void *ctx);
int  neofs_file_count(void);

/* Directory block index of the first entry (for tests). */
u32  neofs_checksum(const struct neofs_super *sb);

#endif
