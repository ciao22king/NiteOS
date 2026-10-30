#ifndef NEO_BLOCKDEV_H
#define NEO_BLOCKDEV_H

#include <nitekernel.h>
#include <ata.h>

struct block_device {
    char name[16];
    u32  sector_count;
    u32  sector_size;
    const struct ata_drive *ata;
};

void blockdev_init(void);
int  blockdev_count(void);
const struct block_device *blockdev_get(int index);
const struct block_device *blockdev_first(void);

int  blockdev_read(const struct block_device *bd, u32 lba, u32 count, void *buf);
int  blockdev_write(const struct block_device *bd, u32 lba, u32 count, const void *buf);
void blockdev_print(void);

#endif
