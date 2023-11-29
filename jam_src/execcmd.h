/*
 * Copyright 1993, 1995 Christopher Seiwald.
 *
 * This file is part of Jam - see jam.c for Copyright information.
 */

/*
 * execcmd.h - execute a shell script
 *
 * 05/04/94 (seiwald) - async multiprocess interface
 */

void exec_prepare();
void exec_finish();

typedef struct _execstats EXECSTATS;

struct _execstats {
	int index;
	int estimated_id;
} ;


void execcmd(
	char *string,
	void (*func)( void *closure, int status ),
	void *closure,
	LIST *shell,
	EXECSTATS execstats);

int execwait();

# define EXEC_CMD_OK	0
# define EXEC_CMD_FAIL	1
# define EXEC_CMD_INTR	2
