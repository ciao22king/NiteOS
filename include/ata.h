#ifndef NEO_ATA_H
#define NEO_ATA_H

#include <nitekernel.h>

#define ATA_SECTOR_SIZE 512

struct ata_drive {
    int  present;
    int  channel;          /* 0 = primary, 1 = secondary */
    int  drive;            /* 0 = master, 1 = slave */
    int  is_atapi;         /* 1 for ATAPI (CD/DVD) devices */
    int  lba48;
    u32  sectors;          /* total addressable LBA28 sectors */
    char model[41];
    char serial[21];
    char firmware[9];
};

void ata_init(void);
int  ata_drive_count(void);
const struct ata_drive *ata_get_drive(int index);
const struct ata_drive *ata_first_hard_disk(void);
void ata_print_devices(void);

/* LBA28 sector I/O. Returns 0 on success. */
int ata_read_sectors(const struct ata_drive *d, u32 lba, u8 count, void *buffer);
int ata_write_sectors(const struct ata_drive *d, u32 lba, u8 count, const void *buffer);

/* Flush the drive's write cache. */
int ata_flush(const struct ata_drive *d);

#endif
