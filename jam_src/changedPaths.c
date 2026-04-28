/*
 * Copyright 2025 Gaijin Games KFT.
 *
 * This file is part of jam-G8 - see jam.c for Copyright information.
 */

/*
 * changedPaths.c - implementation of the JamChangedFilesPath early-exit
 *
 * See changedPaths.h for the contract.
 *
 * Internal layout:
 *   - g_files   : open-addressing hash table of canonicalized file paths
 *   - g_dirs    : dynamic array of canonicalized directory prefixes
 *   - g_walk_gen: incremented each call to changed_paths_test; compared
 *                 against TARGET::walk_gen to mark visited nodes.
 *
 * Path canonicalization:
 *   - replace '\\' with '/'
 *   - collapse runs of '/'
 *   - strip leading "./"
 *   - strip trailing '/'
 *   - downshift ASCII when DOWNSHIFT_PATHS is defined (Windows etc.)
 *   Interior ".." segments are NOT resolved -- the typical caller
 *   ("git diff --name-only") never emits them.  See design.md.
 */

# include "jam.h"

# include "lists.h"
# include "parse.h"
# include "rules.h"
# include "variable.h"
# include "filesys.h"
# include "changedPaths.h"

# include <sys/stat.h>

# ifdef NT
#  include <direct.h>
#  ifndef S_ISDIR
#   define S_ISDIR( m ) ( ( (m) & _S_IFMT ) == _S_IFDIR )
#  endif
# endif

# ifdef DOWNSHIFT_PATHS
#  define CP_DOWNSHIFT 1
# else
#  define CP_DOWNSHIFT 0
# endif

/* --- containers --- */

typedef struct {
	char **slots;
	int    cap;
	int    count;
} cp_FileSet;

typedef struct {
	char **arr;
	int    cap;
	int    count;
} cp_DirVec;

static cp_FileSet g_files;
static cp_DirVec  g_dirs;
static int        g_loaded   = 0;
static int        g_walk_gen = 0;

/* --- small string utilities --- */

static char *
cp_xstrdup( const char *s )
{
	int n = (int)strlen( s ) + 1;
	char *p = (char *)malloc( n );
	if ( p )
	    memcpy( p, s, n );
	return p;
}

static unsigned
cp_hashstr( const char *s )
{
	unsigned h = 5381u;
	while ( *s )
	    h = ( h * 33u ) ^ (unsigned char)*s++;
	return h;
}

/*
 * cp_canon - canonicalize a path in place.
 * Result length is <= input length.  See file header for rules.
 */

static void
cp_canon( char *p )
{
	char *r;
	char *w;
	int   prev_slash;
	int   len;

	/* unify separators; downshift ASCII if requested */
	for ( r = p; *r; r++ ) {
	    if ( *r == '\\' )
		*r = '/';
	    if ( CP_DOWNSHIFT && *r >= 'A' && *r <= 'Z' )
		*r = (char)( *r + 32 );
	}

	/* collapse runs of '/' */
	w = p;
	r = p;
	prev_slash = 0;
	while ( *r ) {
	    if ( *r == '/' ) {
		if ( !prev_slash )
		    *w++ = '/';
		prev_slash = 1;
	    } else {
		*w++ = *r;
		prev_slash = 0;
	    }
	    r++;
	}
	*w = 0;

	/* strip any number of leading "./" */
	while ( p[0] == '.' && p[1] == '/' ) {
	    memmove( p, p + 2, strlen( p + 2 ) + 1 );
	}

	/* strip trailing '/' (but never make a non-empty string empty) */
	len = (int)strlen( p );
	if ( len > 1 && p[len - 1] == '/' )
	    p[len - 1] = 0;
}

/* --- file hash set (open addressing, linear probing) --- */

static void cp_fs_grow( cp_FileSet *fs );

static void
cp_fs_init( cp_FileSet *fs )
{
	fs->cap   = 64;
	fs->count = 0;
	fs->slots = (char **)calloc( fs->cap, sizeof( char * ) );
}

static void
cp_fs_free( cp_FileSet *fs )
{
	int i;
	if ( !fs->slots )
	    return;
	for ( i = 0; i < fs->cap; i++ )
	    if ( fs->slots[i] )
		free( fs->slots[i] );
	free( fs->slots );
	fs->slots = 0;
	fs->cap = fs->count = 0;
}

static void
cp_fs_insert( cp_FileSet *fs, const char *s )
{
	unsigned h;
	unsigned mask;
	unsigned i;

	if ( ( fs->count * 2 ) >= fs->cap )
	    cp_fs_grow( fs );

	mask = (unsigned)( fs->cap - 1 );
	h = cp_hashstr( s );
	i = h & mask;
	while ( fs->slots[i] ) {
	    if ( strcmp( fs->slots[i], s ) == 0 )
		return; /* already present */
	    i = ( i + 1 ) & mask;
	}
	fs->slots[i] = cp_xstrdup( s );
	fs->count++;
}

static int
cp_fs_contains( const cp_FileSet *fs, const char *s )
{
	unsigned h;
	unsigned mask;
	unsigned i;

	if ( !fs->cap )
	    return 0;
	mask = (unsigned)( fs->cap - 1 );
	h = cp_hashstr( s );
	i = h & mask;
	while ( fs->slots[i] ) {
	    if ( strcmp( fs->slots[i], s ) == 0 )
		return 1;
	    i = ( i + 1 ) & mask;
	}
	return 0;
}

static void
cp_fs_grow( cp_FileSet *fs )
{
	int    old_cap   = fs->cap;
	char **old_slots = fs->slots;
	int    i;

	fs->cap   = old_cap * 2;
	fs->slots = (char **)calloc( fs->cap, sizeof( char * ) );
	fs->count = 0;
	for ( i = 0; i < old_cap; i++ ) {
	    if ( old_slots[i] ) {
		cp_fs_insert( fs, old_slots[i] );
		free( old_slots[i] );
	    }
	}
	free( old_slots );
}

/* --- directory vector (small N expected) --- */

static void
cp_dv_init( cp_DirVec *dv )
{
	dv->arr   = 0;
	dv->cap   = 0;
	dv->count = 0;
}

static void
cp_dv_free( cp_DirVec *dv )
{
	int i;
	for ( i = 0; i < dv->count; i++ )
	    free( dv->arr[i] );
	free( dv->arr );
	dv->arr = 0;
	dv->cap = dv->count = 0;
}

static void
cp_dv_push( cp_DirVec *dv, const char *s )
{
	int i;

	for ( i = 0; i < dv->count; i++ )
	    if ( strcmp( dv->arr[i], s ) == 0 )
		return; /* dedupe */

	if ( dv->count == dv->cap ) {
	    dv->cap = dv->cap ? dv->cap * 2 : 8;
	    dv->arr = (char **)realloc( dv->arr,
		(size_t)dv->cap * sizeof( char * ) );
	}
	dv->arr[dv->count++] = cp_xstrdup( s );
}

static int
cp_dv_contains_prefix( const cp_DirVec *dv, const char *p )
{
	int i;
	int dl;
	const char *d;

	for ( i = 0; i < dv->count; i++ ) {
	    d  = dv->arr[i];
	    if ( !d[0] )
		return 1;	/* universal-match (cwd "." entry) */
	    dl = (int)strlen( d );
	    if ( strncmp( p, d, (size_t)dl ) == 0 &&
		 ( p[dl] == '/' || p[dl] == 0 ) )
		return 1;
	}
	return 0;
}

/* --- directory test (used to classify list entries that lack a trailing /) --- */

static int
cp_is_directory( const char *p )
{
	struct stat st;
	if ( stat( p, &st ) != 0 )
	    return 0;
	return S_ISDIR( st.st_mode );
}

/* --- public: load --- */

int
changed_paths_load( const char *list_file )
{
	FILE *f;
	char  line[4096];

	if ( g_loaded ) {
	    cp_fs_free( &g_files );
	    cp_dv_free( &g_dirs );
	    g_loaded = 0;
	}
	cp_fs_init( &g_files );
	cp_dv_init( &g_dirs );

	f = fopen( list_file, "r" );
	if ( !f ) {
	    fprintf( stderr,
		"jam: JamChangedFilesPath: cannot open '%s'\n", list_file );
	    return 0;
	}

	while ( fgets( line, sizeof( line ), f ) ) {
	    char *p = line;
	    char *e;
	    int   is_dir;

	    /* trim leading whitespace */
	    while ( *p == ' ' || *p == '\t' )
		p++;

	    /* trim trailing whitespace + CR/LF */
	    e = p + strlen( p );
	    while ( e > p && ( e[-1] == '\n' || e[-1] == '\r' ||
			       e[-1] == ' '  || e[-1] == '\t' ) )
		*--e = 0;

	    /* skip empty / comment */
	    if ( *p == 0 || *p == '#' )
		continue;

	    /* trailing slash explicitly marks this entry as a directory */
	    is_dir = ( e > p && ( e[-1] == '/' || e[-1] == '\\' ) );
	    if ( is_dir )
		*--e = 0;

	    cp_canon( p );

	    /* If no explicit trailing slash, stat the path to classify it. */
	    if ( !is_dir )
		is_dir = cp_is_directory( p );

	    /*
	     * Normalize "." (cwd) to "" so universal-match logic in
	     * cp_dv_contains_prefix kicks in.  An empty entry is silently
	     * dropped from the file set (it cannot match any real file).
	     */
	    if ( p[0] == '.' && p[1] == 0 )
		p[0] = 0;

	    if ( is_dir )
		cp_dv_push( &g_dirs, p );
	    else if ( p[0] )
		cp_fs_insert( &g_files, p );
	}
	fclose( f );

	g_loaded = 1;

	if ( DEBUG_DEPENDS )
	    printf( "JamChangedFilesPath: loaded %d files, %d dirs from '%s'\n",
		g_files.count, g_dirs.count, list_file );

	return 1;
}

/* --- public: free --- */

void
changed_paths_free( void )
{
	if ( !g_loaded )
	    return;
	cp_fs_free( &g_files );
	cp_dv_free( &g_dirs );
	g_loaded = 0;
}

/* --- internal: recursive walk --- */

static int
cp_walk( TARGET *t, int gen )
{
	TARGETS *c;

	if ( !t )
	    return 0;
	if ( t->walk_gen == gen )
	    return 0;
	t->walk_gen = gen;

	/* skip phony (NOTFILE) and internal includes container */
	if ( !( t->flags & ( T_FLAG_NOTFILE | T_FLAG_INTERNAL ) ) ) {
	    const char *bn;
	    char        norm_buf[2048];
	    char       *norm;
	    int         n;

	    bn = ( t->boundname && t->boundname[0] ) ? t->boundname : t->name;
	    if ( bn && bn[0] ) {
		n = (int)strlen( bn );
		if ( n + 1 > (int)sizeof( norm_buf ) )
		    norm = (char *)malloc( (size_t)n + 1 );
		else
		    norm = norm_buf;
		memcpy( norm, bn, (size_t)n + 1 );
		cp_canon( norm );

		if ( cp_fs_contains( &g_files, norm ) ||
		     cp_dv_contains_prefix( &g_dirs, norm ) ) {
		    if ( DEBUG_DEPENDS )
			printf( "JamChangedFilesPath: matched '%s' (target '%s')\n",
			    norm, t->name );
		    if ( norm != norm_buf )
			free( norm );
		    return 1;
		}
		if ( norm != norm_buf )
		    free( norm );
	    }
	}

	for ( c = t->depends; c; c = c->next )
	    if ( cp_walk( c->target, gen ) )
		return 1;

	if ( t->includes && cp_walk( t->includes, gen ) )
	    return 1;

	return 0;
}

/* --- public: test one root target --- */

int
changed_paths_test( TARGET *t )
{
	g_walk_gen++;
	if ( !g_walk_gen ) /* skip 0 (the un-visited sentinel) on wraparound */
	    g_walk_gen = 1;
	return cp_walk( t, g_walk_gen );
}
