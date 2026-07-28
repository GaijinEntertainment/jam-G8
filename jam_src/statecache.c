/*
 * statecache.c - persistent parse-state cache
 *
 * See statecache.h for the contract.
 *
 * File layout "JSC1" (binary, little-endian, host-local cache):
 *
 *   char[4] "JSC1", u32 format version, u32 payload checksum, u32 payload checksum
 *
 *   manifest (raw strings; validated before anything is restored):
 *     str  jam build id
 *     str  command line (args joined with \x1f)
 *     str  current directory
 *     u32  n env refs: { str name, str value, u8 was_set }
 *     u32  n includes: { str boundname, u8 existed, i64 mtime, i64 size }
 *     glob-dir section (see globdirs_write in builtins.c)
 *
 *   state:
 *     string table:  u32 n, { u32 len, bytes }
 *     variables:     u32 n, { u32 sym, list }
 *     rules:         u32 n, { u32 name, u32 flags, u8 has_actions,
 *                             [u32 actions], list bind, list params }
 *     actions:       u32 n, { u32 rulename, names targets, names sources }
 *     targets:       u32 n, { u32 name, u32 flags, names deps,
 *                             u8 has_includes,
 *                              [u32 iflags, names ideps],
 *                             u32 nact, u32 actionid[],
 *                             u32 nsettings, { u32 sym, list } }
 *     where  list  = u32 n, u32 stridx[]   and   names = list of
 *     target names.
 *
 * Save builds the whole state in memory buffers (string table in
 * first-use order) and writes the file via tmp+rename.  Load reads
 * and structurally validates the ENTIRE file into arrays first and
 * only then applies it, so a truncated or corrupt cache can never
 * leave half-restored state behind - any anomaly falls back to a
 * normal parse.
 *
 * statecache_save() refuses when any HDRRULE value - global or
 * per-target - names a rule with an interpreted body: procedures are
 * not serialized, and HDRRULE is the only rule invocation that can
 * happen after parsing.  With the DepRule* builtins active this does
 * not trigger on dagor trees (CheckOnly=yes setups fall back).
 */

# include "jam.h"

# include "lists.h"
# include "parse.h"
# include "rules.h"
# include "variable.h"
# include "newstr.h"
# include "hash.h"
# include "filesys.h"
# include "builtins.h"
# include "changedPaths.h"
# include "statecache.h"

# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <sys/types.h>
# include <sys/stat.h>

# ifdef OS_NT
# include <windows.h>
# include <direct.h>
# include <process.h>
# define sc_getcwd _getcwd
# define sc_getpid _getpid
# define sc_environ _environ
# else
# include <unistd.h>
# define sc_getcwd getcwd
# define sc_getpid getpid
extern char **environ;
# define sc_environ environ
# endif

# define SC_FORMAT	2u
# define SC_MAXSTR	( 1u << 22 )
# define SC_MAXCOUNT	( 1u << 26 )

/* ------------------------------------------------------------------ */
/* recorded inputs                                                     */

typedef struct sc_sig {
	long long	msig;		/* full-resolution mtime */
	long long	size;
} SC_SIG;

static int sc_filesig( const char *path, SC_SIG *sig );
static int sc_debug( void );

static long long
sc_now_msig( void )
{
# ifdef OS_NT
	FILETIME ft;
	GetSystemTimeAsFileTime( &ft );
	return (long long)( ( (unsigned long long)ft.dwHighDateTime << 32 )
	    | ft.dwLowDateTime );
# else
	return (long long)time( 0 ) * 10000000ll;
# endif
}

typedef struct sc_inc {
	const char	*name;		/* interned */
	int		existed;
	int		sig_ok;		/* fingerprint captured at note time */
	SC_SIG		sig;
	struct sc_inc	*next;
} SC_INC;

static SC_INC *sc_incs, *sc_incs_tail;
static char sc_argv[ 4096 ];
static int  sc_argv_over = 0;	/* command line did not fit: disable */
static const char *sc_jamver = "?";
static int  sc_state = 0;	/* 0 unknown, -1 off, 1 armed */
static int  sc_loaded = 0;	/* cache hit this run */
static int  sc_sealed = 0;	/* parse over; stop recording reads */
static long long sc_run_start;	/* racy-entry horizon (msig units) */
static char sc_path[ 1024 ];

static struct hash *sc_builtinrules;
typedef struct sc_name { const char *name; PARSE *proc; } SC_NAME;

void
statecache_note_argv( int argc, char **argv, const char *jamver )
{
	int i;
	char *w = sc_argv;
	char *end = sc_argv + sizeof( sc_argv ) - 2;

	sc_jamver = jamver;

	for( i = 0; i < argc; i++ )
	{
	    const char *a = argv[i];
	    while( *a && w < end )
		*w++ = *a++;
	    if( w < end )
		*w++ = '\x1f';
	    else
		sc_argv_over = 1;	/* truncated lines could alias */
	}
	*w = 0;
}

/*
 * Variables the parse read while they were UNSET leave no trace in the
 * variable table, but they influence the parse all the same: if such a
 * name shows up in the environment later, the outcome may change.
 * var_get() reports every miss here; the names join the env manifest
 * with "was not set".
 */

static struct hash *sc_varmiss;

void
statecache_note_varmiss( const char *symbol )
{
	SC_NAME e, *ep = &e;

	if( sc_state < 0 || sc_loaded || sc_sealed )
	    return;

	if( !sc_varmiss )
	    sc_varmiss = hashinit( sizeof( SC_NAME ), "sc var misses" );

	ep->name = symbol;
	if( hashenter( sc_varmiss, (HASHDATA **)&ep ) )
	    ep->name = newstr( symbol );
}

void
statecache_note_include( const char *boundname, int existed )
{
	SC_INC *n;

	n = (SC_INC *)malloc( sizeof( *n ) );
	n->name = newstr( boundname );
	n->existed = existed;

	/* fingerprint NOW, before the caller reads the file: an edit that
	 * lands mid-parse then mismatches on the next run (self-healing),
	 * instead of being baked in by a save-time stat.  A parsed input
	 * that cannot be fingerprinted at all (stdin via -f -, a file
	 * deleted mid-parse) forces a save refusal later. */

	n->sig_ok = existed ? sc_filesig( boundname, &n->sig ) : 0;
	n->next = 0;

	if( sc_incs_tail )
	    sc_incs_tail->next = n;
	else
	    sc_incs = n;
	sc_incs_tail = n;
}

/* parse-time ECHO transcript */

static char *sc_echo;
static size_t sc_echolen, sc_echocap;

void
statecache_note_echo( const char *text )
{
	size_t n;

	if( sc_state < 0 || sc_loaded )
	    return;

	n = strlen( text );

	if( sc_echolen + n + 1 > sc_echocap )
	{
	    size_t nc = sc_echocap ? sc_echocap * 2 : 4096;
	    while( nc < sc_echolen + n + 1 )
		nc *= 2;
	    sc_echo = (char *)realloc( sc_echo, nc );
	    sc_echocap = nc;
	}

	memcpy( sc_echo + sc_echolen, text, n );
	sc_echolen += n;
	sc_echo[ sc_echolen ] = 0;
}

/* ------------------------------------------------------------------ */
/* include-resolution tracking: while compile_include runs search(),
 * every candidate path that did NOT resolve is recorded as a must-
 * stay-missing manifest entry -- a file appearing earlier on an
 * include's SEARCH path changes the resolution and must invalidate  */

static int sc_in_inc_search;

void
statecache_include_search_begin( void )
{
	sc_in_inc_search = 1;
}

void
statecache_include_search_end( void )
{
	sc_in_inc_search = 0;
}

void
statecache_note_search_miss( const char *path )
{
	if( sc_in_inc_search && !sc_loaded )
	    statecache_note_include( path, 0 );
}

static void
sc_barrier_one( void *closure, void *data )
{
	RULE *r = (RULE *)data;
	SC_NAME e, *ep = &e;

	ep->name = r->name;
	if( hashenter( sc_builtinrules, (HASHDATA **)&ep ) )
	    {
		ep->name = r->name;
		ep->proc = r->procedure;
	    }
}

void
statecache_builtin_barrier( void )
{
	sc_builtinrules = hashinit( sizeof( SC_NAME ), "sc builtins" );
	rules_iterate( sc_barrier_one, 0 );

	/* from here on, "written" means written by the parse itself,
	 * not by the environment import / -s option startup */

	var_clear_written();
}

/*
 * sc_is_builtin_proc() - is `proc` the pristine builtin procedure
 * registered under `name` at barrier time?  A jamfile that OVERRIDES a
 * builtin (rule definitions always win) must count as interpreted: its
 * body cannot be serialized, and a cached run would silently fall back
 * to the C builtin.
 */

static int
sc_is_builtin_proc( const char *name, PARSE *proc )
{
	SC_NAME e, *ep = &e;
	if( !sc_builtinrules )
	    return 0;
	ep->name = name;
	return hashcheck( sc_builtinrules, (HASHDATA **)&ep )
	    && ep->proc == proc;
}

/*
 * Environment fingerprint.
 *
 * Hashing the whole environment would be correct but far too strict:
 * shells inject per-command noise (bash sets "_" to the program path,
 * PWD/OLDPWD/SHLVL churn) that no jamfile reads, and every such change
 * would throw the cache away.  Instead we record, for every jam
 * variable the parse actually READ (var_get), the value the environment
 * had for that name - including "was not set at all".
 *
 * That is exactly the influence the environment can have: jam turns
 * environment entries into jam variables before parsing, so a name the
 * parse never reads cannot have changed its outcome, while a name it
 * did read matters whether it was set, unset, or changed.
 */

typedef struct sc_envref {
	const char	*name;		/* interned */
	const char	*value;		/* 0 when the name was not in the env */
	struct sc_envref *next;
} SC_ENVREF;

static SC_ENVREF *sc_envrefs;
static unsigned sc_envrefcount;

static void
sc_env_collect_one( void *closure, const char *symbol, LIST *value )
{
	SC_ENVREF *r;
	const char *env;

	/* every READ variable, and also every WRITTEN one: whatever gets
	 * serialized must be env-validated, or a write-only name that
	 * shadow-restored to its startup import (local X ; X = v ;)
	 * would freeze a stale environment value with no manifest entry */

	if( !var_was_read( symbol ) && !var_was_written( symbol ) )
	    return;

	env = getenv( symbol );

	r = (SC_ENVREF *)malloc( sizeof( *r ) );
	r->name = symbol;
	r->value = env ? newstr( env ) : 0;
	r->next = sc_envrefs;
	sc_envrefs = r;
	sc_envrefcount++;
}

static void
sc_env_miss_one( void *closure, HASHDATA *data )
{
	SC_NAME *n = (SC_NAME *)data;
	SC_ENVREF *r;
	const char *env = getenv( n->name );

	/* names also present in the variable table are already emitted
	 * by sc_env_collect_one with the same (name, env value) pair; a
	 * duplicate ref would validate identically, so don't bother */

	if( var_was_read( n->name ) )
	    return;

	r = (SC_ENVREF *)malloc( sizeof( *r ) );
	r->name = n->name;
	r->value = env ? newstr( env ) : 0;
	r->next = sc_envrefs;
	sc_envrefs = r;
	sc_envrefcount++;
}

static void
sc_env_collect( void )
{
	sc_envrefs = 0;
	sc_envrefcount = 0;
	var_iterate( sc_env_collect_one, 0 );
	if( sc_varmiss )
	    hashiterate( sc_varmiss, sc_env_miss_one, 0 );
}

static const char *
sc_env_current_value( const char *name )
{
	return getenv( name );
}

static int
sc_init( void )
{
	LIST *var;

	if( sc_state )
	    return sc_state > 0;

	sc_state = -1;

	var = var_get( "JamStateCachePath" );
	if( !var || !var->string || !var->string[0] )
	    return 0;
	if( strlen( var->string ) >= sizeof( sc_path ) )
	    return 0;
	if( sc_argv_over )
	    return 0;	/* truncated command lines could alias */

	{
	    /* an unfingerprintable cwd would make the manifest's cwd
	     * check vacuous: disable instead */
	    char cwd[ 1024 ];
	    cwd[0] = 0;
	    if( !sc_getcwd( cwd, sizeof( cwd ) - 1 ) || !cwd[0] )
		return 0;
	}

	strcpy( sc_path, var->string );

	sc_run_start = sc_now_msig();
	sc_state = 1;
	return 1;
}

/* ------------------------------------------------------------------ */
/* growable byte buffer                                                */

typedef struct {
	unsigned char	*p;
	size_t		len;
	size_t		cap;
} SC_BUF;

static void
buf_need( SC_BUF *b, size_t extra )
{
	if( b->len + extra > b->cap )
	{
	    size_t nc = b->cap ? b->cap * 2 : ( 1 << 20 );
	    while( nc < b->len + extra )
		nc *= 2;
	    b->p = (unsigned char *)realloc( b->p, nc );
	    b->cap = nc;
	}
}

static void
buf_u32( SC_BUF *b, unsigned v )
{
	buf_need( b, 4 );
	b->p[ b->len++ ] = (unsigned char)v;
	b->p[ b->len++ ] = (unsigned char)( v >> 8 );
	b->p[ b->len++ ] = (unsigned char)( v >> 16 );
	b->p[ b->len++ ] = (unsigned char)( v >> 24 );
}

static void
buf_u8( SC_BUF *b, unsigned v )
{
	buf_need( b, 1 );
	b->p[ b->len++ ] = (unsigned char)v;
}

static void
buf_bytes( SC_BUF *b, const void *src, size_t n )
{
	buf_need( b, n );
	memcpy( b->p + b->len, src, n );
	b->len += n;
}

static void
buf_patch_u32( SC_BUF *b, size_t pos, unsigned v )
{
	b->p[ pos + 0 ] = (unsigned char)v;
	b->p[ pos + 1 ] = (unsigned char)( v >> 8 );
	b->p[ pos + 2 ] = (unsigned char)( v >> 16 );
	b->p[ pos + 3 ] = (unsigned char)( v >> 24 );
}

/* ------------------------------------------------------------------ */
/* save: string table                                                  */

typedef struct sc_str {
	const char	*s;	/* key */
	unsigned	idx;
} SC_STR;

static struct hash *sc_strhash;
static unsigned sc_strcount;
static SC_BUF sc_strtab;
static SC_BUF sc_body;

static unsigned
sc_stridx( const char *s )
{
	SC_STR d, *dp = &d;

	dp->s = s;
	if( hashenter( sc_strhash, (HASHDATA **)&dp ) )
	{
	    unsigned l = (unsigned)strlen( s );
	    dp->s = s;
	    dp->idx = sc_strcount++;
	    buf_u32( &sc_strtab, l );
	    buf_bytes( &sc_strtab, s, l );
	}
	return dp->idx;
}

static void
sc_wlist( SC_BUF *b, LIST *l )
{
	LIST *p;
	unsigned n = 0;
	for( p = l; p; p = list_next( p ) ) n++;
	buf_u32( b, n );
	for( p = l; p; p = list_next( p ) )
	    buf_u32( b, sc_stridx( p->string ) );
}

static void
sc_wtargets( SC_BUF *b, TARGETS *c )
{
	TARGETS *p;
	unsigned n = 0;
	for( p = c; p; p = p->next ) n++;
	buf_u32( b, n );
	for( p = c; p; p = p->next )
	    buf_u32( b, sc_stridx( p->target->name ) );
}

/* ------------------------------------------------------------------ */
/* save: sections                                                      */

static const char *sc_volatile[] = { "JAMDATE", "JAMUNAME", 0 };

static int
sc_is_volatile( const char *sym )
{
	int i;
	for( i = 0; sc_volatile[i]; i++ )
	    if( !strcmp( sym, sc_volatile[i] ) )
		return 1;
	return 0;
}

static int sc_refuse;

static void
sc_scan_hdrrule_value( LIST *v )
{
	for( ; v; v = list_next( v ) )
	{
	    RULE *r = bindrule( v->string );
	    if( r->procedure && !sc_is_builtin_proc( v->string, r->procedure ) )
	    {
		if( !sc_refuse )
		    fprintf( stderr, "jam: statecache: HDRRULE '%s' has an "
			"interpreted body (or overrides a builtin)\n", v->string );
		sc_refuse = 1;
	    }
	}
}

static void
sc_refusal_target( void *closure, void *data )
{
	TARGET *t = (TARGET *)data;
	SETTINGS *s;

	for( s = t->settings; s; s = s->next )
	    if( !strcmp( s->symbol, "HDRRULE" ) )
		sc_scan_hdrrule_value( s->value );
}

/* variables */

static unsigned sc_seccount;
static void
sc_write_var( void *cl, const char *sym, LIST *val )
{
	if( sc_is_volatile( sym ) )
	    return;

	/* only variables the PARSE wrote.  Startup-imported variables
	 * (environment, -s options) must keep the CURRENT run's values:
	 * restoring yesterday's CFLAGS over a changed environment would
	 * poison every action body.  Env influence on values the parse
	 * read or appended to is covered by the env manifest instead. */

	if( !var_was_written( sym ) )
	    return;

	buf_u32( &sc_body, sc_stridx( sym ) );
	sc_wlist( &sc_body, val );
	sc_seccount++;
}

/* rules */

static void
sc_write_rule( void *cl, void *data )
{
	RULE *r = (RULE *)data;

	/* untouched builtins are reconstructed by load_builtins() */
	if( !r->actions && !r->bindlist && !r->params && !r->flags )
	    return;

	buf_u32( &sc_body, sc_stridx( r->name ) );
	buf_u32( &sc_body, (unsigned)r->flags );
	buf_u8( &sc_body, r->actions ? 1 : 0 );
	if( r->actions )
	    buf_u32( &sc_body, sc_stridx( r->actions ) );
	sc_wlist( &sc_body, r->bindlist );
	sc_wlist( &sc_body, r->params );
	sc_seccount++;
}

/* actions: pointer -> id map, sized once */

static ACTION **sc_actslot;
static unsigned *sc_actid;
static unsigned sc_actcap;	/* power of two */
static unsigned sc_actcount;

static unsigned
sc_act_lookup( ACTION *a, int *isnew )
{
	unsigned m = sc_actcap - 1;
	unsigned i = (unsigned)( ( (size_t)a >> 4 ) * 2654435761u ) & m;

	while( sc_actslot[i] )
	{
	    if( sc_actslot[i] == a )
		{ *isnew = 0; return sc_actid[i]; }
	    i = ( i + 1 ) & m;
	}

	sc_actslot[i] = a;
	sc_actid[i] = sc_actcount;
	*isnew = 1;
	return sc_actcount++;
}

static unsigned sc_actnodes;
static void
sc_count_actions( void *cl, void *data )
{
	TARGET *t = (TARGET *)data;
	ACTIONS *a;
	for( a = t->actions; a; a = a->next )
	    sc_actnodes++;
}

static void
sc_write_action_entries( void *cl, void *data )
{
	TARGET *t = (TARGET *)data;
	ACTIONS *a;

	for( a = t->actions; a; a = a->next )
	{
	    int isnew;
	    sc_act_lookup( a->action, &isnew );
	    if( isnew )
	    {
		buf_u32( &sc_body, sc_stridx( a->action->rule->name ) );
		sc_wtargets( &sc_body, a->action->targets );
		sc_wtargets( &sc_body, a->action->sources );
	    }
	}
}

/* targets */

static void
sc_write_settings( SETTINGS *s )
{
	SETTINGS *p;
	unsigned n = 0;
	for( p = s; p; p = p->next ) n++;
	buf_u32( &sc_body, n );
	for( p = s; p; p = p->next )
	{
	    buf_u32( &sc_body, sc_stridx( p->symbol ) );
	    sc_wlist( &sc_body, p->value );
	}
}

static void
sc_write_target( void *cl, void *data )
{
	TARGET *t = (TARGET *)data;
	ACTIONS *a;
	unsigned n;

	buf_u32( &sc_body, sc_stridx( t->name ) );
	buf_u32( &sc_body, (unsigned)(unsigned char)t->flags );

	sc_wtargets( &sc_body, t->depends );

	buf_u8( &sc_body, t->includes ? 1 : 0 );
	if( t->includes )
	{
	    buf_u32( &sc_body, (unsigned)(unsigned char)t->includes->flags );
	    sc_wtargets( &sc_body, t->includes->depends );
	}

	n = 0;
	for( a = t->actions; a; a = a->next ) n++;
	buf_u32( &sc_body, n );
	for( a = t->actions; a; a = a->next )
	{
	    int isnew;
	    buf_u32( &sc_body, sc_act_lookup( a->action, &isnew ) );
	}

	sc_write_settings( t->settings );
	sc_seccount++;
}

/* ------------------------------------------------------------------ */
/* save: file-level                                                    */

static FILE *sc_f;

static void fw_u32( unsigned v )
{
	unsigned char b[4];
	b[0]=(unsigned char)v; b[1]=(unsigned char)(v>>8);
	b[2]=(unsigned char)(v>>16); b[3]=(unsigned char)(v>>24);
	fwrite( b, 1, 4, sc_f );
}

/* payload helpers: the whole file body below the 12-byte header is
 * assembled in memory so one checksum covers it -- a structurally
 * valid byte flip must never restore */

static void
buf_i64( SC_BUF *b, long long v )
{
	buf_u32( b, (unsigned)( (unsigned long long)v & 0xffffffffu ) );
	buf_u32( b, (unsigned)( (unsigned long long)v >> 32 ) );
}

static void
buf_str( SC_BUF *b, const char *s )
{
	unsigned l = (unsigned)strlen( s );
	buf_u32( b, l );
	buf_bytes( b, s, l );
}

static unsigned
sc_checksum( const unsigned char *p, size_t n )
{
	unsigned h = 5381u;
	while( n-- )
	    h = ( h * 33u ) ^ *p++;
	return h;
}

/*
 * sc_filesig() - full-resolution file signature (mirrors depcache's
 * dc_filesig): whole-second stat mtimes cannot tell two rewrites
 * within the same second apart, which matters for generated jamfiles.
 */

static int
sc_filesig( const char *path, SC_SIG *sig )
{
# ifdef OS_NT
	WIN32_FILE_ATTRIBUTE_DATA fa;

	if( !GetFileAttributesExA( path, GetFileExInfoStandard, &fa ) )
	    return 0;

	sig->msig = (long long)
	    ( ( (unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32 )
	      | fa.ftLastWriteTime.dwLowDateTime );
	sig->size = (long long)
	    ( ( (unsigned long long)fa.nFileSizeHigh << 32 )
	      | fa.nFileSizeLow );
# else
	struct stat st;

	if( stat( path, &st ) < 0 )
	    return 0;

	sig->msig = (long long)st.st_mtime * 10000000ll;
#  if defined(__linux__)
	sig->msig += st.st_mtim.tv_nsec / 100;
#  elif defined(__APPLE__)
	sig->msig += st.st_mtimespec.tv_nsec / 100;
#  endif
	sig->size = (long long)st.st_size;
# endif
	return 1;
}

static int
sc_replace( const char *tmp, const char *dst )
{
# ifdef OS_NT
	/* rename() cannot overwrite on NT; MoveFileEx replaces without
	 * a delete window, so readers see old or new, never neither */
	return MoveFileExA( tmp, dst, MOVEFILE_REPLACE_EXISTING ) ? 0 : -1;
# else
	return rename( tmp, dst );
# endif
}

static void
sc_globdir_write_one( void *closure, const char *dir, unsigned hash,
	unsigned nfiles )
{
	SC_BUF *b = (SC_BUF *)closure;
	buf_str( b, dir );
	buf_u32( b, hash );
	buf_u32( b, nfiles );
}

static void
sc_write_manifest( SC_BUF *b )
{
	unsigned n;
	SC_INC *i;
	char cwd[ 1024 ];

	sc_env_collect();

	buf_str( b, sc_jamver );
	buf_str( b, sc_argv );

	cwd[0] = 0;
	sc_getcwd( cwd, sizeof( cwd ) - 1 );
	buf_str( b, cwd );

	/* environment values of every variable the parse read */
	{
	    SC_ENVREF *r;

	    buf_u32( b, sc_envrefcount );

	    for( r = sc_envrefs; r; r = r->next )
	    {
		buf_str( b, r->name );
		buf_str( b, r->value ? r->value : "" );
		buf_u8( b, r->value ? 1 : 0 );
	    }
	}

	n = 0;
	for( i = sc_incs; i; i = i->next ) n++;
	buf_u32( b, n );

	for( i = sc_incs; i; i = i->next )
	{
	    buf_str( b, i->name );
	    if( !i->existed || !i->sig_ok )
	    {
		buf_u8( b, 0 );
		buf_i64( b, 0 );
		buf_i64( b, 0 );
	    }
	    else
	    {
		buf_u8( b, 1 );
		buf_i64( b, i->sig.msig );
		buf_i64( b, i->sig.size );
	    }
	}

	/* GLOB directory listings (order-independent fingerprints) */

	buf_u32( b, globdirs_size() );
	globdirs_iterate( sc_globdir_write_one, b );

	/* parse-time ECHO transcript */
	buf_str( b, sc_echo ? sc_echo : "" );
}

void
statecache_save( void )
{
	char tmp[ 1100 ];
	size_t patch_vars, patch_rules, patch_acts, patch_tgts;
	unsigned nvars, nrules, ntgts;
	LIST *gv;
	int ok;

	/* reads after this point (make0/make1 expansions) no longer
	 * influence the recorded parse */
	sc_sealed = 1;

	if( !sc_init() || sc_loaded )
	    return;

	/* refusal scan: HDRRULE must resolve to builtins only */

	sc_refuse = 0;
	if( ( gv = var_get( "HDRRULE" ) ) )
	    sc_scan_hdrrule_value( gv );
	targets_iterate( sc_refusal_target, 0 );

	/* racy-entry rule (as git's index): a jamfile whose timestamp is
	 * not strictly older than this run could be rewritten again
	 * within the same granularity without changing its signature.
	 * Routine race (edit + immediate build): skip quietly, the next
	 * run caches. */
	{
	    SC_INC *i;
	    for( i = sc_incs; i; i = i->next )
		if( i->existed && i->sig_ok && i->sig.msig >= sc_run_start )
		{
		    if( sc_debug() )
			fprintf( stderr, "jam: statecache: '%s' changed too "
			    "recently; state not cached\n", i->name );
		    return;
		}
	}

	/* a parsed input with no fingerprint (stdin via -f -, or a file
	 * deleted while parsing) can never validate: refuse to cache */
	{
	    SC_INC *i;
	    for( i = sc_incs; i; i = i->next )
		if( i->existed && !i->sig_ok )
		{
		    fprintf( stderr, "jam: statecache: cannot fingerprint "
			"parsed input '%s'; state not cached\n", i->name );
		    sc_refuse = 1;
		}
	}

	/* the DepRule builtins invoke null_action by name after parsing;
	 * a jamfile-defined interpreted body would be lost on a warm run */
	{
	    RULE *inc = bindrule( "Includes" );
	    if( inc->procedure && !sc_is_builtin_proc( inc->name, inc->procedure ) )
	    {
		fprintf( stderr, "jam: statecache: rule 'Includes' has an "
		    "interpreted body (or overrides a builtin)\n" );
		sc_refuse = 1;
	    }
	}
	{
	    RULE *na = bindrule( "null_action" );
	    if( na->procedure && !sc_is_builtin_proc( na->name, na->procedure ) )
	    {
		fprintf( stderr, "jam: statecache: rule 'null_action' has an "
		    "interpreted body (or overrides a builtin)\n" );
		sc_refuse = 1;
	    }
	}
	if( sc_refuse )
	    return;	/* each cause printed its own line above */

	/* build state in memory */

	sc_strhash = hashinit( sizeof( SC_STR ), "sc strings" );
	sc_strcount = 0;
	memset( &sc_strtab, 0, sizeof( sc_strtab ) );
	memset( &sc_body, 0, sizeof( sc_body ) );

	/* variables */
	patch_vars = sc_body.len; buf_u32( &sc_body, 0 );
	sc_seccount = 0;
	var_iterate( sc_write_var, 0 );
	nvars = sc_seccount;
	buf_patch_u32( &sc_body, patch_vars, nvars );

	/* rules */
	patch_rules = sc_body.len; buf_u32( &sc_body, 0 );
	sc_seccount = 0;
	rules_iterate( sc_write_rule, 0 );
	nrules = sc_seccount;
	buf_patch_u32( &sc_body, patch_rules, nrules );

	/* actions */
	sc_actnodes = 0;
	targets_iterate( sc_count_actions, 0 );
	sc_actcap = 64;
	while( sc_actcap < sc_actnodes * 4 + 64 )
	    sc_actcap *= 2;
	sc_actslot = (ACTION **)calloc( sc_actcap, sizeof( ACTION * ) );
	sc_actid = (unsigned *)calloc( sc_actcap, sizeof( unsigned ) );
	sc_actcount = 0;

	patch_acts = sc_body.len; buf_u32( &sc_body, 0 );
	targets_iterate( sc_write_action_entries, 0 );
	buf_patch_u32( &sc_body, patch_acts, sc_actcount );

	/* targets */
	patch_tgts = sc_body.len; buf_u32( &sc_body, 0 );
	sc_seccount = 0;
	targets_iterate( sc_write_target, 0 );
	ntgts = sc_seccount;
	buf_patch_u32( &sc_body, patch_tgts, ntgts );

	/* emit file: 12-byte header + checksummed payload */
	{
	    SC_BUF payload;
	    unsigned sum;
	    int ok;

	    memset( &payload, 0, sizeof( payload ) );
	    sc_write_manifest( &payload );
	    buf_u32( &payload, sc_strcount );
	    buf_bytes( &payload, sc_strtab.p, sc_strtab.len );
	    buf_bytes( &payload, sc_body.p, sc_body.len );

	    sum = sc_checksum( payload.p, payload.len );

	    /* per-process temp + atomic replace: concurrent jam
	     * processes must never write the same temp file, and
	     * readers must always see the old or the new cache */

	    sprintf( tmp, "%s.%u.tmp", sc_path, (unsigned)sc_getpid() );

	    sc_f = fopen( tmp, "wb" );
	    if( sc_f )
	    {
		setvbuf( sc_f, 0, _IOFBF, 1 << 20 );
		fwrite( "JSC1", 1, 4, sc_f );
		fw_u32( SC_FORMAT );
		fw_u32( sum );
		fwrite( payload.p, 1, payload.len, sc_f );

		ok = !ferror( sc_f );
		if( fclose( sc_f ) || !ok || sc_replace( tmp, sc_path ) )
		    remove( tmp );
	    }

	    free( payload.p );
	}

	free( sc_strtab.p );
	free( sc_body.p );
	free( sc_actslot );
	free( sc_actid );
	hashdone( sc_strhash );
	sc_strhash = 0;
}

/* ------------------------------------------------------------------ */
/* load: file-based manifest readers                                   */

static int
fr_u32( FILE *f, unsigned *v )
{
	unsigned char b[4];
	if( fread( b, 1, 4, f ) != 4 ) return 0;
	*v = (unsigned)b[0] | ((unsigned)b[1]<<8)
	   | ((unsigned)b[2]<<16) | ((unsigned)b[3]<<24);
	return 1;
}

/* memory readers over the slurped, checksum-verified payload */

typedef struct {
	const unsigned char *p;
	size_t		len;
	size_t		pos;
	int		err;
} SC_RD;

static unsigned
rd_u32( SC_RD *r )
{
	unsigned v;
	if( r->pos + 4 > r->len )
	    { r->err = 1; return 0; }
	v = (unsigned)r->p[r->pos] | ((unsigned)r->p[r->pos+1]<<8)
	  | ((unsigned)r->p[r->pos+2]<<16) | ((unsigned)r->p[r->pos+3]<<24);
	r->pos += 4;
	return v;
}

static unsigned
rd_u8( SC_RD *r )
{
	if( r->pos + 1 > r->len )
	    { r->err = 1; return 0; }
	return r->p[ r->pos++ ];
}

static long long
rd_i64( SC_RD *r )
{
	unsigned lo = rd_u32( r );
	unsigned hi = rd_u32( r );
	return (long long)( ( (unsigned long long)hi << 32 ) | lo );
}

static char *
rd_str( SC_RD *r )
{
	unsigned l = rd_u32( r );
	char *s;

	if( r->err || l >= SC_MAXSTR || r->pos + l > r->len )
	    { r->err = 1; return 0; }
	s = (char *)malloc( l + 1 );
	if( !s )
	    { r->err = 1; return 0; }
	memcpy( s, r->p + r->pos, l );
	s[l] = 0;
	r->pos += l;
	return s;
}

typedef struct {
	char		**names;
	unsigned	count;
	unsigned	cap;
} SC_PARSEDVEC;

static void
sc_pv_push( SC_PARSEDVEC *pv, char *s )
{
	if( pv->count == pv->cap )
	{
	    pv->cap = pv->cap ? pv->cap * 2 : 64;
	    pv->names = (char **)realloc( pv->names,
		pv->cap * sizeof( char * ) );
	}
	pv->names[ pv->count++ ] = s;
}

static void
sc_pv_free( SC_PARSEDVEC *pv )
{
	unsigned i;
	for( i = 0; i < pv->count; i++ )
	    free( pv->names[i] );
	free( pv->names );
	pv->names = 0;
	pv->count = pv->cap = 0;
}

static int sc_debug( void )
{
	static int d = -1;
	if( d < 0 )
	    d = getenv( "JAM_SC_DEBUG" ) ? 1 : 0;
	return d;
}

# define SC_MISS( ... ) do { if( sc_debug() ) \
	fprintf( stderr, "jam: statecache miss: " __VA_ARGS__ ); } while( 0 )

/*
 * sc_check_manifest() - validate everything that influenced the
 * cached parse.  Parsed-file names are collected and registered with
 * changedPaths only by the caller AFTER the whole load succeeded.
 */

static int
sc_check_manifest( SC_RD *r, SC_PARSEDVEC *pv, char **echo_out )
{
	char *s;
	unsigned n, i;
	char cwd[ 1024 ];

	s = rd_str( r );
	if( !s || strcmp( s, sc_jamver ) )
	    { SC_MISS( "jam version\n" ); free( s ); return 0; }
	free( s );

	s = rd_str( r );
	if( !s || strcmp( s, sc_argv ) )
	    { SC_MISS( "command line\n" ); free( s ); return 0; }
	free( s );

	cwd[0] = 0;
	sc_getcwd( cwd, sizeof( cwd ) - 1 );
	s = rd_str( r );
	if( !s || strcmp( s, cwd ) )
	    { SC_MISS( "cwd\n" ); free( s ); return 0; }
	free( s );

	/* environment variables the cached parse actually read */

	n = rd_u32( r );
	if( r->err || n > 100000 )
	    return 0;

	for( i = 0; i < n; i++ )
	{
	    char *name = rd_str( r );
	    char *val = rd_str( r );
	    unsigned had = rd_u8( r );
	    const char *now;

	    if( !name || !val || r->err )
		{ free( name ); free( val ); return 0; }

	    now = sc_env_current_value( name );

	    if( ( now != 0 ) != ( had == 1 )
		|| ( now && strcmp( now, val ) ) )
	    {
		SC_MISS( "environment variable %s\n", name );
		free( name );
		free( val );
		return 0;
	    }

	    free( name );
	    free( val );
	}

	/* files the parse read, and include attempts that must stay
	 * missing (NOCARE'd includes, unresolved SEARCH candidates) */

	n = rd_u32( r );
	if( r->err || n > 100000 )
	    return 0;

	for( i = 0; i < n; i++ )
	{
	    SC_SIG sig;
	    long long msig, size;
	    unsigned existed;
	    int present;

	    s = rd_str( r );
	    if( !s )
		return 0;
	    existed = rd_u8( r );
	    msig = rd_i64( r );
	    size = rd_i64( r );
	    if( r->err )
		{ free( s ); return 0; }

	    present = sc_filesig( s, &sig );

	    if( ( existed != 0 ) != ( present != 0 ) )
		{ SC_MISS( "file appeared/vanished: %s\n", s ); free( s ); return 0; }
	    if( existed && ( sig.msig != msig || sig.size != size ) )
		{ SC_MISS( "file changed: %s\n", s ); free( s ); return 0; }

	    if( existed )
		sc_pv_push( pv, s );
	    else
		free( s );
	}

	/* GLOB directory listings */

	n = rd_u32( r );
	if( r->err || n > 1000000 )
	    return 0;

	for( i = 0; i < n; i++ )
	{
	    char *dir = rd_str( r );
	    unsigned h, cnt, nh, nc;

	    h = rd_u32( r );
	    cnt = rd_u32( r );

	    if( !dir || r->err )
		{ free( dir ); return 0; }

	    if( !globdir_current( dir, &nh, &nc ) || nh != h || nc != cnt )
	    {
		SC_MISS( "directory listing changed: %s\n", dir );
		free( dir );
		return 0;
	    }

	    free( dir );
	}

	/* parse-time ECHO transcript: hand it to the caller, who prints
	 * it only once the whole state loaded (a decode failure after
	 * this point falls back to a parse that prints it itself) */

	*echo_out = rd_str( r );
	if( !*echo_out )
	    return 0;

	return 1;
}

/* ------------------------------------------------------------------ */
/* load: state decode + apply                                          */

/* (SC_RD and its readers are defined above, before the manifest) */

typedef struct {
	unsigned	sym;
	unsigned	nval;
	unsigned	*val;
} SC_DVAR;

typedef struct {
	unsigned	name;
	unsigned	flags;
	int		has_actions;
	unsigned	actions;
	unsigned	nbind, *bind;
	unsigned	nparams, *params;
} SC_DRULE;

typedef struct {
	unsigned	rulename;
	unsigned	ntgt, *tgt;
	unsigned	nsrc, *src;
} SC_DACT;

typedef struct {
	unsigned	sym;
	unsigned	nval, *val;
} SC_DSET;

typedef struct {
	unsigned	name;
	unsigned	flags;
	unsigned	ndeps, *deps;
	int		has_inc;
	unsigned	iflags;
	unsigned	nideps, *ideps;
	unsigned	nact, *act;
	unsigned	nset;
	SC_DSET		*set;
} SC_DTGT;

static unsigned *
rd_idxarray( SC_RD *r, unsigned *count, unsigned maxidx )
{
	unsigned n = rd_u32( r );
	unsigned *a;
	unsigned i;

	*count = 0;
	if( r->err || n > SC_MAXCOUNT )
	    { r->err = 1; return 0; }
	a = (unsigned *)malloc( ( n ? n : 1 ) * sizeof( unsigned ) );
	for( i = 0; i < n; i++ )
	{
	    a[i] = rd_u32( r );
	    if( !r->err && a[i] >= maxidx )
		r->err = 1;
	}
	*count = n;
	if( r->err )
	    { free( a ); *count = 0; return 0; }
	return a;
}

int
statecache_try_load( void )
{
	FILE *f;
	long fsize;
	unsigned sum;
	unsigned char *data = 0;
	SC_RD rd;
	char magic[4];
	unsigned i, nstr = 0, nvars = 0, nrules = 0, nacts = 0, ntgts = 0;
	const char **tab = 0;
	SC_DVAR *dvars = 0;
	SC_DRULE *drules = 0;
	SC_DACT *dacts = 0;
	SC_DTGT *dtgts = 0;
	ACTION **acts = 0;
	unsigned char *seen = 0;
	SC_PARSEDVEC pv;
	char *echo = 0;
	int success = 0;

	memset( &pv, 0, sizeof( pv ) );

	if( !sc_init() )
	    return 0;

	f = fopen( sc_path, "rb" );
	if( !f )
	    return 0;
	setvbuf( f, 0, _IOFBF, 1 << 20 );

	if( fread( magic, 1, 4, f ) != 4 || memcmp( magic, "JSC1", 4 ) )
	    { fclose( f ); return 0; }

	{
	    unsigned fmt;
	    if( !fr_u32( f, &fmt ) || fmt != SC_FORMAT )
		{ fclose( f ); return 0; }
	}
	if( !fr_u32( f, &sum ) )
	    { fclose( f ); return 0; }

	/* slurp the whole checksummed payload before trusting a byte:
	 * a structurally-valid corruption must never restore */

	fseek( f, 0, SEEK_END );
	fsize = ftell( f );
	if( fsize <= 12 )
	    { fclose( f ); return 0; }
	fseek( f, 12, SEEK_SET );

	data = (unsigned char *)malloc( fsize - 12 + 1 );
	if( !data || (long)fread( data, 1, fsize - 12, f ) != fsize - 12 )
	    { free( data ); fclose( f ); return 0; }
	data[ fsize - 12 ] = 0;	/* guard for in-place termination */
	fclose( f );

	if( sum != sc_checksum( data, fsize - 12 ) )
	    { free( data ); return 0; }

	rd.p = data; rd.len = fsize - 12; rd.pos = 0; rd.err = 0;

	if( !sc_check_manifest( &rd, &pv, &echo ) )
	    goto out;

	/* ---- decode; nothing global is touched before full success
	 * except newstr interning, which is harmless ---- */

	nstr = rd_u32( &rd );
	if( rd.err || nstr > SC_MAXCOUNT )
	    goto out;

	tab = (const char **)calloc( nstr ? nstr : 1, sizeof( char * ) );
	for( i = 0; i < nstr; i++ )
	{
	    unsigned l = rd_u32( &rd );
	    unsigned char save;
	    if( rd.err || l >= SC_MAXSTR || rd.pos + l > rd.len )
		goto out;
	    save = rd.p[ rd.pos + l ];
	    ( (unsigned char *)rd.p )[ rd.pos + l ] = 0;
	    tab[i] = newstr( (const char *)rd.p + rd.pos );
	    ( (unsigned char *)rd.p )[ rd.pos + l ] = save;
	    rd.pos += l;
	}

	nvars = rd_u32( &rd );
	if( rd.err || nvars > SC_MAXCOUNT ) goto out;
	dvars = (SC_DVAR *)calloc( nvars ? nvars : 1, sizeof( SC_DVAR ) );
	for( i = 0; i < nvars; i++ )
	{
	    dvars[i].sym = rd_u32( &rd );
	    if( rd.err || dvars[i].sym >= nstr ) goto out;
	    dvars[i].val = rd_idxarray( &rd, &dvars[i].nval, nstr );
	    if( rd.err ) goto out;
	}

	nrules = rd_u32( &rd );
	if( rd.err || nrules > SC_MAXCOUNT ) goto out;
	drules = (SC_DRULE *)calloc( nrules ? nrules : 1, sizeof( SC_DRULE ) );
	for( i = 0; i < nrules; i++ )
	{
	    SC_DRULE *r = &drules[i];
	    r->name = rd_u32( &rd );
	    r->flags = rd_u32( &rd );
	    r->has_actions = (int)rd_u8( &rd );
	    if( r->has_actions )
		r->actions = rd_u32( &rd );
	    if( rd.err || r->name >= nstr
		|| ( r->has_actions && r->actions >= nstr ) ) goto out;
	    r->bind = rd_idxarray( &rd, &r->nbind, nstr );
	    if( rd.err ) goto out;
	    r->params = rd_idxarray( &rd, &r->nparams, nstr );
	    if( rd.err ) goto out;
	}

	nacts = rd_u32( &rd );
	if( rd.err || nacts > SC_MAXCOUNT ) goto out;
	dacts = (SC_DACT *)calloc( nacts ? nacts : 1, sizeof( SC_DACT ) );
	for( i = 0; i < nacts; i++ )
	{
	    dacts[i].rulename = rd_u32( &rd );
	    if( rd.err || dacts[i].rulename >= nstr ) goto out;
	    dacts[i].tgt = rd_idxarray( &rd, &dacts[i].ntgt, nstr );
	    if( rd.err ) goto out;
	    dacts[i].src = rd_idxarray( &rd, &dacts[i].nsrc, nstr );
	    if( rd.err ) goto out;
	}

	ntgts = rd_u32( &rd );
	if( rd.err || ntgts > SC_MAXCOUNT ) goto out;
	dtgts = (SC_DTGT *)calloc( ntgts ? ntgts : 1, sizeof( SC_DTGT ) );
	seen = (unsigned char *)calloc( nstr ? nstr : 1, 1 );
	if( !dtgts || !seen ) goto out;
	for( i = 0; i < ntgts; i++ )
	{
	    SC_DTGT *t = &dtgts[i];
	    unsigned j;

	    t->name = rd_u32( &rd );
	    t->flags = rd_u32( &rd );
	    if( rd.err || t->name >= nstr ) goto out;

	    /* a crafted file with the same target twice would merge
	     * entries and REPLACE the first one's settings: reject */
	    if( seen[ t->name ]++ ) goto out;
	    t->deps = rd_idxarray( &rd, &t->ndeps, nstr );
	    if( rd.err ) goto out;
	    t->has_inc = (int)rd_u8( &rd );
	    if( t->has_inc )
	    {
		t->iflags = rd_u32( &rd );
		t->ideps = rd_idxarray( &rd, &t->nideps, nstr );
		if( rd.err ) goto out;
	    }
	    t->act = rd_idxarray( &rd, &t->nact, nacts );
	    if( rd.err ) goto out;
	    t->nset = rd_u32( &rd );
	    if( rd.err || t->nset > SC_MAXCOUNT ) goto out;
	    t->set = (SC_DSET *)calloc( t->nset ? t->nset : 1, sizeof( SC_DSET ) );
	    for( j = 0; j < t->nset; j++ )
	    {
		t->set[j].sym = rd_u32( &rd );
		if( rd.err || t->set[j].sym >= nstr ) goto out;
		t->set[j].val = rd_idxarray( &rd, &t->set[j].nval, nstr );
		if( rd.err ) goto out;
	    }
	}

	if( rd.err || rd.pos != rd.len )
	    goto out;

	/* ---- apply ---- */

	for( i = 0; i < nvars; i++ )
	{
	    LIST *l = L0;
	    unsigned j;
	    if( sc_is_volatile( tab[ dvars[i].sym ] ) )
		continue;
	    for( j = 0; j < dvars[i].nval; j++ )
		l = list_new( l, tab[ dvars[i].val[j] ], 1 );
	    var_set( tab[ dvars[i].sym ], l, VAR_SET );
	}

	for( i = 0; i < nrules; i++ )
	{
	    RULE *r = bindrule( tab[ drules[i].name ] );
	    unsigned j;
	    LIST *l;

	    r->flags = (int)drules[i].flags;
	    if( drules[i].has_actions )
		r->actions = tab[ drules[i].actions ];

	    l = L0;
	    for( j = 0; j < drules[i].nbind; j++ )
		l = list_new( l, tab[ drules[i].bind[j] ], 1 );
	    r->bindlist = l;

	    l = L0;
	    for( j = 0; j < drules[i].nparams; j++ )
		l = list_new( l, tab[ drules[i].params[j] ], 1 );
	    r->params = l;
	}

	acts = (ACTION **)calloc( nacts ? nacts : 1, sizeof( ACTION * ) );
	for( i = 0; i < nacts; i++ )
	{
	    ACTION *a = (ACTION *)malloc( sizeof( ACTION ) );
	    TARGETS *c;
	    unsigned j;

	    memset( a, 0, sizeof( *a ) );
	    a->rule = bindrule( tab[ dacts[i].rulename ] );

	    c = 0;
	    for( j = 0; j < dacts[i].ntgt; j++ )
		c = targetentry( c,
		    bindtarget_interned( tab[ dacts[i].tgt[j] ] ) );
	    a->targets = c;

	    c = 0;
	    for( j = 0; j < dacts[i].nsrc; j++ )
		c = targetentry( c,
		    bindtarget_interned( tab[ dacts[i].src[j] ] ) );
	    a->sources = c;

	    acts[i] = a;
	}

	for( i = 0; i < ntgts; i++ )
	{
	    SC_DTGT *dt = &dtgts[i];
	    TARGET *t = bindtarget( tab[ dt->name ] );
	    unsigned j;
	    SETTINGS *tail = 0;

	    t->flags = (char)dt->flags;

	    for( j = 0; j < dt->ndeps; j++ )
		t->depends = targetentry( t->depends,
		    bindtarget_interned( tab[ dt->deps[j] ] ) );

	    if( dt->has_inc )
	    {
		if( !t->includes )
		    t->includes = copytarget( t );
		t->includes->flags = (char)dt->iflags;
		for( j = 0; j < dt->nideps; j++ )
		    t->includes->depends = targetentry( t->includes->depends,
			bindtarget_interned( tab[ dt->ideps[j] ] ) );
	    }

	    for( j = 0; j < dt->nact; j++ )
		t->actions = actionlist( t->actions, acts[ dt->act[j] ] );

	    for( j = 0; j < dt->nset; j++ )
	    {
		SETTINGS *v = (SETTINGS *)malloc( sizeof( *v ) );
		LIST *l = L0;
		unsigned k;

		for( k = 0; k < dt->set[j].nval; k++ )
		    l = list_new( l, tab[ dt->set[j].val[k] ], 1 );

		v->symbol = tab[ dt->set[j].sym ];
		v->value = l;
		v->next = 0;

		if( tail )
		    tail->next = v;
		else
		    t->settings = v;
		tail = v;
	    }
	}

	/* only now publish the parsed-file list and replay the parse-time
	 * ECHO output */

	for( i = 0; i < pv.count; i++ )
	    changed_paths_register_parsed_file( pv.names[i] );

	fputs( echo, stdout );

	sc_loaded = 1;
	success = 1;

    out:
	free( echo );
	sc_pv_free( &pv );
	if( tab ) free( (void *)tab );
	if( dvars )
	{
	    for( i = 0; i < nvars; i++ ) free( dvars[i].val );
	    free( dvars );
	}
	if( drules )
	{
	    for( i = 0; i < nrules; i++ )
		{ free( drules[i].bind ); free( drules[i].params ); }
	    free( drules );
	}
	if( dacts )
	{
	    for( i = 0; i < nacts; i++ )
		{ free( dacts[i].tgt ); free( dacts[i].src ); }
	    free( dacts );
	}
	if( dtgts )
	{
	    for( i = 0; i < ntgts; i++ )
	    {
		unsigned j;
		free( dtgts[i].deps );
		free( dtgts[i].ideps );
		free( dtgts[i].act );
		for( j = 0; j < dtgts[i].nset; j++ )
		    free( dtgts[i].set[j].val );
		free( dtgts[i].set );
	    }
	    free( dtgts );
	}
	if( acts ) free( acts );
	free( seen );
	free( data );

	return success;
}
