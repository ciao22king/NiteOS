#include <ata.h>
#include <console.h>
#include <string.h>
#include <cpu.h>

/* Primary channel */
#define ATA_PRIMARY_IO   0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
/* Secondary channel */
#define ATA_SECONDARY_IO   0x170
#define ATA_SECONDARY_CTRL 0x376

#define ATA_REG_DATA      0
#define ATA_REG_ERROR     1
#define ATA_REG_FEATURES  1
#define ATA_REG_SECCOUNT  2
#define ATA_REG_LBA0      3
#define ATA_REG_LBA1      4
#define ATA_REG_LBA2      5
#define ATA_REG_DRIVE     6
#define ATA_REG_COMMAND   7
#define ATA_REG_STATUS    7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_FLUSH      0xE7
#define ATA_CMD_IDENTIFY_PACKET 0xA1

static struct ata_drive drives[4];
static int drive_count;

static inline u16 io_base(int channel) { return channel ? ATA_SECONDARY_IO : ATA_PRIMARY_IO; }
static inline u16 ctrl_base(int channel) { return channel ? ATA_SECONDARY_CTRL : ATA_PRIMARY_CTRL; }

static u8 status(int channel) { return inb(io_base(channel) + ATA_REG_STATUS); }

static int wait_not_busy(int channel, u32 timeout)
{
    while (timeout--) {
        u8 s = status(channel);
        if (!(s & ATA_SR_BSY)) return 0;
    }
    return -1;
}

/* Wait for the drive to assert DRQ (data ready) or report an error. */
static int wait_drq(int channel, u32 timeout)
{
    while (timeout--) {
        u8 s = status(channel);
        if (s & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (s & ATA_SR_DRQ) return 0;
    }
    return -1;
}

static void io_delay(int channel) { (void)inb(ctrl_base(channel) + 2); (void)inb(ctrl_base(channel) + 2); }

static void select_drive(int channel, int drive)
{
    outb(io_base(channel) + ATA_REG_DRIVE, (u8)(0xA0 | (drive << 4)));
    io_delay(channel);
}

static void identify_to_drive(struct ata_drive *d, const u16 *id)
{
    d->present = 1;
    for (int i = 0; i < 20; i++) {
        d->model[i * 2] = (char)(id[27 + i] >> 8);
        d->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    d->model[40] = '\0';
    for (int i = 0; i < 10; i++) {
        d->serial[i * 2] = (char)(id[10 + i] >> 8);
        d->serial[i * 2 + 1] = (char)(id[10 + i] & 0xFF);
    }
    d->serial[20] = '\0';
    for (int i = 0; i < 4; i++) {
        d->firmware[i * 2] = (char)(id[23 + i] >> 8);
        d->firmware[i * 2 + 1] = (char)(id[23 + i] & 0xFF);
    }
    d->firmware[8] = '\0';

    d->lba48 = (id[83] & 0x0400) != 0;
    d->sectors = ((u32)id[61] << 16) | id[60];
    str_trim(d->model);
    str_trim(d->serial);
    str_trim(d->firmware);
}

static void probe_drive(int channel, int drive)
{
    select_drive(channel, drive);
    outb(io_base(channel) + ATA_REG_SECCOUNT, 0);
    outb(io_base(channel) + ATA_REG_LBA0, 0);
    outb(io_base(channel) + ATA_REG_LBA1, 0);
    outb(io_base(channel) + ATA_REG_LBA2, 0);
    outb(io_base(channel) + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_delay(channel);

    u8 s = status(channel);
    if (s == 0) return;                       /* no drive on this slot */

    if (wait_not_busy(channel, 100000) != 0) return;
    /* A non-zero LBA1 means ATAPI rather than plain ATA. */
    if (inb(io_base(channel) + ATA_REG_LBA1) != 0 || inb(io_base(channel) + ATA_REG_LBA2) != 0) {
        outb(io_base(channel) + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
        io_delay(channel);
        if (wait_drq(channel, 100000) != 0) return;
        struct ata_drive d;
        memset(&d, 0, sizeof(d));
        d.channel = channel;
        d.drive = drive;
        d.is_atapi = 1;
        u16 id[256];
        for (int i = 0; i < 256; i++) id[i] = inw(io_base(channel) + ATA_REG_DATA);
        identify_to_drive(&d, id);
        d.is_atapi = 1;
        d.sectors = 0;
        strlcpy(d.model, d.model[0] ? d.model : "ATAPI device", sizeof(d.model));
        if (drive_count < 4) drives[drive_count++] = d;
        return;
    }

    if (wait_drq(channel, 100000) != 0) return;

    struct ata_drive d;
    memset(&d, 0, sizeof(d));
    d.channel = channel;
    d.drive = drive;
    u16 id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(io_base(channel) + ATA_REG_DATA);
    identify_to_drive(&d, id);
    if (drive_count < 4) drives[drive_count++] = d;
}

void ata_init(void)
{
    drive_count = 0;
    memset(drives, 0, sizeof(drives));
    for (int ch = 0; ch < 2; ch++) {
        for (int dr = 0; dr < 2; dr++)
            probe_drive(ch, dr);
    }
}

int ata_drive_count(void) { return drive_count; }

const struct ata_drive *ata_get_drive(int index)
{
    if (index < 0 || index >= drive_count) return NULL;
    return &drives[index];
}

const struct ata_drive *ata_first_hard_disk(void)
{
    for (int i = 0; i < drive_count; i++)
        if (!drives[i].is_atapi) return &drives[i];
    return NULL;
}

void ata_print_devices(void)
{
    if (drive_count == 0) {
        klog_warn("no ATA/ATAPI devices detected");
        return;
    }
    for (int i = 0; i < drive_count; i++) {
        const struct ata_drive *d = &drives[i];
        if (d->is_atapi)
            klog_info("ATA", "%s %s: %s (ATAPI)",
                      d->channel ? "secondary" : "primary",
                      d->drive ? "slave" : "master", d->model);
        else
            klog_info("ATA", "%s %s: %s, %u MiB, LBA%s",
                      d->channel ? "secondary" : "primary",
                      d->drive ? "slave" : "master", d->model,
                      (u32)(((u64)d->sectors * ATA_SECTOR_SIZE) >> 20),
                      d->lba48 ? "48" : "28");
    }
}

int ata_read_sectors(const struct ata_drive *d, u32 lba, u8 count, void *buffer)
{
    if (!d || !d->present || d->is_atapi || count == 0) return -1;
    int ch = d->channel;
    if (wait_not_busy(ch, 1000000) != 0) return -1;

    outb(io_base(ch) + ATA_REG_DRIVE, (u8)(0xE0 | (d->drive << 4) | ((lba >> 24) & 0x0F)));
    outb(io_base(ch) + ATA_REG_FEATURES, 0);
    outb(io_base(ch) + ATA_REG_SECCOUNT, count);
    outb(io_base(ch) + ATA_REG_LBA0, (u8)(lba & 0xFF));
    outb(io_base(ch) + ATA_REG_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(io_base(ch) + ATA_REG_LBA2, (u8)((lba >> 16) & 0xFF));
    outb(io_base(ch) + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    u16 *out = (u16 *)buffer;
    for (int s = 0; s < count; s++) {
        if (wait_drq(ch, 1000000) != 0) return -1;
        for (int i = 0; i < 256; i++) *out++ = inw(io_base(ch) + ATA_REG_DATA);
    }
    return 0;
}

int ata_write_sectors(const struct ata_drive *d, u32 lba, u8 count, const void *buffer)
{
    if (!d || !d->present || d->is_atapi || count == 0) return -1;
    int ch = d->channel;
    if (wait_not_busy(ch, 1000000) != 0) return -1;

    outb(io_base(ch) + ATA_REG_DRIVE, (u8)(0xE0 | (d->drive << 4) | ((lba >> 24) & 0x0F)));
    outb(io_base(ch) + ATA_REG_FEATURES, 0);
    outb(io_base(ch) + ATA_REG_SECCOUNT, count);
    outb(io_base(ch) + ATA_REG_LBA0, (u8)(lba & 0xFF));
    outb(io_base(ch) + ATA_REG_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(io_base(ch) + ATA_REG_LBA2, (u8)((lba >> 16) & 0xFF));
    outb(io_base(ch) + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    const u16 *in = (const u16 *)buffer;
    for (int s = 0; s < count; s++) {
        if (wait_drq(ch, 1000000) != 0) return -1;
        for (int i = 0; i < 256; i++) outw(io_base(ch) + ATA_REG_DATA, *in++);
    }
    return ata_flush(d);
}

int ata_flush(const struct ata_drive *d)
{
    if (!d || !d->present || d->is_atapi) return -1;
    outb(io_base(d->channel) + ATA_REG_COMMAND, ATA_CMD_FLUSH);
    return wait_not_busy(d->channel, 1000000);
}
