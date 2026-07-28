/*
 * depcache.c - persistent header-scan (HDRSCAN) result cache
 *
 * See depcache.h for the contract.
 *
 * File format "JDC3" (binary, little-endian, host-local cache):
 *
 *   char[4]  "JDC3"
 *   u32      checksum          djb2 over every payload byte below
 *   u32      generation        save counter (retention clock)
 *   u32      nstrings
 *   nstrings x { u32 len, bytes }     string table (dep paths + names)
 *   u32      nentries
 *   nentries x { i64 msig             full-resolution mtime signature
 *                i64 size             file size
 *                u32 scanhash         hash of HDRSCAN patterns + HDRFS
 *                u32 lastgen          generation last seen/used
 *                u32 name index
 *                u32 ndeps
 *                ndeps x u32 index }
 *
 * Validity of an entry is (msig, size, scanhash).  msig is the raw
 * FILETIME (100 ns units) on NT and seconds(+nanoseconds where the
 * libc exposes them) elsewhere -- whole-second keys cannot tell two
 * rewrites within the same second apart.  The signature is taken by
 * depcache_get() BEFORE the caller reads the file, so a rewrite that
 * lands between stat and read makes the NEXT run miss (never this
 * run cache new content under an old key).
 *
 * The checksum rejects byte corruption that a purely structural
 * check would accept -- a mutated dependency path must never reach
 * HDRRULE.  Entries not seen for DC_KEEP_GENS saves are dropped at
 * save time, bounding growth from deleted or renamed targets while
 * tolerating alternating partial-target runs (a hit refreshes the
 * entry's generation whenever a save happens).
 *
 * Dependency paths repeat across nearly every .d file (system and
 * engine headers), so the string table keeps the file roughly an
 * order of magnitude smaller than inline strings and makes reload
 * cheap: each unique string is interned via newstr() exactly once
 * and list entries then reuse it via copystr().
 *
 * The in-memory table is the union of the loaded file and this run's
 * fresh scans (fresh wins).  It is rewritten only when at least one
 * fresh scan happened.  The payload is assembled in memory, written
 * to a per-process temp file and published with an atomic replace
 * (MoveFileEx / rename), so concurrent jam processes never tear each
 * other's cache and a crash never loses the last good file.
 */

# include "jam.h"
# include "hash.h"
# include "lists.h"
# include "parse.h"
# include "rules.h"
# include "variable.h"
# include "newstr.h"
# include "depcache.h"

# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <sys/stat.h>
# include <time.h>

# ifdef OS_NT
# include <windows.h>
# include <process.h>
# define dc_getpid _getpid
# else
# include <unistd.h>
# define dc_getpid getpid
# endif

# define DC_MAXSTR	4096
# define DC_MAXCOUNT	( 1u << 26 )
# define DC_KEEP_GENS	32

typedef struct dcsig {
	long long	msig;		/* full-resolution mtime */
	long long	size;
} DCSIG;

typedef struct dcentry {
	const char	*name;		/* key: bound name (interned) */
	DCSIG		sig;
	unsigned	scanhash;
	unsigned	lastgen;
	LIST		*deps;		/* owned by the cache */
	char		fresh;		/* scanned this run */
	char		seen;		/* hit this run */
} DCENTRY;

static struct hash *dc_hash = 0;
static int  dc_state = 0;	/* 0 unknown, 1 enabled, -1 disabled */
static char dc_path[ 1024 ];
static int  dc_dirty = 0;
static unsigned dc_gen = 0;	/* generation loaded from the file */
static int  dc_hits = 0, dc_misses = 0;
static long long dc_run_start;	/* racy-entry horizon (msig units) */

/* signature taken by depcache_get() before the caller reads the file;
 * depcache_put() records exactly this one (see the header comment) */

static const char *dc_pending_name;
static DCSIG dc_pending_sig;
static int  dc_pending_ok;

/* ------------------------------------------------------------------ */

static unsigned
dc_scanhash( LIST *hdrscan, int hdrfs )
{
	unsigned h = 5381u ^ ( hdrfs ? 0x9e3779b9u : 0 );
	for( ; hdrscan; hdrscan = list_next( hdrscan ) )
	{
	    const char *s = hdrscan->string;
	    while( *s )
		h = ( h * 33u ) ^ (unsigned char)*s++;
	    h = ( h * 33u ) ^ 0xffu;	/* string separator */
	}
	return h;
}

static long long
dc_now_msig( void )
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

static int
dc_filesig( const char *path, DCSIG *sig )
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

static unsigned
dc_checksum( const unsigned char *p, size_t n )
{
	unsigned h = 5381u;
	while( n-- )
	    h = ( h * 33u ) ^ *p++;
	return h;
}

/* --- growable output buffer (payload is assembled in memory) ------- */

typedef struct dcbuf {
	unsigned char	*p;
	size_t		len;
	size_t		cap;
} DCBUF;

static void
dc_buf_bytes( DCBUF *b, const void *src, size_t n )
{
	if( b->len + n > b->cap )
	{
	    size_t nc = b->cap ? b->cap * 2 : ( 1 << 16 );
	    while( nc < b->len + n )
		nc *= 2;
	    b->p = (unsigned char *)realloc( b->p, nc );
	    b->cap = nc;
	}
	memcpy( b->p + b->len, src, n );
	b->len += n;
}

static void
dc_buf_u32( DCBUF *b, unsigned v )
{
	unsigned char x[4];
	x[0] = (unsigned char)v; x[1] = (unsigned char)( v >> 8 );
	x[2] = (unsigned char)( v >> 16 ); x[3] = (unsigned char)( v >> 24 );
	dc_buf_bytes( b, x, 4 );
}

static void
dc_buf_i64( DCBUF *b, long long v )
{
	dc_buf_u32( b, (unsigned)( (unsigned long long)v & 0xffffffffu ) );
	dc_buf_u32( b, (unsigned)( (unsigned long long)v >> 32 ) );
}

/* --- load: slurp, verify checksum, then parse ---------------------- */

typedef struct dcrd {
	const unsigned char *p;
	size_t		len;
	size_t		pos;
	int		err;
} DCRD;

static unsigned
dc_rd_u32( DCRD *r )
{
	unsigned v;
	if( r->pos + 4 > r->len )
	    { r->err = 1; return 0; }
	v = (unsigned)r->p[r->pos] | ((unsigned)r->p[r->pos+1]<<8)
	  | ((unsigned)r->p[r->pos+2]<<16) | ((unsigned)r->p[r->pos+3]<<24);
	r->pos += 4;
	return v;
}

static long long
dc_rd_i64( DCRD *r )
{
	unsigned lo = dc_rd_u32( r );
	unsigned hi = dc_rd_u32( r );
	return (long long)( ( (unsigned long long)hi << 32 ) | lo );
}

static void
dc_load( void )
{
	FILE *f;
	long fsize;
	unsigned char *data;
	DCRD rd;
	unsigned nstr, nent, i;
	const char **strtab = 0;

	f = fopen( dc_path, "rb" );
	if( !f )
	    return;

	fseek( f, 0, SEEK_END );
	fsize = ftell( f );
	fseek( f, 0, SEEK_SET );

	if( fsize < 12 )
	    { fclose( f ); return; }

	/* +1: the string-table NUL-swap below may touch data[fsize]
	 * when a string ends exactly at EOF */

	data = (unsigned char *)malloc( fsize + 1 );
	if( !data || (long)fread( data, 1, fsize, f ) != fsize )
	    { free( data ); fclose( f ); return; }
	fclose( f );

	if( memcmp( data, "JDC3", 4 ) )
	    { free( data ); return; }

	/* integrity: a same-length mutation inside a path would pass
	 * every structural check and reach HDRRULE -- reject it here */

	{
	    unsigned stored = (unsigned)data[4] | ((unsigned)data[5]<<8)
		| ((unsigned)data[6]<<16) | ((unsigned)data[7]<<24);
	    if( stored != dc_checksum( data + 8, fsize - 8 ) )
		{ free( data ); return; }
	}

	rd.p = data; rd.len = (size_t)fsize; rd.pos = 8; rd.err = 0;

	dc_gen = dc_rd_u32( &rd );

	nstr = dc_rd_u32( &rd );

	/* every string costs at least its 4-byte length field: bound the
	 * table allocation by what the payload could actually hold */

	if( rd.err || nstr > DC_MAXCOUNT || nstr > ( rd.len - rd.pos ) / 4 )
	    goto out;

	strtab = (const char **)malloc( nstr ? nstr * sizeof( char * ) : 1 );
	if( !strtab )
	    goto out;

	for( i = 0; i < nstr; i++ )
	{
	    unsigned len = dc_rd_u32( &rd );
	    unsigned char save;

	    if( rd.err || len >= DC_MAXSTR || rd.pos + len > rd.len )
		goto out;

	    save = data[ rd.pos + len ];
	    data[ rd.pos + len ] = 0;
	    strtab[ i ] = newstr( (const char *)data + rd.pos );
	    data[ rd.pos + len ] = save;
	    rd.pos += len;
	}

	nent = dc_rd_u32( &rd );

	for( i = 0; !rd.err && i < nent; i++ )
	{
	    DCENTRY entry, *e = &entry;
	    LIST *deps = L0;
	    long long msig, size;
	    unsigned scanhash, lastgen, nameidx, ndeps, j;

	    msig = dc_rd_i64( &rd );
	    size = dc_rd_i64( &rd );
	    scanhash = dc_rd_u32( &rd );
	    lastgen = dc_rd_u32( &rd );
	    nameidx = dc_rd_u32( &rd );
	    ndeps = dc_rd_u32( &rd );

	    if( rd.err || nameidx >= nstr || ndeps > DC_MAXCOUNT )
		break;

	    for( j = 0; j < ndeps; j++ )
	    {
		unsigned idx = dc_rd_u32( &rd );
		if( rd.err || idx >= nstr )
		    break;
		deps = list_new( deps, strtab[ idx ], 1 );  /* copystr */
	    }
	    if( j < ndeps )
		{ list_free( deps ); break; }

	    e->name = strtab[ nameidx ];

	    if( hashenter( dc_hash, (HASHDATA **)&e ) )
	    {
		e->name = strtab[ nameidx ];	/* already interned */
		e->sig.msig = msig;
		e->sig.size = size;
		e->scanhash = scanhash;
		e->lastgen = lastgen;
		e->deps = deps;
		e->fresh = 0;
		e->seen = 0;
	    }
	    else
		list_free( deps );	/* duplicate name: keep first */
	}

    out:
	free( (void *)strtab );
	free( data );
}

static int
dc_init( void )
{
	LIST *var;

	if( dc_state )
	    return dc_state > 0;

	dc_state = -1;

	var = var_get( "JamDepCachePath" );
	if( !var || !var->string || !var->string[0] )
	    return 0;

	if( strlen( var->string ) >= sizeof( dc_path ) )
	    return 0;
	strcpy( dc_path, var->string );

	dc_run_start = dc_now_msig();
	dc_hash = hashinit( sizeof( DCENTRY ), "depcache" );
	dc_load();

	dc_state = 1;
	return 1;
}

/* ------------------------------------------------------------------ */

int
depcache_get( TARGET *t, LIST *hdrscan, int hdrfs, LIST **deps )
{
	DCENTRY entry, *e = &entry;

	*deps = L0;

	if( !dc_init() )
	    return 0;

	/* signature BEFORE the caller reads the file (see file header) */

	dc_pending_name = t->boundname;
	dc_pending_ok = dc_filesig( t->boundname, &dc_pending_sig );

	e->name = t->boundname;

	if( !dc_pending_ok || !hashcheck( dc_hash, (HASHDATA **)&e ) )
	    { dc_misses++; return 0; }

	if( e->sig.msig != dc_pending_sig.msig
	    || e->sig.size != dc_pending_sig.size
	    || e->scanhash != dc_scanhash( hdrscan, hdrfs ) )
	    { dc_misses++; return 0; }

	dc_hits++;
	e->seen = 1;

	if( DEBUG_HEADER )
	    printf( "depcache hit %s\n", t->boundname );

	*deps = list_copy( L0, e->deps );
	return 1;
}

void
depcache_put( TARGET *t, LIST *hdrscan, int hdrfs, LIST *deps )
{
	DCENTRY entry, *e = &entry;

	if( !dc_init() )
	    return;

	/* no pre-read signature (stat failed / different target than
	 * the preceding get): the content cannot be tied to a validity
	 * key, so do not record it */

	if( !dc_pending_ok || dc_pending_name != t->boundname )
	    return;

	e->name = t->boundname;

	if( hashenter( dc_hash, (HASHDATA **)&e ) )
	    e->name = newstr( t->boundname );
	else
	    list_free( e->deps );

	e->sig = dc_pending_sig;
	e->scanhash = dc_scanhash( hdrscan, hdrfs );
	e->lastgen = 0;		/* assigned at save */
	e->deps = list_copy( L0, deps );
	e->fresh = 1;
	e->seen = 1;

	dc_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* save: two passes over the kept entries -- first to build the       */
/* string table, then to emit entries as indices                      */

typedef struct dcstr {
	const char	*s;	/* key */
	unsigned	idx;
} DCSTR;

static struct hash *dc_strhash;
static unsigned dc_strcount;
static unsigned dc_outcount;
static unsigned dc_newgen;
static DCBUF dc_strbuf;
static DCBUF dc_entbuf;

static unsigned
dc_str_index( const char *s )
{
	DCSTR d, *dp = &d;

	dp->s = s;

	if( hashenter( dc_strhash, (HASHDATA **)&dp ) )
	{
	    unsigned len = (unsigned)strlen( s );
	    dp->s = s;		/* interned already (newstr/copystr) */
	    dp->idx = dc_strcount++;
	    dc_buf_u32( &dc_strbuf, len );
	    dc_buf_bytes( &dc_strbuf, s, len );
	}

	return dp->idx;
}

static int
dc_keep( DCENTRY *e )
{
	LIST *l;

	/* an over-long path would be rejected wholesale at load time and
	 * permanently kill the cache: drop just this entry instead */

	if( strlen( e->name ) >= DC_MAXSTR )
	    return 0;
	for( l = e->deps; l; l = list_next( l ) )
	    if( strlen( l->string ) >= DC_MAXSTR )
		return 0;

	/* the racy-entry rule (as git's index): a file whose mtime is
	 * not strictly older than this run could be rewritten again
	 * within the same timestamp granularity without changing its
	 * signature -- never persist such an entry, rescan next run */

	if( e->fresh && e->sig.msig >= dc_run_start )
	    return 0;

	if( e->fresh || e->seen )
	    return 1;
	if( e->lastgen > dc_newgen )
	    return 0;		/* stale future gen (older file re-published) */
	return dc_newgen - e->lastgen <= DC_KEEP_GENS;
}

static void
dc_save_strings( void *closure, HASHDATA *data )
{
	DCENTRY *e = (DCENTRY *)data;
	LIST *l;

	if( !dc_keep( e ) )
	    return;

	dc_str_index( e->name );
	for( l = e->deps; l; l = list_next( l ) )
	    dc_str_index( l->string );
}

static void
dc_save_entry( void *closure, HASHDATA *data )
{
	DCENTRY *e = (DCENTRY *)data;
	LIST *l;
	unsigned n = 0;

	if( !dc_keep( e ) )
	    return;

	dc_buf_i64( &dc_entbuf, e->sig.msig );
	dc_buf_i64( &dc_entbuf, e->sig.size );
	dc_buf_u32( &dc_entbuf, e->scanhash );
	dc_buf_u32( &dc_entbuf, ( e->fresh || e->seen ) ? dc_newgen : e->lastgen );
	dc_buf_u32( &dc_entbuf, dc_str_index( e->name ) );

	for( l = e->deps; l; l = list_next( l ) )
	    n++;
	dc_buf_u32( &dc_entbuf, n );

	for( l = e->deps; l; l = list_next( l ) )
	    dc_buf_u32( &dc_entbuf, dc_str_index( l->string ) );

	dc_outcount++;
}

static int
dc_replace( const char *tmp, const char *dst )
{
# ifdef OS_NT
	/* rename() cannot overwrite on NT; MoveFileEx replaces without
	 * a delete window, so readers always see old or new, never none */
	return MoveFileExA( tmp, dst, MOVEFILE_REPLACE_EXISTING ) ? 0 : -1;
# else
	return rename( tmp, dst );
# endif
}

void
depcache_done( void )
{
	char tmp[ 1100 ];
	DCBUF payload;
	unsigned char hdr[ 8 ];
	unsigned sum;
	FILE *f;
	int ok;

	if( dc_state <= 0 || !dc_dirty )
	    return;

	dc_newgen = dc_gen + 1;

	dc_strhash = hashinit( sizeof( DCSTR ), "depcache strings" );
	dc_strcount = 0;
	dc_outcount = 0;
	memset( &dc_strbuf, 0, sizeof( dc_strbuf ) );
	memset( &dc_entbuf, 0, sizeof( dc_entbuf ) );

	hashiterate( dc_hash, dc_save_strings, 0 );
	hashiterate( dc_hash, dc_save_entry, 0 );

	/* assemble payload: gen, nstrings, strings, nentries, entries */

	memset( &payload, 0, sizeof( payload ) );
	dc_buf_u32( &payload, dc_newgen );
	dc_buf_u32( &payload, dc_strcount );
	dc_buf_bytes( &payload, dc_strbuf.p, dc_strbuf.len );
	dc_buf_u32( &payload, dc_outcount );
	dc_buf_bytes( &payload, dc_entbuf.p, dc_entbuf.len );

	sum = dc_checksum( payload.p, payload.len );

	memcpy( hdr, "JDC3", 4 );
	hdr[4] = (unsigned char)sum; hdr[5] = (unsigned char)( sum >> 8 );
	hdr[6] = (unsigned char)( sum >> 16 ); hdr[7] = (unsigned char)( sum >> 24 );

	/* per-process temp name: concurrent jam processes must never
	 * write the same file (a shared .tmp can interleave) */

	sprintf( tmp, "%s.%u.tmp", dc_path, (unsigned)dc_getpid() );

	f = fopen( tmp, "wb" );
	if( f )
	{
	    setvbuf( f, 0, _IOFBF, 1 << 20 );
	    fwrite( hdr, 1, 8, f );
	    fwrite( payload.p, 1, payload.len, f );

	    ok = !ferror( f );
	    if( fclose( f ) || !ok || dc_replace( tmp, dc_path ) )
		remove( tmp );
	}

	free( payload.p );
	free( dc_strbuf.p );
	free( dc_entbuf.p );
	hashdone( dc_strhash );
	dc_strhash = 0;

	if( DEBUG_HEADER )
	    printf( "depcache saved %u entries, %u strings (%d hits, %d misses)\n",
		dc_outcount, dc_strcount, dc_hits, dc_misses );
}
