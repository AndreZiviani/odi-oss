/* The command handlers, one per enum cmd_id (table.h). */
#ifndef ODI_COMMANDS_H
#define ODI_COMMANDS_H

#include "parse.h"

/* Run a parsed command. Returns its exit status: 0 on success. */
int cmd_run(const struct parsed *p);

/* The most commands one listing holds; well above the table size. */
#define CMD_LIST_MAX 64

/* Print every command in `list`, syntax and description, one per line. */
void cmd_list(const struct cmd_def *const list[], int n);

/* True once `exit` has run and the shell should stop reading input. */
int cmd_should_quit(void);

#endif
