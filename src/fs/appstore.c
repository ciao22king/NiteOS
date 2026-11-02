#include <appstore.h>
#include <console.h>
#include <net.h>
#include <neofs.h>
#include <mm.h>
#include <store.h>
#include <sha256.h>
#include <string.h>

#define APP_URL_MAX 320

static const char *const fmt_names[APP_FMT_COUNT] = {
    "lash", "exe", "appimage", "elf", "dmg"
};

const char *app_format_name(u8 fmt)
{
    if (fmt >= APP_FMT_COUNT) return "unknown";
    return fmt_names[fmt];
}

/* How to launch a package once it is on the volume. A .lash package is a
   script, so it names the interpreter; everything else goes through `run`,
   which sniffs the format anyway. */
const char *app_format_runner(u8 fmt)
{
    return fmt == APP_FMT_LASH ? "lash" : "run";
}

/* Preference order: native package, then the Windows loader, then AppImage,
   then a plain ELF. Anything else (a .dmg, or a format this build does not
   know) ranks past every real candidate and is therefore never installed. */
static int fmt_rank(u8 fmt)
{
    switch (fmt) {
    case APP_FMT_LASH:     return 0;
    case APP_FMT_EXE:      return 1;
    case APP_FMT_APPIMAGE: return 2;
    case APP_FMT_ELF:      return 3;
    default:               return 99;
    }
}

/* A candidate is installable only if the catalogue knows its digest: an entry
   without one is published elsewhere but has nothing for this mirror to serve. */
static int installable(const struct app_candidate *c)
{
    return c->sha256 && c->sha256[0] && c->size > 0;
}

u32 app_count(void) { return g_app_count; }

const struct app_entry *app_at(u32 index)
{
    if (index >= g_app_count) return NULL;
    return &g_app_entries[index];
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ieq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (lower(*a) != lower(*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Case-insensitive substring test; an empty needle matches everything. */
static int ifind(const char *hay, const char *needle)
{
    if (!needle || !*needle) return 1;
    if (!hay) return 0;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

const struct app_entry *app_find(const char *name)
{
    if (!name || !*name) return NULL;

    for (u32 i = 0; i < g_app_count; i++)
        if (ieq(g_app_entries[i].name, name)) return &g_app_entries[i];

    /* Nothing exact: accept a unique partial name, so `install hell` finds
       "hello" but an ambiguous term is left for the caller to disambiguate. */
    const struct app_entry *hit = NULL;
    int hits = 0;
    for (u32 i = 0; i < g_app_count; i++) {
        if (ifind(g_app_entries[i].name, name)) {
            hit = &g_app_entries[i];
            hits++;
        }
    }
    return hits == 1 ? hit : NULL;
}

const struct app_candidate *app_best(const struct app_entry *e)
{
    if (!e) return NULL;
    const struct app_candidate *best = NULL;
    int best_rank = 99;
    for (u32 i = 0; i < e->candidate_count; i++) {
        const struct app_candidate *c = &e->candidates[i];
        if (!installable(c)) continue;
        int r = fmt_rank(c->format);
        if (r < best_rank) {
            best = c;
            best_rank = r;
        }
    }
    return best;
}

void app_print_catalog(const char *needle)
{
    if (g_app_count == 0) {
        console_printf("app store is empty\n");
        return;
    }

    u32 shown = 0;
    for (u32 i = 0; i < g_app_count; i++) {
        const struct app_entry *e = &g_app_entries[i];
        if (!ifind(e->name, needle) && !ifind(e->summary, needle)) continue;
        shown++;

        console_printf("  %-10s %-5s %s\n", e->name, e->version, e->summary);
        const struct app_candidate *best = app_best(e);
        for (u32 j = 0; j < e->candidate_count; j++) {
            const struct app_candidate *c = &e->candidates[j];
            const char *state;
            if (!installable(c))
                state = (c->format == APP_FMT_DMG) ? "macOS only, never selected"
                                                   : "not on this mirror";
            else if (c == best)
                state = neofs_file_exists(c->file) ? "installed" : "preferred";
            else
                state = "alternative";
            console_printf("      %-8s %-16s %8u bytes  %s\n",
                           app_format_name(c->format), c->file, c->size, state);
        }
    }

    if (!shown) {
        if (needle && *needle) console_printf("no app matches '%s'\n", needle);
        else console_printf("app store is empty\n");
    }
}

int app_install(const char *name)
{
    if (!name || !*name) {
        console_printf("usage: install <app> | install --list [term]\n");
        return 1;
    }

    const struct app_entry *e = app_find(name);
    if (!e) {
        console_printf("install: no single app matches '%s'\n", name);
        app_print_catalog(name);
        return 1;
    }

    const struct app_candidate *best = app_best(e);
    if (!best) {
        console_printf("install: %s publishes no package this machine can run\n", e->name);
        for (u32 i = 0; i < e->candidate_count; i++) {
            const struct app_candidate *c = &e->candidates[i];
            console_printf("  %-8s %-16s %s\n", app_format_name(c->format), c->file,
                           c->format == APP_FMT_DMG
                               ? "macOS disk image, unsupported here"
                               : "not downloadable from the mirror");
        }
        return 1;
    }

    /* Say what was chosen and what lost, so a download that picks the second
       format on the site is explained rather than surprising. */
    console_printf("install: %s %s\n", e->name, e->version);
    console_printf("  picked %s (%s, %u bytes)\n", best->file,
                   app_format_name(best->format), best->size);
    for (u32 i = 0; i < e->candidate_count; i++) {
        const struct app_candidate *c = &e->candidates[i];
        if (c == best) continue;
        if (!installable(c)) {
            if (c->format == APP_FMT_DMG)
                console_printf("  skipping %s: macOS disk image, cannot run here\n", c->file);
            continue;
        }
        if (fmt_rank(c->format) > fmt_rank(best->format))
            console_printf("  skipped %s (%s): %s is preferred\n",
                           c->file, app_format_name(c->format),
                           app_format_name(best->format));
    }

    if (neofs_file_exists(best->file)) {
        console_printf("  already installed; run it with: %s %s\n",
                       app_format_runner(best->format), best->file);
        return 0;
    }
    if (!neofs_mounted()) {
        console_printf("install: no writable NEOFS volume to install into\n");
        return 1;
    }
    if (best->size > NEOFS_MAX_FILE_SIZE) {
        console_printf("install: %s needs %u bytes, more than this volume allows (%u)\n",
                       best->file, best->size, (u32)NEOFS_MAX_FILE_SIZE);
        return 1;
    }
    if (!net_ready()) {
        console_printf("install: no network; run 'dhcp' first\n");
        return 1;
    }

    /* The mirror keeps the packages under /apps; the descriptor may also name
       an upstream location to fall back to. */
    char url[APP_URL_MAX];
    u8 *data = NULL;
    u32 len = 0;
    snprintf(url, sizeof(url), "%s/apps/%s", g_app_mirror, best->file);
    if (store_download(url, &data, &len) != 0) {
        if (!best->url[0]) {
            console_printf("install: could not fetch %s from %s\n", best->file, g_app_mirror);
            return 1;
        }
        console_printf("  mirror miss, trying %s\n", best->url);
        if (store_download(best->url, &data, &len) != 0) {
            console_printf("install: download failed for %s\n", best->file);
            return 1;
        }
    }

    u8 digest[SHA256_DIGEST_SIZE];
    char hex[65];
    sha256(data, len, digest);
    sha256_hex(digest, hex);
    if (!sha256_hex_equal(digest, best->sha256)) {
        console_printf("install: checksum mismatch for %s\n", best->file);
        console_printf("  expected %.16s...\n", best->sha256);
        console_printf("  got      %.16s...\n", hex);
        kfree(data);
        return 1;
    }
    if (len != best->size)
        console_printf("  note: fetched %u bytes, catalogue lists %u\n", len, best->size);

    int rc = neofs_write_file(best->file, data, len);
    kfree(data);
    if (rc != 0) {
        console_printf("install: could not store %s on the volume\n", best->file);
        return 1;
    }

    console_printf("  sha256 verified: %.32s...\n", hex);
    console_printf("  installed %s (%u bytes)\n", best->file, len);
    console_printf("  run it with: %s %s\n", app_format_runner(best->format), best->file);
    return 0;
}
