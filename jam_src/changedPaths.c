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

/*
 * Both the changed-paths list entries and the walked TARGET boundnames
 * are resolved to absolute paths before comparison.  Entries are
 * resolved against `g_changed_root`; boundnames against `g_jam_cwd`.
 * Both anchors are themselves canonicalized at load time.
 */
static char       g_jam_cwd[1024];
static char       g_changed_root[1024];

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
 * cp_make_abs - resolve `in` to an absolute-ish path using `anchor` as
 * the base when `in` is relative.  Result is written to `out` (NUL-
 * terminated).  Does NOT do .. / . resolution -- that's cp_canon's job.
 *
 * If `in` is already absolute (Unix "/x" or Windows "<drv>:..." or
 * "\\x"), `anchor` is ignored.  If `anchor` is NULL or empty, `in` is
 * copied as-is.
 */

static void
cp_make_abs( char *out, int out_size, const char *in, const char *anchor )
{
	int absolute;
	int al;
	int il;
	int need_sep;
	int total;

	if ( out_size <= 0 )
	    return;
	out[0] = 0;
	if ( !in )
	    return;

	absolute = 0;
	if ( in[0] == '/' || in[0] == '\\' )
	    absolute = 1;
	else if ( ( ( in[0] >= 'a' && in[0] <= 'z' ) ||
		    ( in[0] >= 'A' && in[0] <= 'Z' ) )
		  && in[1] == ':' )
	    absolute = 1;

	if ( absolute || !anchor || !anchor[0] ) {
	    il = (int)strlen( in );
	    if ( il >= out_size )
		il = out_size - 1;
	    memcpy( out, in, (size_t)il );
	    out[il] = 0;
	    return;
	}

	al = (int)strlen( anchor );
	il = (int)strlen( in );
	need_sep = ( al > 0 && anchor[al - 1] != '/' && anchor[al - 1] != '\\' );
	total = al + ( need_sep ? 1 : 0 ) + il;
	if ( total >= out_size ) {
	    /* truncate defensively */
	    if ( al >= out_size )
		al = out_size - 1;
	    memcpy( out, anchor, (size_t)al );
	    out[al] = 0;
	    return;
	}
	memcpy( out, anchor, (size_t)al );
	if ( need_sep )
	    out[al] = '/';
	memcpy( out + al + ( need_sep ? 1 : 0 ), in, (size_t)il );
	out[total] = 0;
}

/*
 * cp_canon - canonicalize a path in place:
 *   - replace '\\' with '/'
 *   - downshift ASCII when DOWNSHIFT_PATHS is defined
 *   - collapse runs of '/'
 *   - resolve '.' and '..' segments lexically (no symlink follow)
 *   - strip trailing '/' (except for "/" itself)
 * Result length is <= input length.  Operates on absolute or relative
 * paths; for relative paths, leading ".." is preserved (cannot pop).
 */

#define CP_MAX_SEGS 256

static void
cp_canon( char *p )
{
	char *seg_ptr[CP_MAX_SEGS];
	int   seg_len[CP_MAX_SEGS];
	int   n_segs = 0;
	int   prefix_len = 0;
	int   absolute = 0;
	char *r;
	char *w;
	char *seg_start;
	int   slen;
	int   is_dot;
	int   is_dotdot;
	int   i;

	if ( !p || !*p )
	    return;

	/* Step 1: unify separators, downshift. */
	for ( r = p; *r; r++ ) {
	    if ( *r == '\\' )
		*r = '/';
	    if ( CP_DOWNSHIFT && *r >= 'A' && *r <= 'Z' )
		*r = (char)( *r + 32 );
	}

	/* Step 2: identify the immutable prefix (drive letter or leading slash). */
	if ( p[0] >= 'a' && p[0] <= 'z' && p[1] == ':' ) {
	    if ( p[2] == '/' ) {
		prefix_len = 3;	/* "c:/" - absolute on drive */
		absolute   = 1;
	    } else {
		prefix_len = 2;	/* "c:" - per-drive cwd; treat as absolute */
		absolute   = 1;
	    }
	} else if ( p[0] == '/' ) {
	    prefix_len = 1;
	    absolute   = 1;
	}

	/* Step 3: tokenize segments after prefix; classify and stack. */
	r = p + prefix_len;
	while ( *r ) {
	    while ( *r == '/' )
		r++;
	    if ( !*r )
		break;
	    seg_start = r;
	    while ( *r && *r != '/' )
		r++;
	    slen = (int)( r - seg_start );

	    is_dot    = ( slen == 1 && seg_start[0] == '.' );
	    is_dotdot = ( slen == 2 && seg_start[0] == '.' && seg_start[1] == '.' );

	    if ( is_dot ) {
		continue;
	    } else if ( is_dotdot ) {
		if ( n_segs > 0 ) {
		    int top_is_dotdot =
			( seg_len[n_segs - 1] == 2 &&
			  seg_ptr[n_segs - 1][0] == '.' &&
			  seg_ptr[n_segs - 1][1] == '.' );
		    if ( !top_is_dotdot ) {
			n_segs--;
			continue;
		    }
		}
		if ( !absolute && n_segs < CP_MAX_SEGS ) {
		    seg_ptr[n_segs] = seg_start;
		    seg_len[n_segs] = slen;
		    n_segs++;
		}
		/* absolute & stack empty: drop ".." (cannot escape root) */
	    } else {
		if ( n_segs < CP_MAX_SEGS ) {
		    seg_ptr[n_segs] = seg_start;
		    seg_len[n_segs] = slen;
		    n_segs++;
		}
	    }
	}

	/* Step 4: rebuild in place.  Source segments are at monotonically
	 * increasing offsets and total length never grows, so memmove is
	 * safe even when src/dst overlap. */
	w = p + prefix_len;
	for ( i = 0; i < n_segs; i++ ) {
	    if ( i > 0 ) {
		*w++ = '/';
	    } else if ( absolute && prefix_len > 0 && p[prefix_len - 1] != '/' ) {
		*w++ = '/';
	    }
	    memmove( w, seg_ptr[i], (size_t)seg_len[i] );
	    w += seg_len[i];
	}
	*w = 0;

	/* If we ended up with just a prefix + nothing, ensure the prefix
	 * is preserved (it already is; this is just the empty result case). */
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

/*
 * Read one jam variable into a buffer (NUL-terminated, canonicalized).
 * Returns 1 if the variable was set and non-empty, 0 otherwise.
 */
static int
cp_read_var( const char *name, char *out, int out_size )
{
	LIST *l;

	out[0] = 0;
	l = var_get( name );
	if ( !l || !l->string || !l->string[0] )
	    return 0;

	{
	    int n = (int)strlen( l->string );
	    if ( n >= out_size )
		n = out_size - 1;
	    memcpy( out, l->string, (size_t)n );
	    out[n] = 0;
	}
	cp_canon( out );
	return 1;
}

int
changed_paths_load( const char *list_file )
{
	FILE *f;
	char  line[4096];
	char  abs[8192];

	if ( g_loaded ) {
	    cp_fs_free( &g_files );
	    cp_dv_free( &g_dirs );
	    g_loaded = 0;
	}
	cp_fs_init( &g_files );
	cp_dv_init( &g_dirs );

	/*
	 * Resolve the two anchors:
	 *   g_jam_cwd      = $(JAM_CWD)             -- jam's getcwd() at startup
	 *   g_changed_root = $(JamChangedPathsRoot) (default = JAM_CWD)
	 * Both are canonicalized (absolute, lowercase if DOWNSHIFT_PATHS,
	 * with .. and . resolved).
	 */
	if ( !cp_read_var( "JAM_CWD", g_jam_cwd, sizeof( g_jam_cwd ) ) )
	    g_jam_cwd[0] = 0;
	if ( !cp_read_var( "JamChangedPathsRoot",
			   g_changed_root, sizeof( g_changed_root ) ) ) {
	    /* default: jam's CWD */
	    int n = (int)strlen( g_jam_cwd );
	    if ( n >= (int)sizeof( g_changed_root ) )
		n = (int)sizeof( g_changed_root ) - 1;
	    memcpy( g_changed_root, g_jam_cwd, (size_t)n );
	    g_changed_root[n] = 0;
	} else {
	    /* relative root resolved against JAM_CWD */
	    if ( g_changed_root[0] != '/' &&
		 !( ( ( g_changed_root[0] >= 'a' && g_changed_root[0] <= 'z' ) ||
		      ( g_changed_root[0] >= 'A' && g_changed_root[0] <= 'Z' ) )
		    && g_changed_root[1] == ':' ) ) {
		char tmp[1024];
		cp_make_abs( tmp, sizeof( tmp ),
		    g_changed_root, g_jam_cwd );
		cp_canon( tmp );
		{
		    int n = (int)strlen( tmp );
		    if ( n >= (int)sizeof( g_changed_root ) )
			n = (int)sizeof( g_changed_root ) - 1;
		    memcpy( g_changed_root, tmp, (size_t)n );
		    g_changed_root[n] = 0;
		}
	    }
	}

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

	    /* If no explicit trailing slash, stat to classify (relative
	     * to caller's cwd, before we resolve to absolute -- matches
	     * what fopen would do in scripts that produce the list). */
	    if ( !is_dir )
		is_dir = cp_is_directory( p );

	    /* Resolve to absolute and canonicalize. */
	    cp_make_abs( abs, sizeof( abs ), p, g_changed_root );
	    cp_canon( abs );

	    if ( !abs[0] )
		continue;

	    if ( is_dir )
		cp_dv_push( &g_dirs, abs );
	    else
		cp_fs_insert( &g_files, abs );
	}
	fclose( f );

	g_loaded = 1;

	if ( DEBUG_DEPENDS ) {
	    printf( "JamChangedFilesPath: loaded %d files, %d dirs from '%s'\n",
		g_files.count, g_dirs.count, list_file );
	    printf( "JamChangedFilesPath: cwd='%s' root='%s'\n",
		g_jam_cwd, g_changed_root );
	}

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

	    bn = ( t->boundname && t->boundname[0] ) ? t->boundname : t->name;
	    if ( bn && bn[0] ) {
		char abs[8192];
		cp_make_abs( abs, sizeof( abs ), bn, g_jam_cwd );
		cp_canon( abs );

		if ( abs[0] && ( cp_fs_contains( &g_files, abs ) ||
				 cp_dv_contains_prefix( &g_dirs, abs ) ) ) {
		    if ( DEBUG_DEPENDS )
			printf( "JamChangedFilesPath: matched '%s' (target '%s')\n",
			    abs, t->name );
		    return 1;
		}
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
