#ifndef NEO_LASH_H
#define NEO_LASH_H

#include <nitekernel.h>

/* lash: the NiteKernel shell language. A bash-compatible subset with
   pipelines, redirection, &&/||/;, variables, and the usual builtins.
   The same interpreter runs interactive lines and .lash scripts. */

void lash_init(void);

/* Execute one command line. Returns the exit status. */
int  lash_exec_line(const char *line);

/* Run a whole script. `name` is used for $0 and diagnostics. */
int  lash_run_script(const char *name, const char *src);

/* Last exit status, exposed to the interactive shell as $?. */
int  lash_last_status(void);

/* Variable table helpers (used by the builtins and by $VAR expansion). */
const char *lash_get_var(const char *name);
void lash_set_var(const char *name, const char *value);

/* True if `name` is a lash builtin. */
int  lash_is_builtin(const char *name);

#endif
