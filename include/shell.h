#ifndef NEO_SHELL_H
#define NEO_SHELL_H

#include <nitekernel.h>

void shell_init(void);
void shell_run(void) __attribute__((noreturn));

/* Feed a character directly (used by scripted/headless input). */
void shell_input(char c);

/* Run a single command line. */
int  shell_execute(const char *line);

/* The kernel's own command table, exposed so the lash interpreter can call
   built-in commands (ls, ping, report, ...) from scripts and pipelines. */
int  shell_has_command(const char *name);
int  shell_run_command(int argc, char **argv);

#endif
