/*
 * Copyright 1993-2002 Christopher Seiwald and Perforce Software, Inc.
 *
 * This file is part of Jam - see jam.c for Copyright information.
 */

/*
 * builtins.h - compile parsed jam statements
 *
 * 01/10/01 (seiwald) - split from compile.h
 */

void load_builtins();
void load_strrules();

/* GLOB directory-listing fingerprints for the parse-state cache */
# include <stdio.h>
/* GLOB directory-listing fingerprints (parse-state cache manifest) */
unsigned globdirs_size( void );
void globdirs_iterate( void (*func)( void *closure, const char *dir,
		unsigned hash, unsigned nfiles ), void *closure );
int  globdir_current( const char *dir, unsigned *hash, unsigned *nfiles );

