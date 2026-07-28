/*
 * fastre.c - cached + fast-path regexp matching for jam
 *
 * Motivation: dagor's jam rules call MATCH hundreds of thousands of
 * times per run with a handful of pattern shapes, and HDRSCAN runs a
 * regexp over ~1M dependency-file lines.  Compiling the pattern on
 * every call and running the generic backtracking engine dominates
 * jam's startup time.
 *
 * This module keeps a permanent cache of compiled patterns and, for
 * four structural pattern shapes (recognized from the pattern text),
 * executes a specialized linear-time matcher instead of the Spencer
 * engine.  Anything that doesn't strictly fit a shape falls back to
 * the Spencer engine, so behavior is unchanged.
 *
 * Fast-path semantics replicate the Spencer engine in regexp.c
 * EXACTLY, including its quirks:
 *   - inside [...] a backslash is a literal character (no escapes)
 *   - '.' matches any character except NUL (including newline)
 *   - '(atom)+' captures only the LAST repetition
 *   - alternation tries branches in source order
 *   - unanchored search returns the leftmost match
 *
 * Shapes (as used by dagor's jBuild rules):
 *   T1  ([^X])+Y+(.*)          SplitStringsOnSpace
 *   T2  ([^X]*)([X])(.*)       AddEscapesToMakeJamValidForWrite etc.
 *   T3  ^(a|b|...|)LIT(LIT2.*) ReplaceRootPrefixForKnownOption...,
 *       ^LIT(.*)               SimplifyComposedPath, ReplaceRootPrefix
 *   T4  ^[^X]*: *([^Y]*).*$    .d dependency line scan (HDRSCAN)
 *
 * Validation: set JAM_FASTRE_CHECK=1 to run BOTH engines on every
 * call and abort on any disagreement.  JAM_FASTRE=0 disables fast
 * paths (cache still active).
 */

# include "jam.h"
# include "hash.h"
# include "newstr.h"
# include "fastre.h"
# include "prof.h"

# include <string.h>
# include <stdlib.h>

/* ------------------------------------------------------------------ */
/* pattern tokenizer (mirrors regcomp's lexical rules)                 */

typedef struct ftok {
	int	type;
# define FT_CHAR	1	/* literal char (possibly \-escaped)     */
# define FT_ANY		2	/* .                                     */
# define FT_CLASS	3	/* [...] or [^...]                       */
# define FT_LPAREN	4
# define FT_RPAREN	5
# define FT_STAR	6
# define FT_PLUS	7
# define FT_PIPE	8
# define FT_BOL		9	/* ^ as first token                      */
# define FT_EOL		10	/* $ as last token                       */
	char	c;		/* FT_CHAR                               */
	unsigned char cls[32];	/* FT_CLASS: 256-bit membership bitmap   */
} FTOK;

# define CLS_SET( m, c )   ( (m)[ ((unsigned char)(c)) >> 3 ] |=  ( 1u << ( (c) & 7 ) ) )
# define CLS_TEST( m, c )  ( (m)[ ((unsigned char)(c)) >> 3 ] &   ( 1u << ( (c) & 7 ) ) )

# define FASTRE_MAXTOK 256

/*
 * tokenize() - lex a pattern into FTOKs; returns count or -1 if the
 * pattern uses anything beyond the subset we model (?, \<, \>, nested
 * or quantified groups are handled at recognition, not here).
 */

static int
tokenize( const char *p, FTOK *t, int maxtok )
{
	int n = 0;
	const char *start = p;

	memset( t, 0, maxtok * sizeof( *t ) );	/* no indeterminate bytes
						 * reach the FASTPAT copies */

	while( *p )
	{
	    if( n >= maxtok )
		return -1;

	    switch( *p )
	    {
	    case '^':
		/* Spencer treats '^' as a BOL assertion at ANY position;   */
		/* we only model it as the first atom, else fall back       */
		if( p != start )
		    return -1;
		t[n].type = FT_BOL;
		t[n].c = '^';
		n++; p++;
		break;

	    case '$':
		/* likewise '$' is an EOL assertion anywhere; only model    */
		/* it in final position                                     */
		if( p[1] )
		    return -1;
		t[n].type = FT_EOL;
		t[n].c = '$';
		n++; p++;
		break;

	    case '.':
		t[n++].type = FT_ANY; p++;
		break;

	    case '(':
		t[n++].type = FT_LPAREN; p++;
		break;

	    case ')':
		t[n++].type = FT_RPAREN; p++;
		break;

	    case '*':
		t[n++].type = FT_STAR; p++;
		break;

	    case '+':
		t[n++].type = FT_PLUS; p++;
		break;

	    case '|':
		t[n++].type = FT_PIPE; p++;
		break;

	    case '?':
		return -1;	/* not modeled */

	    case '\n':
		/* Spencer treats a raw newline exactly like '|' (an
		 * alternation separator, see reg()/regbranch()); the
		 * tokenizer does not model that -- fall back */
		return -1;

	    case '\\':
		if( !p[1] )
		    return -1;
		if( p[1] == '<' || p[1] == '>' )
		    return -1;	/* word boundaries not modeled */
		t[n].type = FT_CHAR;
		t[n].c = p[1];
		n++; p += 2;
		break;

	    case '[':
	    {
		/* replicate regcomp's class parsing exactly:       */
		/* optional '^', leading ']' or '-' literal, ranges */
		/* with '-', backslash NOT special                  */
		int negate = 0;
		unsigned char *m = t[n].cls;
		const char *q = p + 1;
		const char *prev = 0;

		memset( m, 0, 32 );

		if( *q == '^' )
		    { negate = 1; q++; }

		if( *q == ']' || *q == '-' )
		    { CLS_SET( m, *q ); prev = q; q++; }

		while( *q && *q != ']' )
		{
		    if( *q == '-' )
		    {
			q++;
			if( *q == ']' || !*q )
			{
			    CLS_SET( m, '-' );
			    prev = 0;
			}
			else
			{
			    int lo, hi;
			    if( !prev )
				return -1;	/* e.g. [-a] variants; keep safe */
			    lo = (unsigned char)*prev + 1;
			    hi = (unsigned char)*q;
			    if( lo > hi + 1 )
				return -1;	/* invalid range -> Spencer errors */
			    for( ; lo <= hi; lo++ )
				CLS_SET( m, lo );
			    prev = 0;
			    q++;
			}
		    }
		    else
		    {
			CLS_SET( m, *q );
			prev = q;
			q++;
		    }
		}

		if( *q != ']' )
		    return -1;	/* unmatched [] */

		if( negate )
		{
		    int i;
		    for( i = 0; i < 32; i++ )
			m[i] = (unsigned char)~m[i];
		    /* NUL never matches any class */
		    m[0] &= (unsigned char)~1u;
		}

		t[n++].type = FT_CLASS;
		p = q + 1;
		break;
	    }

	    default:
		t[n].type = FT_CHAR;
		t[n].c = *p;
		n++; p++;
		break;
	    }
	}

	return n;
}

/* ------------------------------------------------------------------ */
/* recognized fast shapes                                              */

typedef struct fitem {
	int	type;		/* FT_CHAR / FT_ANY / FT_CLASS */
	char	c;
	unsigned char cls[32];
} FITEM;

# define FP_NONE	0
# define FP_SPLITPLUS	1	/* ([^X])+Y+(.*)                  */
# define FP_SPLITSTAR	2	/* ([^X]*)([X])(.*)               */
# define FP_PREFIX	3	/* ^(a|b|)SEQ(SEQ2.*)  /  ^SEQ(SEQ2.*) */
# define FP_DEPLINE	4	/* ^[^X]*: *([^Y]*).*$            */

# define FASTRE_MAXALT	24
# define FASTRE_MAXSEQ	16

typedef struct fastpat {
	int	kind;

	/* T1 / T2 / T4 */
	unsigned char cls0[32];
	unsigned char cls1[32];
	char	ch;		/* T1 separator                    */

	/* T4 (dep-line): ^A* LIT B* ( C* ) .* $                      */
	int	pre_any;	/* A is ANY (vs class in cls0)     */
	char	lit[ 8 ];	/* literal run after prefix        */
	int	litlen;
	int	has_skip;	/* B present                       */
	unsigned char skipcls[32];	/* B class (or single char)  */

	/* T3 */
	int	has_alt;	/* alternation group present       */
	int	nalts;
	const char *alt[ FASTRE_MAXALT ];	/* unescaped literals */
	int	altlen[ FASTRE_MAXALT ];
	FITEM	seq[ FASTRE_MAXSEQ ];
	int	nseq;
	FITEM	cap[ FASTRE_MAXSEQ ];	/* atoms inside capture before .* */
	int	ncap;
	char	altstore[ 512 ];	/* backing store for alt[] strings */
} FASTPAT;

/* item match helper (NUL always fails) */
static int
fitem_match( const FITEM *it, unsigned char c )
{
	if( !c )
	    return 0;
	switch( it->type )
	{
	case FT_CHAR:	return c == (unsigned char)it->c;
	case FT_ANY:	return 1;
	case FT_CLASS:	return CLS_TEST( it->cls, c ) != 0;
	}
	return 0;
}

/*
 * recognize() - map a token stream onto one of the fast shapes
 */

static void
recognize( const FTOK *t, int n, FASTPAT *fp )
{
	fp->kind = FP_NONE;

	/* ---- T1: ( CLASS ) + CHAR + ( ANY * )  ---- */

	if( n == 10
	    && t[0].type == FT_LPAREN && t[1].type == FT_CLASS
	    && t[2].type == FT_RPAREN && t[3].type == FT_PLUS
	    && t[4].type == FT_CHAR   && t[5].type == FT_PLUS
	    && t[6].type == FT_LPAREN && t[7].type == FT_ANY
	    && t[8].type == FT_STAR   && t[9].type == FT_RPAREN )
	{
	    /* separator must NOT be matched by the class, else the   */
	    /* no-backtrack shortcut in the matcher would be wrong    */
	    if( !CLS_TEST( t[1].cls, t[4].c ) )
	    {
		memcpy( fp->cls0, t[1].cls, 32 );
		fp->ch = t[4].c;
		fp->kind = FP_SPLITPLUS;
	    }
	    return;
	}

	/* ---- T2: ( CLASS0 * ) ( CLASS1 ) ( ANY * ) ---- */

	if( n == 11
	    && t[0].type == FT_LPAREN && t[1].type == FT_CLASS
	    && t[2].type == FT_STAR   && t[3].type == FT_RPAREN
	    && t[4].type == FT_LPAREN && t[5].type == FT_CLASS
	    && t[6].type == FT_RPAREN
	    && t[7].type == FT_LPAREN && t[8].type == FT_ANY
	    && t[9].type == FT_STAR   && t[10].type == FT_RPAREN )
	{
	    /* require CLASS0 == complement of CLASS1 (over 1..255), so */
	    /* the first CLASS1 char is exactly where the CLASS0 run     */
	    /* ends and no backtracking can occur                        */
	    int i, iscomp = 1;
	    for( i = 1; i < 256; i++ )
	    {
		int in0 = CLS_TEST( t[1].cls, i ) != 0;
		int in1 = CLS_TEST( t[5].cls, i ) != 0;
		if( in0 == in1 )
		    { iscomp = 0; break; }
	    }
	    if( iscomp )
	    {
		memcpy( fp->cls0, t[1].cls, 32 );
		memcpy( fp->cls1, t[5].cls, 32 );
		fp->kind = FP_SPLITSTAR;
	    }
	    return;
	}

	/* ---- T4: BOL (ANY|CLASS)* LIT+ [skip*] ( CLASS * ) ANY * EOL --- */
	/* covers both dep-file scan patterns:                             */
	/*   msvc:  ^.*: [.\/]*([^\n]*).*$                                 */
	/*   clang: ^[^ ]*: *([^\n]*).*$   (the ' *' is the skip run)      */

	if( n >= 11
	    && t[0].type == FT_BOL
	    && ( t[1].type == FT_ANY || t[1].type == FT_CLASS )
	    && t[2].type == FT_STAR
	    && t[n-7].type == FT_LPAREN
	    && t[n-6].type == FT_CLASS
	    && t[n-5].type == FT_STAR
	    && t[n-4].type == FT_RPAREN
	    && t[n-3].type == FT_ANY
	    && t[n-2].type == FT_STAR
	    && t[n-1].type == FT_EOL )
	{
	    int i = 3, litlen = 0, ok = 1, has_skip = 0, skip_i = 0;

	    while( i < n - 7 )
	    {
		int star = ( i + 1 < n - 7 && t[i+1].type == FT_STAR );
		int plus = ( i + 1 < n - 7 && t[i+1].type == FT_PLUS );

		if( t[i].type == FT_CHAR && !star && !plus )
		{
		    /* literal run char (must precede any skip run) */
		    if( has_skip || litlen >= (int)sizeof( fp->lit ) )
			{ ok = 0; break; }
		    fp->lit[ litlen++ ] = t[i].c;
		    i++;
		}
		else if( ( t[i].type == FT_CHAR || t[i].type == FT_CLASS )
		    && star )
		{
		    if( has_skip )
			{ ok = 0; break; }
		    has_skip = 1;
		    skip_i = i;
		    i += 2;
		}
		else
		    { ok = 0; break; }
	    }

	    if( ok && litlen > 0 )
	    {
		fp->pre_any = ( t[1].type == FT_ANY );
		if( !fp->pre_any )
		    memcpy( fp->cls0, t[1].cls, 32 );
		fp->litlen = litlen;
		fp->has_skip = has_skip;
		if( has_skip )
		{
		    if( t[skip_i].type == FT_CLASS )
			memcpy( fp->skipcls, t[skip_i].cls, 32 );
		    else
		    {
			memset( fp->skipcls, 0, 32 );
			CLS_SET( fp->skipcls, t[skip_i].c );
		    }
		}
		memcpy( fp->cls1, t[n-6].cls, 32 );
		fp->kind = FP_DEPLINE;
		return;
	    }
	    /* not T4: fall through to T3 */
	}

	/* ---- T3: BOL [ ( alt | alt | ... ) ] SEQ ( SEQ2 ANY * ) ---- */

	if( n >= 5 && t[0].type == FT_BOL )
	{
	    int i = 1;
	    char *altbuf = fp->altstore;
	    char *ab = altbuf;

	    fp->has_alt = 0;
	    fp->nalts = 0;
	    fp->nseq = 0;
	    fp->ncap = 0;

	    /* optional alternation group of pure literals */

	    if( t[i].type == FT_LPAREN )
	    {
		/* scan ahead: is there a PIPE before RPAREN?  pure alt */
		/* group must contain only CHAR and PIPE tokens        */
		int j = i + 1, haspipe = 0, ok = 1;
		for( ; j < n && t[j].type != FT_RPAREN; j++ )
		{
		    if( t[j].type == FT_PIPE ) haspipe = 1;
		    else if( t[j].type != FT_CHAR ) { ok = 0; break; }
		}
		if( ok && haspipe && j < n )
		{
		    /* collect alternatives */
		    const char *as = ab;
		    fp->has_alt = 1;
		    for( j = i + 1; ; j++ )
		    {
			if( t[j].type == FT_CHAR )
			{
			    if( ab - altbuf >= (int)sizeof( fp->altstore ) - 2 )
				return;	/* too big; leave FP_NONE */
			    *ab++ = t[j].c;
			}
			else	/* PIPE or RPAREN: close current alt */
			{
			    if( fp->nalts >= FASTRE_MAXALT )
				return;
			    /* the terminator needs space too: a run of
			     * empty alternatives writes NULs without ever
			     * passing the character guard above */
			    if( ab - altbuf >= (int)sizeof( fp->altstore ) )
				return;
			    *ab++ = 0;
			    fp->alt[ fp->nalts ] = as;
			    fp->altlen[ fp->nalts ] = (int)strlen( as );
			    fp->nalts++;
			    as = ab;
			    if( t[j].type == FT_RPAREN )
				break;
			}
		    }
		    i = j + 1;
		}
		else if( t[i+1].type == FT_RPAREN
		      || haspipe )
		{
		    return;	/* weird group; not our shape */
		}
		/* else: group with no pipe = the capture group; falls */
		/* through with i still at LPAREN                      */
	    }

	    /* SEQ: CHAR/ANY/CLASS atoms, no quantifiers, up to LPAREN */

	    while( i < n && t[i].type != FT_LPAREN )
	    {
		if( i + 1 < n
		    && ( t[i+1].type == FT_STAR || t[i+1].type == FT_PLUS ) )
		    return;	/* quantified atom in SEQ: not our shape */

		if( t[i].type == FT_CHAR || t[i].type == FT_ANY
		    || t[i].type == FT_CLASS )
		{
		    if( fp->nseq >= FASTRE_MAXSEQ )
			return;
		    fp->seq[ fp->nseq ].type = t[i].type;
		    fp->seq[ fp->nseq ].c = t[i].c;
		    memcpy( fp->seq[ fp->nseq ].cls, t[i].cls, 32 );
		    fp->nseq++;
		    i++;
		}
		else
		    return;	/* EOL or stray token: not our shape */
	    }

	    if( i >= n || t[i].type != FT_LPAREN )
		return;
	    i++;

	    /* capture: atoms then ANY STAR RPAREN <end> */

	    while( i < n && !( t[i].type == FT_ANY
			       && i + 1 < n && t[i+1].type == FT_STAR ) )
	    {
		if( t[i].type == FT_CHAR || t[i].type == FT_CLASS )
		{
		    if( i + 1 < n
			&& ( t[i+1].type == FT_STAR || t[i+1].type == FT_PLUS ) )
			return;
		    if( fp->ncap >= FASTRE_MAXSEQ )
			return;
		    fp->cap[ fp->ncap ].type = t[i].type;
		    fp->cap[ fp->ncap ].c = t[i].c;
		    memcpy( fp->cap[ fp->ncap ].cls, t[i].cls, 32 );
		    fp->ncap++;
		    i++;
		}
		else
		    return;
	    }

	    if( i + 2 >= n + 1 )	/* need ANY STAR RPAREN */
		return;
	    if( !( t[i].type == FT_ANY && t[i+1].type == FT_STAR
		   && i + 2 < n && t[i+2].type == FT_RPAREN ) )
		return;
	    if( i + 3 != n )
		return;		/* nothing may follow the capture group */

	    fp->kind = FP_PREFIX;
	    return;
	}
}

/* ------------------------------------------------------------------ */
/* fast matchers: fill startp/endp exactly like regexec would          */

static void
caps_clear( const char **sp, const char **ep )
{
	int i;
	for( i = 0; i < NSUBEXP; i++ )
	    sp[i] = ep[i] = 0;
}

/* T1: ([^X])+Y+(.*) unanchored */
static int
run_splitplus( const FASTPAT *fp, const char *s,
	const char **sp, const char **ep )
{
	const unsigned char *p = (const unsigned char *)s;

	caps_clear( sp, ep );

	while( *p )
	{
	    if( CLS_TEST( fp->cls0, *p ) )
	    {
		/* maximal class run */
		const unsigned char *q = p + 1;
		while( *q && CLS_TEST( fp->cls0, *q ) )
		    q++;

		/* separator must follow immediately (no backtrack     */
		/* possible: separator is not a class char, so no      */
		/* shorter repetition can expose one)                  */
		if( *q == (unsigned char)fp->ch )
		{
		    const unsigned char *r = q + 1;
		    while( *r == (unsigned char)fp->ch )
			r++;

		    sp[0] = (const char *)p;
		    sp[1] = (const char *)( q - 1 );	/* last rep only */
		    ep[1] = (const char *)q;
		    sp[2] = (const char *)r;
		    ep[2] = (const char *)r + strlen( (const char *)r );
		    ep[0] = ep[2];
		    return 1;
		}

		/* every start inside [p,q) fails the same way */
		p = q;
	    }
	    else
		p++;
	}

	return 0;
}

/* T2: ([^X]*)([X])(.*) with cls0 == ~cls1, unanchored */
static int
run_splitstar( const FASTPAT *fp, const char *s,
	const char **sp, const char **ep )
{
	const unsigned char *p = (const unsigned char *)s;

	caps_clear( sp, ep );

	while( *p && !CLS_TEST( fp->cls1, *p ) )
	    p++;

	if( !*p )
	    return 0;	/* no separator char anywhere: no match */

	/* leftmost match starts at 0: group1 runs [s,p) */

	sp[0] = s;
	sp[1] = s;
	ep[1] = (const char *)p;
	sp[2] = (const char *)p;
	ep[2] = (const char *)p + 1;
	sp[3] = ep[2];
	ep[3] = ep[2] + strlen( ep[2] );
	ep[0] = ep[3];
	return 1;
}

/* T3: ^(a|b|)SEQ(SEQ2.*) anchored */
static int
run_prefix( const FASTPAT *fp, const char *s,
	const char **sp, const char **ep )
{
	int a;
	int nalts = fp->has_alt ? fp->nalts : 1;

	caps_clear( sp, ep );

	for( a = 0; a < nalts; a++ )
	{
	    const unsigned char *p = (const unsigned char *)s;
	    int k;

	    if( fp->has_alt )
	    {
		if( strncmp( (const char *)p, fp->alt[a], fp->altlen[a] ) )
		    continue;
		p += fp->altlen[a];
	    }

	    /* SEQ atoms */
	    for( k = 0; k < fp->nseq; k++, p++ )
		if( !fitem_match( &fp->seq[k], *p ) )
		    break;
	    if( k < fp->nseq )
		continue;	/* try next alternative */

	    /* capture group: SEQ2 atoms then .* to end */
	    {
		const unsigned char *cs = p;
		for( k = 0; k < fp->ncap; k++, p++ )
		    if( !fitem_match( &fp->cap[k], *p ) )
			break;
		if( k < fp->ncap )
		    continue;

		sp[0] = s;
		if( fp->has_alt )
		{
		    sp[1] = s;
		    ep[1] = s + fp->altlen[a];
		    sp[2] = (const char *)cs;
		    ep[2] = (const char *)cs + strlen( (const char *)cs );
		    ep[0] = ep[2];
		}
		else
		{
		    sp[1] = (const char *)cs;
		    ep[1] = (const char *)cs + strlen( (const char *)cs );
		    ep[0] = ep[1];
		}
		return 1;
	    }
	}

	return 0;
}

/* T4: ^(ANY|CLASS)* LIT+ [skip*] ( CLASS1 * ) .* $  anchored dep-line
 *
 * The greedy prefix star backtracks right-to-left, so the winning
 * position for LIT is the RIGHTMOST q with s[0..q) all prefix-matched
 * and s[q..q+litlen) == LIT.  Everything after LIT (skip run, capture
 * run, .*$) matches unconditionally, so the first q found wins.
 */
static int
run_depline( const FASTPAT *fp, const char *s,
	const char **sp, const char **ep )
{
	const unsigned char *p = (const unsigned char *)s;
	const unsigned char *m;
	const unsigned char *end;
	const unsigned char *q;
	const unsigned char *found = 0;
	int slen;

	caps_clear( sp, ep );

	slen = (int)strlen( s );
	end = p + slen;

	/* maximal prefix run from 0 */

	if( fp->pre_any )
	    m = end;		/* '.' matches everything up to NUL */
	else
	{
	    m = p;
	    while( *m && CLS_TEST( fp->cls0, *m ) )
		m++;
	}

	/* rightmost LIT start q, 0 <= q <= m, q+litlen <= slen */

	if( slen >= fp->litlen )
	{
	    q = m;
	    if( q > end - fp->litlen )
		q = end - fp->litlen;

	    for( ; ; q-- )
	    {
		if( *q == (unsigned char)fp->lit[0]
		    && ( fp->litlen == 1
			 || !memcmp( q, fp->lit, fp->litlen ) ) )
		    { found = q; break; }
		if( q == p )
		    break;
	    }
	}

	if( !found )
	    return 0;

	p = found + fp->litlen;

	if( fp->has_skip )
	    while( *p && CLS_TEST( fp->skipcls, *p ) )
		p++;

	sp[1] = (const char *)p;
	while( *p && CLS_TEST( fp->cls1, *p ) )
	    p++;
	ep[1] = (const char *)p;

	sp[0] = s;
	ep[0] = s + slen;
	return 1;
}

/* ------------------------------------------------------------------ */
/* cache                                                               */

typedef struct recache {
	const char *pattern;	/* key: interned pattern text */
	regexp	*re;		/* Spencer-compiled (or NULL) */
	FASTPAT	*fp;
} RECACHE;

static struct hash *re_hash = 0;
static int fastre_enabled = -1;		/* -1 unknown, 0 off, 1 on, 2 check */

/*
 * Pattern strings almost always come from jam LISTs, whose strings are
 * interned by newstr() -- equal patterns share one address.  A tiny
 * direct-mapped memo on that address skips the content-hash lookup for
 * the few patterns that dominate hot loops.  Entries point into the
 * cache hash, whose item storage never moves.
 */
# define RE_MEMO_SLOTS 16
static struct { const char *pat; RECACHE *entry; } re_memo[ RE_MEMO_SLOTS ];

static int
fast_run( const FASTPAT *fp, const char *string,
	const char **sp, const char **ep )
{
	switch( fp->kind )
	{
	case FP_SPLITPLUS:	return run_splitplus( fp, string, sp, ep );
	case FP_SPLITSTAR:	return run_splitstar( fp, string, sp, ep );
	case FP_PREFIX:		return run_prefix( fp, string, sp, ep );
	case FP_DEPLINE:	return run_depline( fp, string, sp, ep );
	}
	return 0;
}

regexp *
re_match( const char *pattern, const char *string, int *matched )
{
	RECACHE centry, *c = &centry;
	unsigned mi = (unsigned)( ( (size_t)pattern >> 4 ) & ( RE_MEMO_SLOTS - 1 ) );

	if( fastre_enabled < 0 )
	{
	    const char *e = getenv( "JAM_FASTRE" );
	    const char *chk = getenv( "JAM_FASTRE_CHECK" );
	    fastre_enabled = ( e && *e == '0' ) ? 0 : 1;
	    if( chk && *chk == '1' )
		fastre_enabled = 2;
	}

	if( re_memo[ mi ].pat == pattern )
	{
	    c = re_memo[ mi ].entry;
	    goto have_entry;
	}

	if( !re_hash )
	    re_hash = hashinit( sizeof( RECACHE ), "regexps" );

	c->pattern = pattern;

	if( hashenter( re_hash, (HASHDATA **)&c ) )
	{
	    /* first sighting: compile both forms once */
	    c->pattern = newstr( pattern );
	    c->re = regcomp( pattern );
	    c->fp = (FASTPAT *)malloc( sizeof( FASTPAT ) );
	    memset( c->fp, 0, sizeof( FASTPAT ) );
	    c->fp->kind = FP_NONE;

	    if( c->re )
	    {
		FTOK toks[ FASTRE_MAXTOK ];
		int n = tokenize( pattern, toks, FASTRE_MAXTOK );
		if( n > 0 )
		    recognize( toks, n, c->fp );
	    }

	    if( getenv( "JAM_FASTRE_DUMP" ) )
	    {
		const char *q;
		fprintf( stderr, "fastre: kind=%d pattern=<", c->fp->kind );
		for( q = pattern; *q; q++ )
		    fprintf( stderr, ( *q >= 32 && *q < 127 ) ? "%c" : "\\x%02x",
			(unsigned char)*q );
		fprintf( stderr, ">\n" );
	    }
	}

	re_memo[ mi ].pat = c->pattern;
	re_memo[ mi ].entry = c;

    have_entry:

	if( !c->re )
	{
	    *matched = 0;
	    return 0;
	}

	if( c->fp->kind != FP_NONE && fastre_enabled == 1 )
	{
	    *matched = fast_run( c->fp, string,
		c->re->startp, c->re->endp );
	    return c->re;
	}

	if( c->fp->kind != FP_NONE && fastre_enabled == 2 )
	{
	    /* validation: run both, compare everything */
	    const char *sp[ NSUBEXP ], *ep[ NSUBEXP ];
	    int fm = fast_run( c->fp, string, sp, ep );
	    int sm = regexec( c->re, string );
	    int i, bad = ( fm != sm );

	    if( !bad && fm )
		for( i = 0; i < NSUBEXP; i++ )
		    if( sp[i] != c->re->startp[i] || ep[i] != c->re->endp[i] )
			{ bad = 1; break; }

	    if( bad )
	    {
		fprintf( stderr,
		    "jam: FASTRE MISMATCH kind=%d pattern='%s' string='%s' "
		    "fast=%d spencer=%d\n",
		    c->fp->kind, pattern, string, fm, sm );
		for( i = 0; i < NSUBEXP; i++ )
		    fprintf( stderr, "  grp%d fast=[%td,%td) spencer=[%td,%td)\n",
			i,
			fm && sp[i] ? sp[i] - string : -1,
			fm && ep[i] ? ep[i] - string : -1,
			sm && c->re->startp[i] ? c->re->startp[i] - string : -1,
			sm && c->re->endp[i] ? c->re->endp[i] - string : -1 );
		exit( 1 );
	    }

	    *matched = sm;
	    return c->re;
	}

	*matched = regexec( c->re, string );
	return c->re;
}
