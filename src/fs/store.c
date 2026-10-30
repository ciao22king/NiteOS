#include <store.h>
#include <net.h>
#include <dns.h>
#include <http.h>
#include <tcp.h>
#include <sha256.h>
#include <neofs.h>
#include <console.h>
#include <string.h>
#include <mm.h>

#define DOWNLOAD_MAX (1024 * 1024)
#define DRIVER_DB_FILE "drivers.db"

static struct driver_record db[DRIVER_DB_MAX];
static int db_count;
static int db_dirty;

void store_init(void) { /* the generated table is linked in; nothing to do */ }

u32 store_entry_count(void) { return g_store_count; }

const struct store_entry *store_entry(u32 index)
{
    if (index >= g_store_count) return NULL;
    return &g_store_entries[index];
}

const struct pci_device *store_match(const struct store_entry *e, const struct hw_profile *hw)
{
    for (u32 i = 0; i < hw->pci_count; i++) {
        const struct pci_device *d = &hw->pci[i];
        if (e->class_code && d->class_code != e->class_code) continue;
        if (e->vendor_id && d->vendor_id != e->vendor_id) continue;
        if (e->device_id && d->device_id != e->device_id) continue;
        return d;
    }
    return NULL;
}

/* ---------------- URL handling ---------------- */

/* Split "http://host[:port]/path" into host, port and path. */
static int parse_url(const char *url, char *host, usize host_len,
                     u16 *port, char *path, usize path_len)
{
    const char *p = url;
    if (strncasecmp(p, "http://", 7) == 0) p += 7;

    const char *host_start = p;
    while (*p && *p != '/' && *p != ':') p++;
    usize hl = (usize)(p - host_start);
    if (hl == 0 || hl >= host_len) return -1;
    memcpy(host, host_start, hl);
    host[hl] = '\0';

    *port = 80;
    if (*p == ':') {
        p++;
        u32 v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (u32)(*p - '0'); p++; }
        *port = (u16)v;
    }
    if (*p != '/') return -1;
    strlcpy(path, p, path_len);
    return 0;
}

static int resolve_host(const char *host, u32 *ip)
{
    /* A literal dotted-quad needs no DNS. */
    int digits = 1;
    for (const char *c = host; *c; c++)
        if (!((*c >= '0' && *c <= '9') || *c == '.')) { digits = 0; break; }
    if (digits) { *ip = ip_parse(host); return 0; }
    return dns_resolve(host, ip, 5000);
}

int store_download(const char *url, u8 **out, u32 *out_len)
{
    char host[128], path[256];
    u16 port;
    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        klog_err("store: malformed URL '%s'", url);
        return -1;
    }

    u32 ip;
    if (resolve_host(host, &ip) != 0) {
        klog_err("store: cannot resolve '%s'", host);
        return -1;
    }

    u8 *buf = kmalloc(DOWNLOAD_MAX);
    if (!buf) return -1;

    char ipbuf[16];
    klog_info("STORE", "GET %s -> %s:%u%s", url, ip_str(ip, ipbuf), port, path);
    u32 len = 0;
    int rc = http_get(ip, port, host, path, buf, DOWNLOAD_MAX, &len);
    if (rc != 0) { kfree(buf); return -1; }

    *out = buf;
    *out_len = len;
    return 0;
}

/* ---------------- Driver database ---------------- */

int driverdb_count(void) { return db_count; }

const struct driver_record *driverdb_get(int index)
{
    if (index < 0 || index >= db_count) return NULL;
    return &db[index];
}

int driverdb_has(const char *name)
{
    for (int i = 0; i < db_count; i++)
        if (strcmp(db[i].name, name) == 0) return 1;
    return 0;
}

int driverdb_load(void)
{
    db_count = 0;
    if (!neofs_mounted()) return -1;

    u8 raw[sizeof(db) + 8];
    u32 size = 0;
    if (neofs_read_file(DRIVER_DB_FILE, raw, sizeof(raw), &size) != 0) return -1;
    if (size < 4) return -1;

    u32 count = *(u32 *)raw;
    if (count > DRIVER_DB_MAX) count = DRIVER_DB_MAX;
    u32 need = 4 + count * (u32)sizeof(struct driver_record);
    if (need > size) return -1;

    memcpy(db, raw + 4, count * sizeof(struct driver_record));
    db_count = (int)count;
    return 0;
}

int driverdb_save(void)
{
    if (!neofs_mounted()) return -1;
    u8 raw[4 + DRIVER_DB_MAX * sizeof(struct driver_record)];
    *(u32 *)raw = (u32)db_count;
    memcpy(raw + 4, db, (usize)db_count * sizeof(struct driver_record));
    u32 len = 4 + (u32)db_count * (u32)sizeof(struct driver_record);
    if (neofs_write_file(DRIVER_DB_FILE, raw, len) != 0) return -1;
    db_dirty = 0;
    return 0;
}

static struct driver_record *driverdb_add(const struct store_entry *e,
                                          const struct pci_device *dev, u32 size)
{
    if (db_count >= DRIVER_DB_MAX) return NULL;
    struct driver_record *r = &db[db_count++];
    memset(r, 0, sizeof(*r));
    strlcpy(r->name, e->name, sizeof(r->name));
    strlcpy(r->kind, e->kind, sizeof(r->kind));
    strlcpy(r->version, e->version, sizeof(r->version));
    strlcpy(r->sha256, e->sha256, sizeof(r->sha256));
    strlcpy(r->file, e->file, sizeof(r->file));
    r->size = size;
    r->vendor_id = dev->vendor_id;
    r->device_id = dev->device_id;
    r->class_code = dev->class_code;
    r->loaded = 1;
    db_dirty = 1;
    return r;
}

void driverdb_print(void)
{
    if (db_count == 0) { klog_info("DRVDB", "no drivers installed yet"); return; }
    klog_info("DRVDB", "%d driver(s) installed:", db_count);
    for (int i = 0; i < db_count; i++) {
        const struct driver_record *r = &db[i];
        klog_info("DRVDB", "  [%d] %s %s (%s) %u bytes, for %04x:%04x class %02x  sha256=%.12s...",
                  i, r->name, r->version, r->kind, r->size,
                  r->vendor_id, r->device_id, r->class_code, r->sha256);
    }
}

/* ---------------- First-boot acquisition ---------------- */

static int acquire_one(const struct store_entry *e, const struct pci_device *dev,
                       const char *mirror_url, const char *fallback_url)
{
    u8 *data = NULL;
    u32 len = 0;

    if (store_download(mirror_url, &data, &len) != 0) {
        klog_warn("mirror miss for %s, trying upstream", e->name);
        if (store_download(fallback_url, &data, &len) != 0) {
            klog_err("download failed for %s", e->name);
            return -1;
        }
    }

    u8 digest[SHA256_DIGEST_SIZE];
    char hex[65];
    sha256(data, len, digest);
    sha256_hex(digest, hex);

    if (!sha256_hex_equal(digest, e->sha256)) {
        klog_err("checksum mismatch for %s", e->name);
        klog_err("  expected %.16s...", e->sha256);
        klog_err("  got      %.16s...", hex);
        kfree(data);
        return -1;
    }

    /* Stage the verified payload on disk so later boots need no network. */
    char blob_name[NEOFS_NAME_MAX];
    strlcpy(blob_name, e->file, sizeof(blob_name));
    if (neofs_mounted()) {
        if (neofs_write_file(blob_name, data, len) != 0)
            klog_warn("could not stage %s on disk", blob_name);
    }

    klog_ok("installed %s v%s (%s, %u bytes) for %04x:%04x class %02x",
            e->name, e->version, e->kind, len,
            dev->vendor_id, dev->device_id, dev->class_code);
    klog_info("STORE", "  sha256 verified: %.32s...", hex);

    driverdb_add(e, dev, len);
    kfree(data);
    return 0;
}

int store_acquire(const struct hw_profile *hw)
{
    if (!net_ready()) {
        klog_warn("no network; skipping driver acquisition");
        return 0;
    }
    if (!neofs_mounted()) {
        klog_warn("no writable disk; drivers will not persist");
    }

    int installed = 0;
    int checked = 0;

    for (u32 i = 0; i < g_store_count; i++) {
        const struct store_entry *e = &g_store_entries[i];
        const struct pci_device *dev = store_match(e, hw);
        if (!dev) continue;
        checked++;

        if (driverdb_has(e->name)) {
            klog_info("STORE", "%s already installed; skipping", e->name);
            continue;
        }

        char mirror_url[320], fallback[320];
        snprintf(mirror_url, sizeof(mirror_url), "%s/payloads/%s", g_store_mirror, e->file);
        snprintf(fallback, sizeof(fallback), "%s", e->url);

        if (acquire_one(e, dev, mirror_url, fallback) == 0) installed++;
    }

    if (checked == 0)
        klog_info("STORE", "no store entries match the installed hardware");

    if (db_dirty && neofs_mounted()) {
        if (driverdb_save() == 0)
            klog_ok("driver database saved (%d record(s))", db_count);
        else
            klog_warn("could not persist the driver database");
    }
    return installed;
}
