#include <blockdev.h>
#include <console.h>
#include <string.h>

#define MAX_BLOCKDEVS 4
static struct block_device bdevs[MAX_BLOCKDEVS];
static int bdev_count;

void blockdev_init(void)
{
    bdev_count = 0;
    for (int i = 0; i < ata_drive_count() && bdev_count < MAX_BLOCKDEVS; i++) {
        const struct ata_drive *d = ata_get_drive(i);
        if (!d || d->is_atapi || d->sectors == 0) continue;
        struct block_device *b = &bdevs[bdev_count];
        memset(b, 0, sizeof(*b));
        snprintf(b->name, sizeof(b->name), "hd%c", 'a' + bdev_count);
        b->sector_count = d->sectors;
        b->sector_size = ATA_SECTOR_SIZE;
        b->ata = d;
        bdev_count++;
    }
}

int blockdev_count(void) { return bdev_count; }

const struct block_device *blockdev_get(int index)
{
    if (index < 0 || index >= bdev_count) return NULL;
    return &bdevs[index];
}

const struct block_device *blockdev_first(void)
{
    return bdev_count ? &bdevs[0] : NULL;
}

int blockdev_read(const struct block_device *bd, u32 lba, u32 count, void *buf)
{
    if (!bd) return -1;
    u8 *p = buf;
    while (count) {
        u8 chunk = (u8)MIN(count, 255u);
        if (ata_read_sectors(bd->ata, lba, chunk, p) != 0) return -1;
        lba += chunk;
        count -= chunk;
        p += (u64)chunk * ATA_SECTOR_SIZE;
    }
    return 0;
}

int blockdev_write(const struct block_device *bd, u32 lba, u32 count, const void *buf)
{
    if (!bd) return -1;
    const u8 *p = buf;
    while (count) {
        u8 chunk = (u8)MIN(count, 255u);
        if (ata_write_sectors(bd->ata, lba, chunk, p) != 0) return -1;
        lba += chunk;
        count -= chunk;
        p += (u64)chunk * ATA_SECTOR_SIZE;
    }
    return 0;
}

void blockdev_print(void)
{
    for (int i = 0; i < bdev_count; i++) {
        const struct block_device *b = &bdevs[i];
        klog_info("BLK", "%s: %u sectors (%u MiB)",
                  b->name, b->sector_count,
                  (u32)(((u64)b->sector_count * b->sector_size) >> 20));
    }
}
