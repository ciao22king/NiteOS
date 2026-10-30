#include <neofs.h>
#include <console.h>
#include <string.h>
#include <mm.h>

#define DIR_BLOCK         1
#define DATA_START        2
#define ENTRIES_PER_BLOCK (NEOFS_BLOCK_SIZE / (int)sizeof(struct neofs_entry))

static const struct block_device *dev;
static struct neofs_super super;
static int mounted;

u32 neofs_checksum(const struct neofs_super *sb)
{
    /* FNV-1a over the superblock with the checksum field treated as zero. */
    const u8 *p = (const u8 *)sb;
    u32 h = 2166136261u;
    usize cs_off = offsetof(struct neofs_super, checksum);
    for (usize i = 0; i < sizeof(*sb); i++) {
        if (i >= cs_off && i < cs_off + sizeof(u32)) continue;
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static int read_block(u32 block, void *buf) { return blockdev_read(dev, block, 1, buf); }
static int write_block(u32 block, const void *buf) { return blockdev_write(dev, block, 1, buf); }

int neofs_format(const struct block_device *bd)
{
    if (!bd) return -1;
    dev = bd;
    memset(&super, 0, sizeof(super));
    memcpy(super.magic, NEOFS_MAGIC, 6);
    super.version = 1;
    super.block_size = NEOFS_BLOCK_SIZE;
    super.total_blocks = bd->sector_count;
    super.dir_block = DIR_BLOCK;
    super.data_start = DATA_START;
    super.next_free_block = DATA_START;
    super.file_count = 0;
    super.checksum = neofs_checksum(&super);

    /* Block 0 is a full sector; the superblock is only its first 40 bytes, so
       write through a sector-sized buffer rather than over-reading the struct. */
    u8 sblock[NEOFS_BLOCK_SIZE];
    memset(sblock, 0, sizeof(sblock));
    memcpy(sblock, &super, sizeof(super));
    if (write_block(0, sblock) != 0) return -1;

    u8 dir[NEOFS_BLOCK_SIZE];
    memset(dir, 0, sizeof(dir));
    if (write_block(DIR_BLOCK, dir) != 0) return -1;

    mounted = 1;
    return 0;
}

int neofs_mount(const struct block_device *bd)
{
    if (!bd) return -1;
    dev = bd;
    /* Read the whole sector first: reading 512 bytes straight into the 40-byte
       super struct would overwrite whatever follows it in .bss. */
    u8 sblock[NEOFS_BLOCK_SIZE];
    if (read_block(0, sblock) != 0) return -1;
    memcpy(&super, sblock, sizeof(super));
    if (memcmp(super.magic, NEOFS_MAGIC, 6) != 0) return -1;
    if (super.block_size != NEOFS_BLOCK_SIZE) return -1;
    if (super.checksum != neofs_checksum(&super)) {
        klog_warn("NEOFS: superblock checksum mismatch");
        return -1;
    }
    mounted = 1;
    return 0;
}

int neofs_mounted(void) { return mounted; }
const char *neofs_device_name(void) { return dev ? dev->name : NULL; }

static int find_by_name(const char *name, struct neofs_entry *out)
{
    u8 dir[NEOFS_BLOCK_SIZE];
    if (read_block(super.dir_block, dir) != 0) return -1;
    struct neofs_entry *entries = (struct neofs_entry *)dir;
    for (int i = 0; i < ENTRIES_PER_BLOCK; i++) {
        if (entries[i].first_block == 0) continue;
        if (strcmp(entries[i].name, name) == 0) {
            if (out) *out = entries[i];
            return i;
        }
    }
    return -1;
}

int neofs_file_exists(const char *name) { return find_by_name(name, NULL) >= 0; }

long neofs_file_size(const char *name)
{
    struct neofs_entry e;
    if (find_by_name(name, &e) < 0) return -1;
    return (long)e.size;
}

int neofs_write_file(const char *name, const void *data, u32 size)
{
    if (!mounted) return -1;
    if (strlen(name) >= NEOFS_NAME_MAX) return -1;
    if (size > NEOFS_MAX_FILE_SIZE) return -1;

    u8 dir[NEOFS_BLOCK_SIZE];
    if (read_block(super.dir_block, dir) != 0) return -1;
    struct neofs_entry *entries = (struct neofs_entry *)dir;

    int slot = -1, existing = -1;
    for (int i = 0; i < ENTRIES_PER_BLOCK; i++) {
        if (entries[i].first_block != 0 && strcmp(entries[i].name, name) == 0) {
            existing = i;
            break;
        }
        if (slot < 0 && (entries[i].first_block == 0 || entries[i].name[0] == '\0'))
            slot = i;
    }
    int overwrite = existing >= 0;
    if (overwrite) slot = existing;
    if (slot < 0) return -1;              /* directory full */

    u32 blocks = (size + NEOFS_BLOCK_SIZE - 1) / NEOFS_BLOCK_SIZE;
    if (blocks == 0) blocks = 1;

    u32 first;
    if (overwrite && entries[slot].first_block != 0) {
        u32 old_blocks = (entries[slot].size + NEOFS_BLOCK_SIZE - 1) / NEOFS_BLOCK_SIZE;
        if (old_blocks == 0) old_blocks = 1;
        if (old_blocks >= blocks) {
            first = entries[slot].first_block;   /* reuse the existing extent */
        } else {
            first = super.next_free_block;
            if (first + blocks > super.total_blocks) return -1;
            super.next_free_block = first + blocks;
        }
    } else {
        first = super.next_free_block;
        if (first + blocks > super.total_blocks) return -1;
        super.next_free_block = first + blocks;
    }

    const u8 *src = data;
    u8 block[NEOFS_BLOCK_SIZE];
    for (u32 b = 0; b < blocks; b++) {
        u32 off = b * NEOFS_BLOCK_SIZE;
        u32 n = size > off ? MIN(size - off, (u32)NEOFS_BLOCK_SIZE) : 0;
        memset(block, 0, sizeof(block));
        if (n) memcpy(block, src + off, n);
        if (write_block(first + b, block) != 0) return -1;
    }

    memset(&entries[slot], 0, sizeof(entries[slot]));
    strlcpy(entries[slot].name, name, NEOFS_NAME_MAX);
    entries[slot].size = size;
    entries[slot].first_block = first;

    if (!overwrite) super.file_count++;
    super.checksum = neofs_checksum(&super);

    if (write_block(super.dir_block, dir) != 0) return -1;
    /* Block 0 is a full sector; the superblock is only its first 40 bytes, so
       write through a sector-sized buffer rather than over-reading the struct. */
    u8 sblock[NEOFS_BLOCK_SIZE];
    memset(sblock, 0, sizeof(sblock));
    memcpy(sblock, &super, sizeof(super));
    if (write_block(0, sblock) != 0) return -1;
    return 0;
}

int neofs_read_file(const char *name, void *buf, u32 max, u32 *out_size)
{
    if (!mounted) return -1;
    struct neofs_entry e;
    if (find_by_name(name, &e) < 0) return -1;
    if (e.size > max) return -1;

    u32 blocks = (e.size + NEOFS_BLOCK_SIZE - 1) / NEOFS_BLOCK_SIZE;
    u8 *dst = buf;
    u8 block[NEOFS_BLOCK_SIZE];
    u32 done = 0;
    for (u32 b = 0; b < blocks; b++) {
        if (read_block(e.first_block + b, block) != 0) return -1;
        u32 n = MIN(e.size - done, (u32)NEOFS_BLOCK_SIZE);
        memcpy(dst + done, block, n);
        done += n;
    }
    if (out_size) *out_size = e.size;
    return 0;
}

int neofs_delete_file(const char *name)
{
    if (!mounted) return -1;
    u8 dir[NEOFS_BLOCK_SIZE];
    if (read_block(super.dir_block, dir) != 0) return -1;
    struct neofs_entry *entries = (struct neofs_entry *)dir;
    for (int i = 0; i < ENTRIES_PER_BLOCK; i++) {
        if (entries[i].first_block != 0 && strcmp(entries[i].name, name) == 0) {
            memset(&entries[i], 0, sizeof(entries[i]));
            if (super.file_count) super.file_count--;
            super.checksum = neofs_checksum(&super);
            if (write_block(super.dir_block, dir) != 0) return -1;
            u8 sblock[NEOFS_BLOCK_SIZE];
            memset(sblock, 0, sizeof(sblock));
            memcpy(sblock, &super, sizeof(super));
            if (write_block(0, sblock) != 0) return -1;
            return 0;
        }
    }
    return -1;
}

void neofs_list(neofs_list_cb cb, void *ctx)
{
    if (!mounted) return;
    u8 dir[NEOFS_BLOCK_SIZE];
    if (read_block(super.dir_block, dir) != 0) return;
    struct neofs_entry *entries = (struct neofs_entry *)dir;
    for (int i = 0; i < ENTRIES_PER_BLOCK; i++) {
        if (entries[i].first_block == 0 || entries[i].name[0] == '\0') continue;
        if (cb) cb(&entries[i], ctx);
    }
}

int neofs_file_count(void) { return mounted ? (int)super.file_count : 0; }
