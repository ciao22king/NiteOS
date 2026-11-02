#include <shell.h>
#include <input.h>
#include <console.h>
#include <string.h>
#include <cpu.h>
#include <mm.h>
#include <devices.h>
#include <gfx.h>
#include <net.h>
#include <dns.h>
#include <http.h>
#include <neofs.h>
#include <store.h>
#include <ata.h>
#include <blockdev.h>
#include <lash.h>
#include <process.h>
#include <loader.h>
#include <syscall.h>

#define LINE_MAX 256
#define HISTORY_MAX 8

static char line[LINE_MAX];
static u32 line_len;
static char history[HISTORY_MAX][LINE_MAX];
static int history_count;
/* Line editing state: which history entry is on screen (== history_count when
   editing a fresh line), the draft saved at the first recall, how many columns
   the painted line occupies, and the serial escape-sequence parser. */
static int hist_pos;
static char hist_draft[LINE_MAX];
static u32 rendered_cols;
static int esc_state;

extern struct hw_profile *device_profile(void);
extern void rtc_now_string(char *buf, usize n);
extern void sha256(const void *data, usize len, u8 out[32]);
extern void sha256_hex(const u8 digest[32], char out[65]);
extern int dns_resolve_or_parse(const char *host, u32 *out_ip);

static void cmd_run(char *path, int argc, char **argv);
static void cmd_lash(char *path, int argc, char **argv);

/* ---------------- helpers ---------------- */

/* The prompt gains the address once DHCP or ifconfig configured the
   interface. It is rebuilt on every repaint so line editing stays aligned. */
static void build_prompt(char *buf, usize n)
{
    char ipbuf[16];
    struct netif *net = net_iface();
    if (net->configured)
        snprintf(buf, n, "nite:%s> ", ip_str(net->ip, ipbuf));
    else
        strlcpy(buf, "nite> ", n);
}

static void print_prompt(void)
{
    char buf[48];
    build_prompt(buf, sizeof(buf));
    console_write(buf);
    rendered_cols = (u32)strlen(buf);
}

/* ---------------- commands ---------------- */

static void cmd_help(void)
{
    console_printf("NiteKernel shell commands:\n");
    console_printf("  help              this list\n");
    console_printf("  report            full PC component report\n");
    console_printf("  cpu               CPU details\n");
    console_printf("  mem               memory usage\n");
    console_printf("  pci               enumerate the PCI bus\n");
    console_printf("  gfx               graphics/framebuffer info\n");
    console_printf("  rtc               current date and time\n");
    console_printf("  ata               ATA/ATAPI devices\n");
    console_printf("  lsblk             block devices\n");
    console_printf("  net               network status\n");
    console_printf("  ifconfig <ip>/<mask> [gw] [dns]  set a static address\n");
    console_printf("  dhcp              request an address via DHCP\n");
    console_printf("  ping <host|ip>    ICMP echo\n");
    console_printf("  dns <host>        resolve a hostname\n");
    console_printf("  fetch <url>       HTTP GET (prints size + sha256)\n");
    console_printf("  store             list the driver store\n");
    console_printf("  drivers           installed driver database\n");
    console_printf("  install           run first-boot driver acquisition\n");
    console_printf("  fs                filesystem status\n");
    console_printf("  ls                list files on the NEOFS volume\n");
    console_printf("  cat <file>        print a file\n");
    console_printf("  write <f> <text>  create/overwrite a file\n");
    console_printf("  rm <file>         delete a file\n");
    console_printf("  uptime            ticks since boot\n");
    console_printf("  echo <text>       print text\n");
    console_printf("  clear             clear the screen\n");
    console_printf("  reboot            reset the machine\n");
    console_printf("  halt              stop the CPU\n");
    console_printf("  poweroff          stop the CPU (alias for halt)\n");
    console_printf("  trace [on|off]    toggle syscall tracing\n");
    console_printf("  run <file>        execute an ELF/PE/.lash program\n");
    console_printf("  exec <file>       alias for run\n");
    console_printf("  lash <file>       run a .lash script\n");
    console_printf("  history           recall list of this session's commands\n");
    console_printf("  sha256sum <file>  print the SHA-256 of a file\n");
    console_printf("  bash|sh|<file>    built-in lash interpreter\n");
}

static void cmd_cpu(void)
{
    struct hw_profile *p = device_profile();
    console_printf("CPU:    %s (%s)\n", p->cpu_brand, p->cpu_vendor);
    console_printf("Family %u model %u, %u core(s), %u logical\n",
                   p->cpu_family, p->cpu_model, p->cpu_cores, p->cpu_logical);
    console_printf("Flags: ");
    if (p->cpu_flags & CPUF_APIC) console_printf("apic ");
    if (p->cpu_flags & CPUF_X2APIC) console_printf("x2apic ");
    if (p->cpu_flags & CPUF_SSE2) console_printf("sse2 ");
    if (p->cpu_flags & CPUF_AVX) console_printf("avx ");
    if (p->cpu_flags & CPUF_AVX2) console_printf("avx2 ");
    if (p->cpu_flags & CPUF_NX) console_printf("nx ");
    if (p->cpu_flags & CPUF_VMX) console_printf("vmx ");
    if (p->cpu_flags & CPUF_SVM) console_printf("svm ");
    console_printf("\n");
}

static void cmd_mem(void)
{
    usize total = 0, used = 0, freeb = 0;
    heap_stats(&total, &used, &freeb);
    console_printf("Physical: %u MiB total, %u MiB free, %u MiB used\n",
                   (u32)(pmm_total_bytes() >> 20), (u32)(pmm_free_bytes() >> 20),
                   (u32)(pmm_used_bytes() >> 20));
    usize mapped = 0;
    vmm_stats(&mapped);
    console_printf("Virtual:  %u pages mapped\n", (u32)mapped);
    console_printf("Heap:     %u KiB arena, %u KiB used, %u KiB free\n",
                   (u32)(total >> 10), (u32)(used >> 10), (u32)(freeb >> 10));
}

static void cmd_gfx(void)
{
    if (!console_has_fb()) { console_printf("no framebuffer attached\n"); return; }
    const struct gfx_mode *m = gfx_get_mode();
    console_printf("Mode:    %ux%u x %u bpp, pitch %u\n",
                   m->width, m->height, m->bpp, m->pitch);
    console_printf("Address: %p (virtual %p)\n",
                   (void *)m->fb_phys, (void *)m->fb_virt);
}

static void cmd_pci(void)
{
    struct hw_profile *p = device_profile();
    console_printf("%u PCI device(s):\n", p->pci_count);
    for (u32 i = 0; i < p->pci_count; i++) {
        const struct pci_device *d = &p->pci[i];
        console_printf("  %02x:%02x.%u  %04x:%04x  %s\n",
                       d->bus, d->slot, d->func, d->vendor_id, d->device_id,
                       pci_class_name(d->class_code, d->subclass));
    }
}

static void cmd_net(void)
{
    net_print_status();
}

static void cmd_dhcp(void)
{
    console_printf("requesting DHCP lease...\n");
    if (dhcp_configure(8000) == 0) {
        char ip[16], gw[16], dns[16];
        struct netif *n = net_iface();
        console_printf("lease: ip %s gateway %s dns %s\n",
                       ip_str(n->ip, ip), ip_str(n->gateway, gw), ip_str(n->dns, dns));
    } else {
        console_printf("DHCP failed (no server reachable)\n");
    }
}

static void cmd_ifconfig(int argc, char **argv)
{
    struct netif *n = net_iface();
    if (argc < 2) {
        char ip[16], mask[16], gw[16], dns[16];
        console_printf("%s  ip %s  mask %s  gw %s  dns %s\n", n->driver,
                       ip_str(n->ip, ip), ip_str(n->netmask, mask),
                       ip_str(n->gateway, gw), ip_str(n->dns, dns));
        return;
    }
    /* <ip>[/<mask>] [gateway] [dns] */
    char addr[32];
    strlcpy(addr, argv[1], sizeof(addr));
    char *slash = strchr(addr, '/');
    if (slash) { *slash = '\0'; n->netmask = ip_parse(slash + 1); }
    else n->netmask = ip_make(255, 255, 255, 0);
    n->ip = ip_parse(addr);
    n->gateway = argc > 2 ? ip_parse(argv[2]) : 0;
    n->dns = argc > 3 ? ip_parse(argv[3]) : n->gateway;
    n->configured = 1;
    char ip[16], mask[16];
    console_printf("static address set: ip %s mask %s\n",
                   ip_str(n->ip, ip), ip_str(n->netmask, mask));
}

static void cmd_ping(char *host)
{
    u32 ip;
    if (!host) { console_printf("usage: ping <host|ip>\n"); return; }
    if (dns_resolve_or_parse(host, &ip) != 0) {
        console_printf("cannot resolve %s\n", host);
        return;
    }
    char ipbuf[16];
    console_printf("PING %s (%s):\n", host, ip_str(ip, ipbuf));
    for (int i = 0; i < 4; i++) {
        if (net_ping(ip) == 0) console_printf("  reply from %s\n", ipbuf);
        else console_printf("  request timed out\n");
    }
}

static void cmd_dns(char *host)
{
    if (!host) { console_printf("usage: dns <host>\n"); return; }
    u32 ip;
    if (dns_resolve(host, &ip, 5000) == 0) {
        char buf[16];
        console_printf("%s -> %s\n", host, ip_str(ip, buf));
    } else {
        console_printf("resolution failed for %s\n", host);
    }
}

static void cmd_fetch(char *url)
{
    if (!url) { console_printf("usage: fetch <url>\n"); return; }
    if (!net_ready()) { console_printf("no network configured; run 'dhcp' first\n"); return; }
    u8 *data = NULL;
    u32 len = 0;
    if (store_download(url, &data, &len) != 0) {
        console_printf("download failed\n");
        return;
    }
    u8 digest[32];
    char hex[65];
    sha256(data, len, digest);
    sha256_hex(digest, hex);
    console_printf("downloaded %u bytes\nsha256 %s\n", len, hex);
    kfree(data);
}

static void list_cb(const struct neofs_entry *e, void *ctx)
{
    (void)ctx;
    console_printf("  %-24s %u bytes\n", e->name, e->size);
}

static void cmd_fs(void)
{
    if (neofs_mounted()) {
        console_printf("NEOFS mounted on %s, %d file(s)\n",
                       neofs_device_name(), neofs_file_count());
    } else {
        console_printf("no NEOFS volume mounted\n");
    }
}

static void cmd_ls(void)
{
    if (!neofs_mounted()) { console_printf("no filesystem\n"); return; }
    console_printf("%d file(s):\n", neofs_file_count());
    neofs_list(list_cb, NULL);
}

static void cmd_cat(char *name)
{
    static u8 buf[NEOFS_MAX_FILE_SIZE];
    u32 size = 0;
    if (!name) {
        /* No operand means "copy stdin", which is what makes `ls | cat` and
           `cat < file` work. Refuse to block on a console with no data. */
        if (!syscall_stdin_redirected()) {
            console_printf("usage: cat <file>\n");
            return;
        }
        long n;
        while (size < sizeof(buf) - 1 &&
               (n = syscall_stdin_read(buf + size, (u32)(sizeof(buf) - 1 - size))) > 0)
            size += (u32)n;
        for (u32 i = 0; i < size; i++) {
            if (buf[i] == 0) break;
            console_putc((char)buf[i]);
        }
        if (size && buf[size - 1] != '\n') console_putc('\n');
        return;
    }
    if (neofs_read_file(name, buf, sizeof(buf), &size) != 0) {
        console_printf("cannot read %s\n", name);
        return;
    }
    /* Print as text, stopping at the first NUL. */
    for (u32 i = 0; i < size; i++) {
        if (buf[i] == 0) break;
        console_putc((char)buf[i]);
    }
    console_putc('\n');
}

static void cmd_write(char *name, char *rest)
{
    if (!name || !rest) { console_printf("usage: write <file> <text>\n"); return; }
    if (!neofs_mounted()) { console_printf("no filesystem\n"); return; }
    if (neofs_write_file(name, rest, (u32)strlen(rest) + 1) == 0)
        console_printf("wrote %u bytes to %s\n", (u32)strlen(rest) + 1, name);
    else
        console_printf("write failed\n");
}

static void cmd_rm(char *name)
{
    if (!name) { console_printf("usage: rm <file>\n"); return; }
    if (neofs_delete_file(name) == 0) console_printf("removed %s\n", name);
    else console_printf("cannot remove %s\n", name);
}

static void cmd_history(void)
{
    int avail = history_count < HISTORY_MAX ? history_count : HISTORY_MAX;
    if (!avail) { console_printf("no history yet\n"); return; }
    int first = history_count - avail;
    for (int i = 0; i < avail; i++)
        console_printf("  %d  %s\n", first + i + 1, history[(first + i) % HISTORY_MAX]);
}

/* Hash a file with the kernel's SHA-256, so a volume round trip can be checked
   against the same digest a host tool computes. */
static void cmd_sha256sum(char *name)
{
    if (!name) { console_printf("usage: sha256sum <file>\n"); return; }
    long sz = neofs_file_size(name);
    if (sz < 0) { console_printf("sha256sum: %s: no such file\n", name); return; }
    u8 *data = kmalloc((u32)sz ? (u32)sz : 1);
    if (!data) { console_printf("sha256sum: out of memory\n"); return; }
    u32 got = (u32)sz;
    if (neofs_read_file(name, data, got, &got) != 0) {
        kfree(data);
        console_printf("sha256sum: %s: unreadable\n", name);
        return;
    }
    u8 digest[32];
    char hex[65];
    sha256(data, got, digest);
    sha256_hex(digest, hex);
    console_printf("%s  %s\n", hex, name);
    kfree(data);
}

static void cmd_store(void)
{
    console_printf("driver store: %u entr(ies), mirror %s\n", store_entry_count(), g_store_mirror);
    for (u32 i = 0; i < store_entry_count(); i++) {
        const struct store_entry *e = store_entry(i);
        console_printf("  [%u] %s v%s (%s) %u bytes, class %02x\n",
                       i, e->name, e->version, e->kind, e->size, e->class_code);
        console_printf("      %s\n", e->provides);
    }
}

static void cmd_drivers(void) { driverdb_print(); }

static void cmd_install(void)
{
    if (!net_ready()) { console_printf("no network; run 'dhcp' first\n"); return; }
    struct hw_profile *p = device_profile();
    int n = store_acquire(p);
    console_printf("acquisition complete: %d new driver(s)\n", n);
    driverdb_print();
}

static void cmd_ata(void) { ata_print_devices(); }
static void cmd_lsblk(void) { blockdev_print(); }

static void cmd_rtc(void)
{
    char buf[32];
    rtc_now_string(buf, sizeof(buf));
    console_printf("%s\n", buf);
}

static void cmd_reboot(void)
{
    console_printf("rebooting...\n");
    outb(0x64, 0xFE);   /* pulse the 8042 reset line */
    for (;;) hlt();
}

/* ---------------- program execution ---------------- */

static void cmd_run(char *path, int argc, char **argv)
{
    if (!path) { console_printf("usage: run <file> [args...]\n"); return; }
    char buf[160];
    const char *p = path;
    if (p[0] == '.' && p[1] == '/') p += 2;
    strlcpy(buf, p, sizeof(buf));

    long fsize = neofs_file_size(buf);
    if (fsize <= 0) { console_printf("run: %s: no such file or empty\n", buf); return; }

    /* neofs_read_file wants the whole file to fit, so load it once and sniff
       the format from the full image. */
    u8 *image = kmalloc((u32)fsize);
    if (!image) { console_printf("run: out of memory\n"); return; }
    u32 got = (u32)fsize;
    if (neofs_read_file(buf, image, got, &got) != 0) {
        console_printf("run: %s: unreadable\n", buf);
        kfree(image);
        return;
    }
    int fmt = image_detect(image, got);
    kfree(image);

    if (fmt == IMG_SCRIPT) { cmd_lash(buf, argc, argv); return; }

    console_printf("exec: %s (%s)\n", buf,
                   fmt == IMG_PE ? "PE32+ .exe" :
                   fmt == IMG_APPIMAGE ? "AppImage" :
                   fmt == IMG_LASH_BIN ? "lash binary" : "ELF64");
    int rc = process_run_file(buf, 0, argc > 1 ? argc - 1 : 1,
                              argc > 1 ? &argv[1] : argv);
    console_printf("[%s exited with code %d]\n", buf, rc);
}

static void cmd_lash(char *path, int argc, char **argv)
{
    /* `bash -c '<script>'` runs the string directly, like the real shell. */
    if (path && !strcmp(path, "-c") && argc > 2 && argv[2]) {
        lash_exec_line(argv[2]);
        return;
    }
    (void)argc; (void)argv;
    if (!path) {
        console_printf("lash: NiteKernel shell (bash-compatible subset)\n");
        return;
    }
    char buf[160];
    const char *p = path;
    if (p[0] == '.' && p[1] == '/') p += 2;
    strlcpy(buf, p, sizeof(buf));
    if (!neofs_file_exists(buf)) {
        console_printf("lash: %s: no such file\n", buf);
        return;
    }
    long fsize = neofs_file_size(buf);
    if (fsize < 0) { console_printf("lash: %s: unreadable\n", buf); return; }
    u32 size = (u32)fsize;
    char *src = kmalloc(size + 1);
    if (!src) return;
    neofs_read_file(buf, src, size, &size);
    src[size] = '\0';
    int rc = lash_run_script(buf, src);
    kfree(src);
    console_printf("[%s exited with status %d]\n", buf, rc);
}

/* ---------------- dispatch ---------------- */

/* Run a built-in command from an already-tokenised argv. */
int shell_run_command(int argc, char **argv)
{
    if (argc == 0) return 0;
    const char *c = argv[0];
    if (!strcmp(c, "help") || !strcmp(c, "?")) cmd_help();
    else if (!strcmp(c, "report")) device_print_report(device_profile());
    else if (!strcmp(c, "cpu")) cmd_cpu();
    else if (!strcmp(c, "mem")) cmd_mem();
    else if (!strcmp(c, "pci")) cmd_pci();
    else if (!strcmp(c, "gfx")) cmd_gfx();
    else if (!strcmp(c, "rtc") || !strcmp(c, "time") || !strcmp(c, "date")) cmd_rtc();
    else if (!strcmp(c, "ata")) cmd_ata();
    else if (!strcmp(c, "lsblk")) cmd_lsblk();
    else if (!strcmp(c, "net")) cmd_net();
    else if (!strcmp(c, "dhcp")) cmd_dhcp();
    else if (!strcmp(c, "ifconfig")) cmd_ifconfig(argc, argv);
    else if (!strcmp(c, "ping")) cmd_ping(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "dns")) cmd_dns(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "fetch")) cmd_fetch(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "store")) cmd_store();
    else if (!strcmp(c, "drivers")) cmd_drivers();
    else if (!strcmp(c, "install")) cmd_install();
    else if (!strcmp(c, "fs")) cmd_fs();
    else if (!strcmp(c, "ls") || !strcmp(c, "dir")) cmd_ls();
    else if (!strcmp(c, "cat")) cmd_cat(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "rm") || !strcmp(c, "del")) cmd_rm(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "history")) cmd_history();
    else if (!strcmp(c, "sha256sum") || !strcmp(c, "sha256"))
        cmd_sha256sum(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "write")) {
        /* Everything after the file name is the payload. Join the tokens into a
           bounded local buffer: the old version rewrote argv[2] in place and
           could run past the tokeniser's word buffer. */
        char payload[LINE_MAX];
        payload[0] = '\0';
        if (argc > 2) {
            u32 pos = (u32)strlcpy(payload, argv[2], sizeof(payload));
            for (int i = 3; i < argc && pos + 2 < sizeof(payload); i++) {
                payload[pos++] = ' ';
                usize room = sizeof(payload) - pos;
                usize n = strlcpy(payload + pos, argv[i], room);
                pos += (u32)(n < room ? n : room - 1);
            }
        }
        cmd_write(argc > 1 ? argv[1] : NULL, argc > 2 ? payload : NULL);
    }
    else if (!strcmp(c, "echo")) {
        for (int i = 1; i < argc; i++) console_printf("%s%s", argv[i], i + 1 < argc ? " " : "");
        console_printf("\n");
    }
    else if (!strcmp(c, "uptime")) {
        u64 ticks = irq_get_ticks();
        console_printf("uptime: %u.%02u s (%u ticks)\n",
                       (u32)(ticks / 100), (u32)(ticks % 100), (u32)ticks);
    }
    else if (!strcmp(c, "clear")) console_clear();
    else if (!strcmp(c, "reboot")) cmd_reboot();
    else if (!strcmp(c, "halt") || !strcmp(c, "poweroff")) {
        console_printf("halting\n");
        for (;;) hlt();
    }
    else if (!strcmp(c, "trace")) {
        extern int syscall_trace;
        syscall_trace = argc > 1 && !strcmp(argv[1], "on");
        console_printf("syscall trace %s\n", syscall_trace ? "on" : "off");
    }
    else if (!strcmp(c, "run") || !strcmp(c, "exec")) cmd_run(argc > 1 ? argv[1] : NULL,
                                                             argc, argv);
    else if (!strcmp(c, "lash") || !strcmp(c, "bash") || !strcmp(c, "sh"))
        cmd_lash(argc > 1 ? argv[1] : NULL, argc, argv);
    else console_printf("unknown command: %s (try 'help')\n", c);
    return 0;
}

/* The names understood by shell_run_command(). Kept in one place so the lash
   interpreter can ask whether a word is a built-in before treating it as a
   file to execute. */
static const char *command_names[] = {
    "help", "?", "report", "cpu", "mem", "pci", "gfx", "rtc", "time", "date",
    "ata", "lsblk", "net", "dhcp", "ifconfig", "ping", "dns", "fetch",
    "store", "drivers", "install", "fs", "ls", "dir", "cat", "rm", "del",
    "history", "sha256sum", "sha256",
    "write", "echo", "uptime", "clear", "reboot", "halt", "poweroff",
    "run", "exec", "lash", "bash", "sh", "trace", NULL
};

int shell_has_command(const char *name)
{
    for (int i = 0; command_names[i]; i++)
        if (!strcmp(name, command_names[i])) return 1;
    return 0;
}

int shell_execute(const char *cmdline)
{
    return lash_exec_line(cmdline);
}

/* ---------------- interactive loop ---------------- */

/* Repaint the input line in place. The three sinks (serial, VGA text, linear
   framebuffer) share no escape-sequence support, so this returns to column 0,
   rewrites prompt and line, blanks whatever a longer previous render left
   behind, then walks back to the end of the text. */
static void redraw_line(void)
{
    char prompt[48];
    build_prompt(prompt, sizeof(prompt));
    console_printf("\r%s%s", prompt, line);
    u32 now = (u32)strlen(prompt) + line_len;
    if (rendered_cols > now) {
        u32 pad = rendered_cols - now;
        for (u32 i = 0; i < pad; i++) console_putc(' ');
        for (u32 i = 0; i < pad; i++) console_putc('\b');
    }
    rendered_cols = now;
}

/* ---------------- history recall ---------------- */

static int history_available(void)
{
    return history_count < HISTORY_MAX ? history_count : HISTORY_MAX;
}

static void history_load(int idx)
{
    strlcpy(line, history[idx % HISTORY_MAX], LINE_MAX);
    line_len = (u32)strlen(line);
}

static void history_up(void)
{
    int avail = history_available();
    if (avail == 0) return;
    int oldest = history_count - avail;
    if (hist_pos == history_count) strlcpy(hist_draft, line, LINE_MAX);
    if (hist_pos <= oldest) return;              /* already at the oldest entry */
    hist_pos--;
    history_load(hist_pos);
    redraw_line();
}

static void history_down(void)
{
    if (hist_pos >= history_count) return;       /* already on a fresh line */
    hist_pos++;
    if (hist_pos == history_count) {
        strlcpy(line, hist_draft, LINE_MAX);
        line_len = (u32)strlen(line);
    } else {
        history_load(hist_pos);
    }
    redraw_line();
}

static void handle_char(char c)
{
    /* A serial terminal sends the arrow keys as ESC [ A / ESC [ B. Fold them
       into the codes the PS/2 driver synthesises so both inputs edit the line
       the same way. */
    if (esc_state) {
        int state = esc_state;
        esc_state = 0;
        if (state == 1 && c == '[') { esc_state = 2; return; }
        if (state == 2) {
            if (c == 'A') history_up();
            else if (c == 'B') history_down();
        }
        return;
    }
    if (c == 0x1B) { esc_state = 1; return; }
    if (c == KEY_UP) { history_up(); return; }
    if (c == KEY_DOWN) { history_down(); return; }

    if (c == '\r') c = '\n';

    if (c == '\n') {
        console_printf("\n");
        line[line_len] = '\0';
        rendered_cols = 0;
        if (line_len > 0) {
            /* Remember the command in history, skipping repeats of the last one. */
            if (history_count == 0 || strcmp(history[(history_count - 1) % HISTORY_MAX], line) != 0) {
                strlcpy(history[history_count % HISTORY_MAX], line, LINE_MAX);
                history_count++;
            }
            shell_execute(line);
        }
        hist_pos = history_count;       /* next edit starts below history */
        line_len = 0;
        line[0] = '\0';
        print_prompt();
        return;
    }

    if (c == '\b' || c == 0x7F) {
        if (line_len > 0) {
            line_len--;
            if (rendered_cols) rendered_cols--;
            console_printf("\b \b");
        }
        return;
    }

    if (c == 3) {                       /* Ctrl+C */
        console_printf("^C\n");
        line_len = 0;
        line[0] = '\0';
        rendered_cols = 0;
        hist_pos = history_count;
        print_prompt();
        return;
    }

    if (c == 12) {                      /* Ctrl+L */
        console_clear();
        print_prompt();
        if (line_len) {
            console_printf("%s", line);
            rendered_cols += line_len;
        }
        return;
    }

    if (c >= ' ' && line_len < LINE_MAX - 1) {
        line[line_len++] = c;
        line[line_len] = '\0';
        console_putc(c);
        rendered_cols++;
    }
}

void shell_input(char c) { handle_char(c); }

void shell_init(void)
{
    line_len = 0;
    line[0] = '\0';
    history_count = 0;
    hist_pos = 0;
    hist_draft[0] = '\0';
    rendered_cols = 0;
    esc_state = 0;
    keyboard_init();
}

/* First-boot behaviour: execute /autorun.lash if it is on the volume, then
   hand the console to a real bash when one is installed. bash exiting drops
   the user back here, so the kernel command table stays reachable. */
static void shell_boot_sequence(void)
{
    if (neofs_file_exists("autorun.lash")) {
        long sz = neofs_file_size("autorun.lash");
        if (sz > 0) {
            char *src = kmalloc((u32)sz + 1);
            if (src) {
                u32 got = (u32)sz;
                if (neofs_read_file("autorun.lash", src, got, &got) == 0) {
                    src[got] = '\0';
                    console_printf("[autorun] %s\n", "executing autorun.lash");
                    lash_run_script("autorun.lash", src);
                }
                kfree(src);
            }
        }
    }
    if (neofs_file_exists("bash")) {
        console_printf("[shell] starting GNU bash; type 'exit' for the kernel shell\n");
        char *args[] = { "bash", NULL };
        process_run_file("bash", 0, 1, args);
        console_printf("[shell] bash finished; back at the kernel shell\n");
    }
}

void shell_run(void)
{
    shell_boot_sequence();
    print_prompt();
    for (;;) {
        int c = keyboard_try_getchar();
        if (c < 0) c = console_getchar();
        if (c < 0) {
            /* Idle: let the CPU sleep until the next interrupt, then poll
               the serial line again (it has no interrupt handler). */
            hlt();
            while ((c = keyboard_try_getchar()) < 0) {
                int s = console_getchar();
                if (s < 0) break;
                handle_char((char)s);
            }
            if (c < 0) continue;
        }
        handle_char((char)c);
    }
}
