/*
 * Copyright 1993, 2000 Christopher Seiwald.
 *
 * This file is part of Jam - see jam.c for Copyright information.
 */

/*
 * variable.h - handle jam multi-element variables
 *
 * 11/04/02 (seiwald) - const-ing for string literals
 */

void 	var_defines( const char **e );
int 	var_string( const char *in, char *out, int outsize, LOL *lol );
LIST * 	var_get( const char *symbol );
void 	var_set( const char *symbol, LIST *value, int flag );
LIST * 	var_swap( const char *symbol, LIST *value );
void 	var_done();
void 	var_iterate( void (*func)( void *closure, const char *symbol, LIST *value ), void *closure );
/* was the variable ever read via var_get()?  used by the parse-state cache */
int 	var_was_read( const char *symbol );
/* was it written by var_set() since var_clear_written()? */
int 	var_was_written( const char *symbol );
void 	var_clear_written( void );

/*
 * Defines for var_set().
 */

# define VAR_SET	0	/* override previous value */
# define VAR_APPEND	1	/* append to previous value */
# define VAR_DEFAULT	2	/* set only if no previous value */

