/*
 * Copyright 1993, 2000 Christopher Seiwald.
 *
 * This file is part of Jam - see jam.c for Copyright information.
 */

/*
 * headers.c - handle #includes in source files
 *
 * Using regular expressions provided as the variable $(HDRSCAN), 
 * headers() searches a file for #include files and phonies up a
 * rule invocation:
 * 
 *	$(HDRRULE) <target> : <include files> ;
 *
 * External routines:
 *    headers() - scan a target for include files and call HDRRULE
 *
 * Internal routines:
 *    headers1() - using regexp, scan a file and build include LIST
 *
 * 04/13/94 (seiwald) - added shorthand L0 for null list pointer
 * 09/10/00 (seiwald) - replaced call to compile_rule with evaluate_rule,
 *		so that headers() doesn't have to mock up a parse structure
 *		just to invoke a rule.
 * 03/02/02 (seiwald) - rules can be invoked via variable names
 * 10/22/02 (seiwald) - list_new() now does its own newstr()/copystr()
 * 11/04/02 (seiwald) - const-ing for string literals
 * 12/09/02 (seiwald) - push regexp creation down to headers1().
 */

# include "jam.h"
# include "lists.h"
# include "parse.h"
# include "compile.h"
# include "rules.h"
# include "variable.h"
# include "regexp.h"
# include "headers.h"
# include "newstr.h"
# include "filesys.h"
# include "prof.h"
# include "fastre.h"
# include "depcache.h"

static LIST *headers1( const char *file, LIST *hdrscan, int *ok );
static LIST *headers1fs( const char *file, LIST *hdrscan, int *ok );

/*
 * headers() - scan a target for include files and call HDRRULE
 */

# define MAXINC 10

void
headers( TARGET *t )
{
	LIST	*hdrscan;
	LIST	*hdrrule;
	int hdr_full_scan;
	LOL	lol;

	if( !( hdrscan = var_get( "HDRSCAN" ) ) || 
	    !( hdrrule = var_get( "HDRRULE" ) ) )
	        return;
	hdr_full_scan = var_get( "HDRFS" ) != NULL;

	/* Doctor up call to HDRRULE rule */
	/* Call headers1() to get LIST of included files. */

	if( DEBUG_HEADER )
	    printf( "header scan %s (fs=%d)\n", t->name, hdr_full_scan );

	lol_init( &lol );

	lol_add( &lol, list_new( L0, t->name, 1 ) );
	PROF_ENTER( PROF_HDRSCAN1 );
	{
	    /* consult the persistent scan cache first (no-op unless   */
	    /* JamDepCachePath is set); on a miss scan and record      */

	    LIST *deps;

	    if( !depcache_get( t, hdrscan, hdr_full_scan, &deps ) )
	    {
		int scan_ok;

		deps = !hdr_full_scan
		    ? headers1( t->boundname, hdrscan, &scan_ok )
		    : headers1fs( t->boundname, hdrscan, &scan_ok );

		/* a failed or short scan behaves as before (whatever was
		 * read feeds HDRRULE), but must not become authoritative:
		 * only a complete scan may enter the persistent cache */

		if( scan_ok )
		    depcache_put( t, hdrscan, hdr_full_scan, deps );
	    }

	    lol_add( &lol, deps );
	}
	PROF_LEAVE( PROF_HDRSCAN1 );

	if( lol_get( &lol, 1 ) )
	{
	    PROF_ENTER( PROF_HDRRULE );
	    list_free( evaluate_rule( 0, hdrrule->string, &lol, L0 ) );
	    PROF_LEAVE( PROF_HDRRULE );
	}

	/* Clean up */

	lol_free( &lol );
}

/*
 * headers1() - using regexp, scan a file and build include LIST
 */

static LIST *
headers1(
	const char *file,
	LIST *hdrscan,
	int *ok )
{
	FILE	*f;
	int	i;
	int	rec = 0;
	LIST	*result = 0;
	const char *pat[ MAXINC ];
	char	buf[ 1024 ];
	char	*data, *p, *dend;
	long	flen, got;
	FILE_DECLARE_STOR_BUF(abs_name_stor);

	*ok = 0;

	if( !( f = fopen( FILE_SIMPLIFY_REL_PATH(file, abs_name_stor), "rb" ) ) )
	    return result;

	while( rec < MAXINC && hdrscan )
	{
	    pat[rec++] = hdrscan->string;
	    hdrscan = list_next( hdrscan );
	}

	/* Read the whole file once instead of a million fgets calls.
	 * Line delivery below replicates fgets exactly: lines split at
	 * 1023 chars and, on NT only (where fopen "r" meant text mode),
	 * "\r\n" -> "\n", lone '\r' kept, input stops at ^Z. */

	fseek( f, 0, SEEK_END );
	flen = ftell( f );
	fseek( f, 0, SEEK_SET );

	if( flen < 0 )
	    { fclose( f ); return result; }

	data = (char *)malloc( flen + 1 );
	if( !data )
	    { fclose( f ); return result; }

	got = (long)fread( data, 1, flen, f );

	/* complete only if everything there was read (a concurrent
	 * truncation still counts: EOF was reached) -- the caller
	 * refuses to cache incomplete scans */

	*ok = !ferror( f ) && ( got == flen || feof( f ) );

	fclose( f );
	flen = got;
	data[ flen ] = 0;

# ifdef OS_NT
	{
	    char *z = (char *)memchr( data, 0x1a, flen );
	    if( z )
		flen = (long)( z - data );
	}
# endif
	dend = data + flen;

	p = data;
	while( p < dend )
	{
	    /* one fgets-equivalent chunk: up to '\n' or 1023 chars */

	    char *nl = (char *)memchr( p, '\n', dend - p );
	    char *lend = nl ? nl : dend;	/* points at '\n' or end */
	    int clen = (int)( lend - p );	/* translated content len */
	    int len;

	    /* '\r' of a "\r\n" pair is dropped by text mode; lone '\r'
	     * is kept.  Strip before the length check so a pair right
	     * at the 1023 boundary behaves like fgets. */

# ifdef OS_NT
	    if( nl && clen && lend[-1] == '\r' )
		clen--;
# endif

	    if( clen > 1022 )
	    {
		/* long line: fgets would return a full 1023-char chunk
		 * with no newline; the "\r\n" pair is beyond it */
		len = 1023;
		memcpy( buf, p, len );
		buf[ len ] = 0;
		p += len;
	    }
	    else
	    {
		memcpy( buf, p, clen );
		len = clen;
		if( nl )
		    buf[ len++ ] = '\n';
		buf[ len ] = 0;
		p = nl ? nl + 1 : dend;
	    }

	    for( i = 0; i < rec; i++ )
	    {
		int matched;
		regexp *re = re_match( pat[i], buf, &matched );

		if( re && matched && re->startp[1] )
		{
		    /* Copy and terminate extracted string. */

		    char buf2[ MAXSYM ];
		    int l = re->endp[1] - re->startp[1];
		    memcpy( buf2, re->startp[1], l );
		    buf2[ l ] = 0;
		    result = list_new( result, buf2, 0 );

		    if( DEBUG_HEADER )
			printf( "header found: %s\n", buf2 );
		}
	    }
	}

	free( data );

	return result;
}

/*
 * headers1fs() - using regexp, fully scan a file (not only 1 header/line) and build include LIST
 */

static LIST *
headers1fs(
	const char *file,
	LIST *hdrscan,
	int *ok )
{
	FILE	*f;
	int	i;
	int	rec = 0;
	LIST	*result = 0;
	const char *pat[ MAXINC ];
	char	*buf, *pbuf;
	int len, want, found;
	FILE_DECLARE_STOR_BUF(abs_name_stor);

	*ok = 0;

	if( !( f = fopen( FILE_SIMPLIFY_REL_PATH(file, abs_name_stor), "r" ) ) )
	    return result;

	while( rec < MAXINC && hdrscan )
	{
	    pat[rec++] = hdrscan->string;
	    hdrscan = list_next( hdrscan );
	}

	fseek ( f, 0, SEEK_END );
	want = ftell ( f );
	fseek ( f, 0, SEEK_SET );
	if( want < 0 )
	    { fclose( f ); return result; }
	buf = malloc ( want + 16 );
	if( !buf )
	    { fclose( f ); return result; }
	len = (int)fread ( buf, 1, want, f );

	/* text mode shrinks CRLF, so "read it all" means EOF reached */

	*ok = !ferror( f ) && ( len == want || feof( f ) );

	buf[len] = '\0';
	fclose ( f );

	pbuf = buf;
	found = 1;
	if( DEBUG_HEADER )
	  printf ( "full contemts: %s\n", buf );
	while ( pbuf < buf+len && found )
	{
	  found = 0;
    for( i = 0; i < rec; i++ )
    {
  		int matched;
  		regexp *re = re_match( pat[i], pbuf, &matched );

  		if( re && matched && re->startp[1] )
	    {
    		/* Copy and terminate extracted string. */

    		char buf2[ MAXSYM ];
    		int l = re->endp[1] - re->startp[1];
    		memcpy( buf2, re->startp[1], l );
    		buf2[ l ] = 0;
    		result = list_new( result, buf2, 0 );

    		pbuf = (char*)re->endp[1];
    		if( DEBUG_HEADER )
    		    printf( "header found: %s (%d char left)\n", buf2, (int)(buf+len-pbuf));
    		found = 1;
    		break;
	    }
    }
	}
	free ( buf );

	return result;
}
