#include <lash.h>
#include <shell.h>
#include <console.h>
#include <string.h>
#include <mm.h>
#include <process.h>
#include <loader.h>
#include <neofs.h>
#include <syscall.h>

/* lash: a bash-compatible command language for NiteKernel.
   Supports: pipelines (|), redirection (> >> <), sequencing (;), logical
   operators (&& ||), variable assignment/expansion ($VAR, ${VAR}), quoting
   ('...' "..." \x), comments (#), and the builtins cd/pwd/export/unset/
   exit/return/true/false/test. External commands are the shell's own
   command table, so `ls`, `ping`, `report` etc. all work from scripts. */

#define VAR_MAX 32
#define NAME_MAX 32
#define VAL_MAX 128
#define WORD_MAX 512
#define ARG_MAX  32
#define PIPE_MAX 4

static struct { char name[NAME_MAX]; char value[VAL_MAX]; } vars[VAR_MAX];
static int var_count;
static int last_status;
static int exit_requested;
static int exit_code;
static const char *script_name = "lash";

void lash_init(void)
{
    var_count = 0;
    last_status = 0;
    exit_requested = 0;
    exit_code = 0;
    lash_set_var("PATH", "/");
    lash_set_var("SHELL", "/bin/lash");
    lash_set_var("HOME", "/");
    lash_set_var("PS1", "neo> ");
}

int lash_last_status(void) { return last_status; }

const char *lash_get_var(const char *name)
{
    for (int i = var_count - 1; i >= 0; i--)
        if (strcmp(vars[i].name, name) == 0) return vars[i].value;
    return NULL;
}

void lash_set_var(const char *name, const char *value)
{
    if (!name || !*name) return;
    for (int i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            strlcpy(vars[i].value, value ? value : "", VAL_MAX);
            return;
        }
    }
    if (var_count >= VAR_MAX) return;
    strlcpy(vars[var_count].name, name, NAME_MAX);
    strlcpy(vars[var_count].value, value ? value : "", VAL_MAX);
    var_count++;
}

static void unset_var(const char *name)
{
    for (int i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            for (int j = i; j + 1 < var_count; j++) vars[j] = vars[j + 1];
            var_count--;
            return;
        }
    }
}

/* ---------------- word expansion ---------------- */

static void append_char(char *out, u32 *pos, u32 max, char c)
{
    if (*pos + 1 < max) out[(*pos)++] = c;
}

/* Expand $VAR, ${VAR}, $?, and $0..$9 inside a single word. Single quotes
   suppress expansion; backslash escapes the next character. */
static void expand_word(const char *in, char *out, u32 max)
{
    u32 pos = 0;
    int in_single = 0, in_double = 0;
    while (*in && pos + 1 < max) {
        char c = *in++;
        if (c == '\\' && !in_single) {
            if (*in) append_char(out, &pos, max, *in++);
            continue;
        }
        if (c == '\'' && !in_double) { in_single = !in_single; continue; }
        if (c == '"' && !in_single) { in_double = !in_double; continue; }
        if (c == '$' && !in_single) {
            if (*in == '?') {
                in++;
                char n[16]; itoa(last_status, n, 10);
                for (char *p = n; *p; p++) append_char(out, &pos, max, *p);
                continue;
            }
            if (*in == '$') {
                in++;
                char n[16]; itoa(process_current_pid(), n, 10);
                for (char *p = n; *p; p++) append_char(out, &pos, max, *p);
                continue;
            }
            int braced = (*in == '{');
            if (braced) in++;
            char name[NAME_MAX]; u32 nl = 0;
            if (*in >= '0' && *in <= '9') {
                name[nl++] = *in++;
            } else {
                while ((*in == '_' || (*in >= 'a' && *in <= 'z') ||
                        (*in >= 'A' && *in <= 'Z') || (*in >= '0' && *in <= '9')) &&
                       nl + 1 < NAME_MAX)
                    name[nl++] = *in++;
            }
            name[nl] = '\0';
            if (braced && *in == '}') in++;
            const char *v = nl ? lash_get_var(name) : NULL;
            if (v) for (const char *p = v; *p; p++) append_char(out, &pos, max, *p);
            continue;
        }
        append_char(out, &pos, max, c);
    }
    out[pos] = '\0';
}

/* ---------------- parsing ---------------- */

/* Split a word list on unquoted whitespace, expanding each word. */
static int split_words(const char *s, char *outbuf, u32 outmax, char *argv[], int max)
{
    int argc = 0;
    u32 pos = 0;
    const char *p = s;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        /* Find the end of this word, respecting quotes. */
        const char *start = p;
        int in_s = 0, in_d = 0;
        while (*p) {
            if (*p == '\\' && !in_s) { p += (p[1] ? 2 : 1); continue; }
            if (*p == '\'' && !in_d) in_s = !in_s;
            else if (*p == '"' && !in_s) in_d = !in_d;
            else if (!in_s && !in_d && (*p == ' ' || *p == '\t')) break;
            p++;
        }
        u32 len = (u32)(p - start);
        if (len >= WORD_MAX) len = WORD_MAX - 1;
        char raw[WORD_MAX];
        memcpy(raw, start, len);
        raw[len] = '\0';
        char *slot = outbuf + pos;
        u32 room = outmax - pos;
        expand_word(raw, slot, room);
        u32 used = (u32)strlen(slot) + 1;
        pos += used;
        if (used > 1) argv[argc++] = slot;   /* skip empty expansions */
    }
    return argc;
}

/* ---------------- builtins ---------------- */

static int builtin_cd(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/";
    if (dir[0] != '/') {
        char cwd[128];
        lash_set_var("PWD", dir);
        (void)cwd;
    }
    lash_set_var("PWD", dir);
    syscall_set_cwd(dir);
    return 0;
}

static int run_builtin(int argc, char **argv)
{
    const char *c = argv[0];
    if (!strcmp(c, "cd")) return builtin_cd(argc, argv);
    if (!strcmp(c, "pwd")) {
        const char *p = lash_get_var("PWD");
        console_printf("%s\n", p ? p : "/");
        return 0;
    }
    if (!strcmp(c, "export")) {
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq) { *eq = '\0'; lash_set_var(argv[i], eq + 1); }
        }
        return 0;
    }
    if (!strcmp(c, "unset")) {
        for (int i = 1; i < argc; i++) unset_var(argv[i]);
        return 0;
    }
    if (!strcmp(c, "true")) return 0;
    if (!strcmp(c, "false")) return 1;
    if (!strcmp(c, "test") || !strcmp(c, "[")) {
        /* Support the common forms: test -n/-z/-f/-d STR, A = B, A != B. */
        int n = argc;
        if (!strcmp(c, "[")) n--;            /* drop the trailing ] */
        if (n == 2) return argv[1][0] ? 0 : 1;
        if (n == 3 && !strcmp(argv[1], "-n")) return argv[2][0] ? 0 : 1;
        if (n == 3 && !strcmp(argv[1], "-z")) return argv[2][0] ? 1 : 0;
        if (n == 3 && !strcmp(argv[1], "-f")) return neofs_file_exists(argv[2]) ? 0 : 1;
        if (n == 3 && !strcmp(argv[1], "-e")) return neofs_file_exists(argv[2]) ? 0 : 1;
        if (n == 4 && !strcmp(argv[2], "=")) return strcmp(argv[1], argv[3]) ? 1 : 0;
        if (n == 4 && !strcmp(argv[2], "!=")) return strcmp(argv[1], argv[3]) ? 0 : 1;
        return 1;
    }
    if (!strcmp(c, "exit")) {
        exit_requested = 1;
        exit_code = argc > 1 ? atoi(argv[1]) : last_status;
        return exit_code;
    }
    if (!strcmp(c, "return")) {
        exit_requested = 1;
        exit_code = argc > 1 ? atoi(argv[1]) : last_status;
        return exit_code;
    }
    return -1;   /* not a builtin */
}

int lash_is_builtin(const char *name)
{
    static const char *names[] = {
        "cd", "pwd", "export", "unset", "true", "false", "test", "[",
        "exit", "return", "echo", NULL
    };
    for (int i = 0; names[i]; i++)
        if (!strcmp(name, names[i])) return 1;
    return 0;
}

/* ---------------- command execution ---------------- */

/* Run one simple command (argv already expanded). Handles builtins first,
   then the kernel command table, then tries to execute a file. */
static int exec_simple(int argc, char **argv, const char *redir_out,
                       int redir_append, const u8 *stdin_buf, u32 stdin_len)
{
    if (argc == 0) return 0;

    /* Assignment-only line: NAME=value */
    char *eq = strchr(argv[0], '=');
    if (eq && !lash_is_builtin(argv[0]) && argc == 1) {
        *eq = '\0';
        lash_set_var(argv[0], eq + 1);
        return 0;
    }

    if (redir_out) {
        /* Capture output by redirecting fd 1 into a buffer, run, then write. */
        u8 *buf = kmalloc(64 * 1024);
        if (buf) {
            syscall_set_stdio(stdin_buf, stdin_len, buf, 64 * 1024, NULL);
            int rc = exec_simple(argc, argv, NULL, 0, stdin_buf, stdin_len);
            u32 len = syscall_out_length();
            syscall_reset_fds();
            int ok;
            if (redir_append) {
                /* Append: read the existing file, then write back concatenated. */
                long fsize = neofs_file_size(redir_out);
                u32 old_size = fsize > 0 ? (u32)fsize : 0;
                u8 *merged = kmalloc(old_size + len + 1);
                if (merged) {
                    u32 got = 0;
                    if (old_size) neofs_read_file(redir_out, merged, old_size, &got);
                    memcpy(merged + got, buf, len);
                    ok = neofs_write_file(redir_out, merged, got + len);
                    kfree(merged);
                    (void)ok;
                }
            } else {
                neofs_write_file(redir_out, buf, len);
            }
            kfree(buf);
            return rc;
        }
    }

    if (stdin_buf) syscall_set_stdio(stdin_buf, stdin_len, NULL, 0, NULL);

    if (argc == 1 && (!strcmp(argv[0], "echo"))) { console_printf("\n"); return 0; }
    if (!strcmp(argv[0], "echo")) {
        int nl = 1;
        int start = 1;
        if (argc > 1 && !strcmp(argv[1], "-n")) { nl = 0; start = 2; }
        for (int i = start; i < argc; i++)
            console_printf("%s%s", argv[i], i + 1 < argc ? " " : "");
        if (nl) console_printf("\n");
        return 0;
    }

    int rc = run_builtin(argc, argv);
    if (rc >= 0) return rc;

    /* Not a builtin: hand it to the kernel command table (ls, ping, ...). */
    if (shell_has_command(argv[0])) {
        shell_run_command(argc, argv);
        return last_status;
    }

    /* Finally, treat it as a file to execute (auto-detected format). */
    const char *path = argv[0];
    char buf[160];
    if (path[0] == '.' && path[1] == '/') path += 2;
    strlcpy(buf, path, sizeof(buf));
    if (neofs_file_exists(buf)) {
        int rc2 = process_run_file(buf, 0, argc, argv);
        if (rc2 < 0) { console_printf("lash: cannot execute %s\n", buf); return 126; }
        return rc2;
    }
    console_printf("lash: %s: command not found\n", argv[0]);
    return 127;
}

/* Execute a command string that may contain pipes and redirections. */
static int exec_command(const char *cmd)
{
    char work[WORD_MAX * 2];
    strlcpy(work, cmd, sizeof(work));
    str_trim(work);
    if (!work[0] || work[0] == '#') return last_status;

    /* Output redirection. */
    char *rout = NULL;
    int append = 0;
    char *gt = strchr(work, '>');
    if (gt) {
        if (gt[1] == '>') { append = 1; *gt = '\0'; gt += 2; }
        else { *gt = '\0'; gt++; }
        while (*gt == ' ' || *gt == '\t') gt++;
        char *end = gt;
        while (*end && *end != ' ' && *end != '\t') end++;
        *end = '\0';
        rout = gt;
    }

    /* Split on pipes. */
    char *stages[PIPE_MAX];
    int nstages = 0;
    char *p = work;
    while (nstages < PIPE_MAX) {
        char *bar = strchr(p, '|');
        if (!bar) { stages[nstages++] = p; break; }
        *bar = '\0';
        stages[nstages++] = p;
        p = bar + 1;
    }

    int rc = 0;
    u8 *feed = NULL;
    u32 feed_len = 0;
    for (int i = 0; i < nstages; i++) {
        char *stage = stages[i];
        str_trim(stage);
        char wordbuf[WORD_MAX];
        char *argv[ARG_MAX];
        int argc = split_words(stage, wordbuf, sizeof(wordbuf), argv, ARG_MAX);

        int last = (i + 1 == nstages);
        if (!last) {
            /* Intermediate stage: capture stdout for the next stage's stdin. */
            u8 *buf = kmalloc(64 * 1024);
            if (!buf) { if (feed) kfree(feed); return 1; }
            if (argc) {
                syscall_set_stdio(feed, feed_len, buf, 64 * 1024, NULL);
                rc = exec_simple(argc, argv, NULL, 0, feed, feed_len);
                feed_len = syscall_out_length();
                syscall_reset_fds();
            }
            if (feed) kfree(feed);
            feed = buf;
        } else {
            rc = exec_simple(argc, argv, rout, append, feed, feed_len);
        }
    }
    if (feed) kfree(feed);
    return rc;
}

/* Split on top-level ; && || and run sequentially. */
static int exec_list(const char *line)
{
    char work[WORD_MAX * 2];
    strlcpy(work, line, sizeof(work));

    char *seg[16];
    int ops[16];      /* 0 = ; , 1 = && , 2 = || */
    int n = 0;

    char *p = work;
    while (*p && n < 16) {
        char *q = p;
        int in_s = 0, in_d = 0;
        while (*q) {
            if (*q == '\'' && !in_d) in_s = !in_s;
            else if (*q == '"' && !in_s) in_d = !in_d;
            else if (!in_s && !in_d) {
                if (*q == ';') break;
                if (*q == '&' && q[1] == '&') break;
                if (*q == '|' && q[1] == '|') break;
            }
            q++;
        }
        int op = 0;
        if (*q == '&' && q[1] == '&') { op = 1; *q = '\0'; q += 2; }
        else if (*q == '|' && q[1] == '|') { op = 2; *q = '\0'; q += 2; }
        else if (*q == ';') { op = 0; *q = '\0'; q++; }
        seg[n] = p;
        ops[n] = op;
        n++;
        if (!*q) break;
        p = q;
    }

    int rc = last_status;
    for (int i = 0; i < n; i++) {
        if (i > 0) {
            int prev = ops[i - 1];
            if (prev == 1 && rc != 0) continue;    /* && : skip on failure */
            if (prev == 2 && rc == 0) continue;    /* || : skip on success */
        }
        char *s = seg[i];
        str_trim(s);
        if (!*s) continue;
        rc = exec_command(s);
        last_status = rc;
        if (exit_requested) break;
    }
    return rc;
}

int lash_exec_line(const char *line)
{
    if (!line) return last_status;
    exit_requested = 0;
    int rc = exec_list(line);
    last_status = rc;
    return rc;
}

int lash_run_script(const char *name, const char *src)
{
    script_name = name ? name : "lash";
    int rc = 0;
    char line[WORD_MAX];
    u32 lp = 0;
    const char *p = src;
    while (*p) {
        char c = *p++;
        if (c == '\r') continue;
        if (c == '\n' || lp >= sizeof(line) - 1) {
            line[lp] = '\0';
            lp = 0;
            str_trim(line);
            if (line[0] && line[0] != '#') {
                rc = lash_exec_line(line);
                if (exit_requested) break;
            }
            continue;
        }
        line[lp++] = c;
    }
    if (lp > 0 && !exit_requested) {
        line[lp] = '\0';
        str_trim(line);
        if (line[0] && line[0] != '#') rc = lash_exec_line(line);
    }
    if (exit_requested) rc = exit_code;
    return rc;
}
