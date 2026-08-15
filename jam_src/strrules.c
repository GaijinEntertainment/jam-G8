/*
 * strrules.c - C builtins for dagor jBuild's hottest string rules
 *
 * ProcessTargetDigest pushes every compiler-option token of every
 * target through SplitStringsOnSpace / AddEscapesToMakeJamValidForWrite
 * and every object/lib path through MakePath[List]Absolute -- several
 * hundred thousand interpreted rule invocations per run.  These
 * builtins execute the same transforms in C.
 *
 * Backward/forward compatibility contract:
 *  - jam defines the variable JAM_BUILTINS as the list of builtin
 *    rule names provided here.  Jamfiles keep their jam-language
 *    definitions guarded with e.g.
 *        if ! SplitStringsOnSpace in $(JAM_BUILTINS) {
 *            rule SplitStringsOnSpace { ... }
 *        }
 *    On an older jam.exe the variable is unset, the guard passes and
 *    the interpreted rule is defined; on a newer jam.exe the guard
 *    skips the definition and the builtin is used.
 *  - A jamfile that defines these rules unconditionally (old build
 *    scripts on new jam.exe) simply overrides the builtins - rule
 *    definitions always win over builtins in jam.
 *
 * The builtins replicate the interpreted rules from
 * prog/_jBuild/jBuild.jam and jCommonRules.jam BIT-EXACTLY as
 * executed by jam's Spencer regexp engine, including their quirks
 * (kept deliberately so that digests and command lines never change
 * between an old and a new jam.exe):
 *
 *  - SplitStringsOnSpace uses MATCH ([^ ])+ +(.*), and a repeated
 *    group in this engine captures only its LAST repetition: the
 *    first returned element is the last character of the first word
 *    ("abc def" -> "c" "def").  A trailing separator yields an empty
 *    second element ("x " -> "x" "").
 *  - AddEscapesToMakeJamValidForWrite drops the tail after the last
 *    quote/backslash ("a\"b" -> "a\"") because the loop's final
 *    failing MATCH contributes nothing.
 *  - The ../-stripping MATCH pattern ^\.\.\/(.*) reaches regcomp as
 *    ^../(.*) (the jamfile scanner consumes the backslashes), and
 *    '.' matches ANY character: every 2-character path segment pops
 *    one prefix component ("../ui/x" pops twice), a bare "../" pops
 *    nothing (empty rest), and pops saturate at the filesystem root
 *    ("d:/" stays "d:/", giving "d://x" results).
 *
 * One knowingly unreplicated corner: the interpreted rules' internal
 * names are plain jam variables, so a target carrying an on-target
 * variable named like a rule-local (`dep on hdr.h = x`, `changed_dep
 * on hdr.h = x`) would perturb the interpreted `on $(dep)` expansion;
 * the builtins use their C values.  jBuild never sets such variables.
 * The rules' leaked FOR loop variables (s / dep / dep_full) ARE
 * replicated -- see leak_loopvar().
 *
 * Only the list shapes jBuild actually uses are replicated exactly:
 * any number of elements for the list rules, and a single-element
 * prefix/suffix for MakePathAbsolute's ..* branch (multi-element
 * suffixes starting with ".." are processed per element).
 */

# include "jam.h"

# include "lists.h"
# include "parse.h"
# include "rules.h"
# include "variable.h"
# include "newstr.h"
# include "pathsys.h"
# include "compile.h"
# include "builtins.h"
# include "prof.h"

# include <string.h>
# include <stdlib.h>

/* the procedure load_builtins() bound Includes to */
static PARSE *includes_builtin;


/* ------------------------------------------------------------------ */

/*
 * leak_loopvar() - replicate compile_foreach's bare var_set: an
 * interpreted `for x in list` leaves x bound to the LAST element (and
 * untouched for an empty list).  Observable by the caller, so the
 * builtins reproduce it.
 */

static void
leak_loopvar( const char *name, const char *last )
{
	if( last )
	    var_set( name, list_new( L0, last, 1 ), VAR_SET );
}

/*
 * SplitStringsOnSpace list
 *
 * jam equivalent (jBuild.jam):
 *   for s in $(1) {
 *     local split = [ MATCH ([^\ ])+\ +(.*) : $(s) ] ;
 *     if $(split) { ret += $(split[1]) $(split[2]) ; } else { ret += $(s) ; }
 *   }
 */

static LIST *
builtin_split_on_space(
	PARSE	*parse,
	LOL	*args,
	int	*jmp )
{
	LIST *l;
	LIST *out = L0;
	const char *last = 0;

	for( l = lol_get( args, 0 ); l; l = list_next( l ) )
	{
	    const char *s = l->string;
	    int i = 0, j;

	    last = s;
	    int matched = 0;

	    /* leftmost maximal non-space run followed by a space */

	    while( s[i] )
	    {
		if( s[i] != ' ' )
		{
		    j = i;
		    while( s[j] && s[j] != ' ' )
			j++;
		    if( s[j] == ' ' )
		    {
			/* group 1: last repetition = last run char */
			char g1[2];
			int k = j;

			g1[0] = s[j-1];
			g1[1] = 0;

			while( s[k] == ' ' )
			    k++;

			out = list_new( out, g1, 0 );
			out = list_new( out, s + k, 0 );
			matched = 1;
			break;
		    }
		    i = j;	/* run reached end of string: no match */
		}
		else
		    i++;
	    }

	    if( !matched )
		out = list_new( out, l->string, 1 );
	}

	leak_loopvar( "s", last );
	return out;
}

/* ------------------------------------------------------------------ */

/*
 * AddEscapesToMakeJamValidForWrite list
 *
 * jam equivalent (jBuild.jam): split at each quote/backslash with
 * MATCH ([^"\]*)(["\])(.*), prefix the special char with a backslash
 * and join.  The trailing text after the LAST special character is
 * dropped (the final failing MATCH contributes nothing) - replicated.
 */

static LIST *
builtin_add_escapes(
	PARSE	*parse,
	LOL	*args,
	int	*jmp )
{
	LIST *l;
	LIST *out = L0;
	const char *last = 0;

	for( l = lol_get( args, 0 ); l; l = list_next( l ) )
	{
	    const char *s = l->string;
	    const char *k = s + strcspn( s, "\"\\" );

	    last = l->string;
	    char *buf, *w;

	    if( !*k )
	    {
		/* no specials: element passes through unchanged */
		out = list_new( out, l->string, 1 );
		continue;
	    }

	    /* worst case doubles every char */
	    buf = (char *)malloc( strlen( s ) * 2 + 2 );
	    w = buf;

	    memcpy( w, s, k - s );
	    w += k - s;

	    for( ;; )
	    {
		*w++ = '\\';
		*w++ = *k++;

		s = k;
		k = s + strcspn( s, "\"\\" );

		if( !*k )
		    break;	/* tail after last special is dropped */

		memcpy( w, s, k - s );
		w += k - s;
	    }

	    *w = 0;
	    out = list_new( out, buf, 0 );
	    free( buf );
	}

	leak_loopvar( "s", last );
	return out;
}

/* ------------------------------------------------------------------ */

/*
 * path_dirof() - exactly $(path:D)
 */

static void
path_dirof( const char *in, char *outbuf )
{
	PATHNAME f;

	path_parse( in, &f );

	f.f_grist.ptr = "";  f.f_grist.len = 0;
	f.f_root.ptr = "";   f.f_root.len = 0;
	f.f_base.ptr = "";   f.f_base.len = 0;
	f.f_suffix.ptr = ""; f.f_suffix.len = 0;
	f.f_member.ptr = ""; f.f_member.len = 0;
	/* f_dir kept as parsed */

	path_build( &f, outbuf, 0 );
}

/*
 * simplify_composed() - SimplifyComposedPath $(prefix) : $(suffix)
 *
 * while suffix matches ^../(.*) with non-empty rest (NB '.' = ANY:
 * any two chars followed by '/') and prefix is non-empty:
 * pop one prefix component, continue with the rest.
 * Result: "prefix/suffix".
 */

static LIST *
simplify_composed( LIST *out, const char *prefix0, const char *suffix )
{
	char prefix[ MAXJPATH ];
	char result[ MAXJPATH * 2 + 2 ];
	int plen;

	if( strlen( prefix0 ) >= sizeof( prefix )
	    || strlen( suffix ) >= MAXJPATH )
	{
	    /* over-long (not expected): the pops are skipped defensively
	     * -- the fixed-size pop buffers below cannot hold this --
	     * but the result still joins $(prefix)/$(suffix) like the
	     * interpreted rule */

	    size_t pl = strlen( prefix0 ), sl = strlen( suffix );
	    char *big = (char *)malloc( pl + sl + 2 );

	    memcpy( big, prefix0, pl );
	    big[ pl ] = '/';
	    memcpy( big + pl + 1, suffix, sl + 1 );
	    out = list_new( out, big, 0 );
	    free( big );
	    return out;
	}

	strcpy( prefix, prefix0 );

	while( prefix[0]
	    && suffix[0] && suffix[1] && suffix[2] == '/' && suffix[3] )
	{
	    char popped[ MAXJPATH ];

	    suffix += 3;
	    path_dirof( prefix, popped );
	    strcpy( prefix, popped );
	}

	plen = (int)strlen( prefix );
	memcpy( result, prefix, plen );
	result[ plen ] = '/';
	strcpy( result + plen + 1, suffix );

	return list_new( out, result, 0 );
}

/*
 * MakePathAbsolute prefix : suffix
 * MakePathListAbsolute prefix : suffixList
 *
 * jam equivalent (jCommonRules.jam): suffixes matching ..* go through
 * SimplifyComposedPath, everything else is returned unchanged.
 */

static LIST *
make_path_absolute_one( LIST *out, LIST *prefix, const char *suffix )
{
	if( suffix[0] == '.' && suffix[1] == '.' )
	{
	    /* $(prefix)/$(suffix) is a list product: empty prefix
	     * list yields an empty result */
	    if( !prefix )
		return out;
	    return simplify_composed( out, prefix->string, suffix );
	}

	return list_new( out, suffix, 0 );
}

/*
 * SimplifyComposedPath as a callable builtin.  The interpreted
 * MakePathAbsolute dispatches to SimplifyComposedPath BY NAME (and
 * MakePathListAbsolute to MakePathAbsolute), so a jamfile override
 * must keep working: the builtins below re-dispatch through
 * evaluate_rule.  load_strrules() gives every one of these rule
 * names a procedure, and an override merely replaces it, so the
 * ->procedure tests below are always true and the builtins ALWAYS
 * dispatch: into the override when one is defined, back into the
 * sibling builtin otherwise.  The per-element loops behind the
 * tests are an unreachable safety net, not an equivalent inline
 * path, and must not replace the dispatch: a multi-element ..*
 * suffix is fed to SimplifyComposedPath as a whole list
 * ([ MakePathAbsolute p : ../a b ] -> "/a p/b"), while the loop
 * switches per element ("/a b").
 */

static LIST *
builtin_simplify_composed(
	PARSE	*parse,
	LOL	*args,
	int	*jmp )
{
	LIST *prefix = lol_get( args, 0 );
	LIST *l;
	LIST *out = L0;

	if( !prefix )
	    return L0;	/* empty product, like $(prefix)/$(suffix) */

	for( l = lol_get( args, 1 ); l; l = list_next( l ) )
	    out = simplify_composed( out, prefix->string, l->string );

	return out;
}

static LIST *
dispatch_rule2( const char *rulename, LIST *arg0, LIST *arg1 )
{
	LOL lol;
	LIST *r;

	lol_init( &lol );
	lol_add( &lol, list_copy( L0, arg0 ) );
	lol_add( &lol, list_copy( L0, arg1 ) );
	r = evaluate_rule( rulename, &lol, L0 );
	lol_free( &lol );

	return r;
}

static LIST *
builtin_make_path_absolute(
	PARSE	*parse,
	LOL	*args,
	int	*jmp )
{
	LIST *prefix = lol_get( args, 0 );
	LIST *suffix = lol_get( args, 1 );
	LIST *out = L0;

	if( !suffix )
	    return L0;

	/* the interpreted rule switches on the first element and, for
	 * the plain case, returns the whole list unchanged */

	if( !( suffix->string[0] == '.' && suffix->string[1] == '.' ) )
	    return list_copy( L0, suffix );

	/* honor a jamfile-defined SimplifyComposedPath, exactly like
	 * the interpreted MakePathAbsolute's named call would */

	if( bindrule( "SimplifyComposedPath" )->procedure )
	    return dispatch_rule2( "SimplifyComposedPath", prefix, suffix );

	for( ; suffix; suffix = list_next( suffix ) )
	    out = make_path_absolute_one( out, prefix, suffix->string );

	return out;
}

static LIST *
builtin_make_path_list_absolute(
	PARSE	*parse,
	LOL	*args,
	int	*jmp )
{
	LIST *prefix = lol_get( args, 0 );
	LIST *l;
	LIST *out = L0;
	int dispatch = bindrule( "MakePathAbsolute" )->procedure != 0;

	for( l = lol_get( args, 1 ); l; l = list_next( l ) )
	{
	    if( dispatch )
	    {
		LIST *one = list_new( L0, l->string, 1 );
		out = list_append( out,
		    dispatch_rule2( "MakePathAbsolute", prefix, one ) );
		list_free( one );
	    }
	    else if( l->string[0] == '.' && l->string[1] == '.' )
		out = make_path_absolute_one( out, prefix, l->string );
	    else
		out = list_new( out, l->string, 1 );
	}

	return out;
}

/* ------------------------------------------------------------------ */
/* DepRule builtins: C versions of the HDRRULE bodies from            */
/* _jBuild/windows/{msvc,clang}-cpp.jam.  The toolchain jamfiles set  */
/* HDRRULE to these names when available (JAM_BUILTINS guard) and     */
/* keep identically-named interpreted fallbacks for older jam.exe.    */
/* These run once per scanned dependency file during make0, so they   */
/* stay hot even when the parse phase and depcache are already fast.  */

/*
 * The same dependency names recur across nearly every scanned file --
 * every translation unit pulls in the same engine headers -- so this
 * transform otherwise rebuilds and re-interns the identical
 * "$(Root)/<dep>" string tens of times per name.  Memoize it on the
 * ADDRESS of the dep name, and rebuild the memo whenever $(Root)
 * changes, which is the only other input.
 *
 * Keying on an address is sound because the bytes it points at are
 * immortal, not because the pointer came from newstr(): the names
 * arrive interned from the scan cache or newstr, freestr() is a no-op
 * and list_free() recycles only the LIST nodes, so a stored key can
 * never be freed and re-used for different bytes.  DepRuleDotDot
 * relies on exactly that - it hands us an INTERIOR pointer (dep += 3
 * past each "../"), which is not a newstr() result but is just as
 * permanent.  Equal pointer therefore still implies equal name; two
 * different pointers to equal text merely cost an extra miss.
 *
 * A miss does the same work the interpreted rule did, plus one
 * list_copy so the memo can own its answer.
 */

typedef struct {
	const char	*dep;		/* key: address of an immortal name */
	LIST		*rooted;	/* $(Root)/dep, one per Root element */
} DR_MEMO;

# define DR_MEMO_BITS 15
# define DR_MEMO_SLOTS ( 1 << DR_MEMO_BITS )

static DR_MEMO dr_memo[ DR_MEMO_SLOTS ];

/*
 * Which $(Root) the memo was built against.  Comparing the LIST head
 * pointer is NOT enough: list_append() grafts onto an existing list in
 * place, so `Root += x` leaves the head unchanged while the contents
 * differ - the memo would then hand out paths missing the new element.
 * Keep the element strings (interned, so pointer equality is content
 * equality) and compare them exactly.  A Root longer than we track
 * simply disables the memo rather than risking a stale answer - and
 * because such a Root can never compare same, the flush below is
 * conditional, or every call would pay for a whole-table wipe.
 */

# define DR_ROOT_MAX 8

static const char *dr_root[ DR_ROOT_MAX ];
static int dr_root_n = -1;		/* -1: nothing memoized yet */

static int
dr_root_same( LIST *root )
{
	int n = 0;

	for( ; root && n < DR_ROOT_MAX; root = list_next( root ), n++ )
	    if( dr_root[n] != root->string )
		return 0;

	return !root && n == dr_root_n;
}

static int
dr_root_remember( LIST *root )
{
	int n = 0;

	for( ; root && n < DR_ROOT_MAX; root = list_next( root ), n++ )
	    dr_root[n] = root->string;

	if( root )
	{
	    dr_root_n = -1;	/* too long to track: memo stays off */
	    return 0;
	}

	dr_root_n = n;
	return 1;
}

static void
dr_memo_flush( void )
{
	int i;

	for( i = 0; i < DR_MEMO_SLOTS; i++ )
	    if( dr_memo[i].dep )
	    {
		list_free( dr_memo[i].rooted );
		dr_memo[i].dep = 0;
		dr_memo[i].rooted = 0;
	    }
}

static LIST *
deprule_emit_rooted( LIST *out, LIST *root, const char *dep )
{
	char buf[ MAXJPATH * 2 + 2 ];
	int dl = (int)strlen( dep );
	unsigned i;
	LIST *l;
	int memoize;

	/* $(Root) is a plain variable and a target may shadow it, so the
	 * memo is only valid while the list it was built from is */

	if( !dr_root_same( root ) )
	{
	    /* Only flush when something can actually be in the table.  A
	     * Root too long to track never compares same, so without this
	     * guard every single call would wipe the whole memo - far more
	     * expensive than the interpreted rule it replaces. */

	    if( dr_root_n >= 0 )
		dr_memo_flush();

	    memoize = dr_root_remember( root );
	}
	else
	    memoize = dr_root_n >= 0;

	i = (unsigned)( ( (size_t)dep >> 4 ) * 2654435761u ) & ( DR_MEMO_SLOTS - 1 );

	if( memoize && dr_memo[i].dep == dep )
	    return list_append( out, list_copy( L0, dr_memo[i].rooted ) );

	l = L0;
	for( ; root; root = list_next( root ) )
	{
	    int rl = (int)strlen( root->string );

	    if( rl + dl + 2 >= (int)sizeof( buf ) )
		continue;	/* over-long: drop defensively */

	    memcpy( buf, root->string, rl );
	    buf[ rl ] = '/';
	    memcpy( buf + rl + 1, dep, dl + 1 );
	    l = list_new( l, buf, 0 );
	}

	if( memoize )
	{
	    /* direct-mapped: the slot's previous occupant is evicted, and
	     * the memo is its only owner, so hand it back to the freelist */

	    if( dr_memo[i].dep )
		list_free( dr_memo[i].rooted );

	    dr_memo[i].dep = dep;
	    dr_memo[i].rooted = l;
	    return list_append( out, list_copy( L0, l ) );
	}

	return list_append( out, l );
}

static int
deprule_is_abs( const char *dep )
{
	if( dep[0] && dep[1] == ':' )		/* ?:*  drive          */
	    return 1;
	if( dep[0] == '/' )			/* /*   rooted (slash) */
	    return 1;

	/* The jamfile's `case \\* :` looks like it should match
	 * backslash-rooted paths, but the scanner turns the token into
	 * the glob pattern \* which matches only the literal string "*".
	 * Backslash-rooted paths therefore fall through to the Root/
	 * case - replicated faithfully. */

	if( dep[0] == '*' && !dep[1] )
	    return 1;

	return 0;
}

/*
 * deprule_finish() - the shared rule tail:
 *     Includes $(<) : $(changed) ;
 *     null_action $(changed) ;
 *
 * Both are performed inline rather than through evaluate_rule(): this
 * runs once per scanned dependency file, and the two rule invocations
 * (argument LOL setup, rule lookup, parse-tree dispatch, result list
 * churn) cost more than the graph work itself.  The effect is
 * identical to invoking them - builtin_depends() with parse->num set
 * is exactly the loop below, and a rule with actions and no procedure
 * only ever appends one ACTION to each target.
 *
 * `null_action` is looked up by name, so a jamfile is still free to
 * redefine it; if it somehow has a procedure (the stock one is a bare
 * `actions`), fall back to a real invocation so semantics hold.
 *
 * Takes ownership of `changed`.
 */

static void
deprule_finish( LOL *args, LIST *changed )
{
	LIST	*targets = lol_get( args, 0 );
	LIST	*l;
	RULE	*na = bindrule( "null_action" );
	RULE	*inc = bindrule( "Includes" );

	/* Includes $(<) : $(changed) ; -- dispatched for real when a
	 * jamfile overrode the builtin, inlined otherwise.
	 *
	 * NB the builtin itself HAS a procedure (load_builtins binds it
	 * to builtin_depends), so testing inc->procedure alone is always
	 * true: compare against the procedure load_builtins installed,
	 * which is what actually marks an override.
	 *
	 * `actions Includes { ... }` is an override too, even though it
	 * leaves the procedure alone: evaluate_rule() attaches the ACTION
	 * before it ever looks at the procedure, so inlining would drop
	 * it.  Same split the null_action dispatch below makes. */

	if( inc->procedure != includes_builtin || inc->actions )
	{
	    LOL lol;

	    lol_init( &lol );
	    lol_add( &lol, list_copy( L0, targets ) );
	    lol_add( &lol, list_copy( L0, changed ) );
	    list_free( evaluate_rule( "Includes", &lol, L0 ) );
	    lol_free( &lol );
	}
	else for( l = targets; l; l = list_next( l ) )
	{
	    TARGET *t = bindtarget( l->string );

	    if( !t->includes )
		t->includes = copytarget( t );

	    t->includes->depends =
		targetlist_interned( t->includes->depends, changed );
	}

	/* null_action $(changed) ; */

	if( na->procedure )
	{
	    LOL lol;

	    lol_init( &lol );
	    lol_add( &lol, changed );		/* ownership transferred */
	    list_free( evaluate_rule( "null_action", &lol, L0 ) );
	    lol_free( &lol );
	    return;
	}

	if( na->actions )
	{
	    ACTION *action = (ACTION *)malloc( sizeof( ACTION ) );
	    TARGETS *t;

	    memset( (char *)action, '\0', sizeof( *action ) );
	    action->rule = na;
	    action->targets = targetlist_interned( (TARGETS *)0, changed );
	    action->sources = targetlist( (TARGETS *)0, L0 );

	    for( t = action->targets; t; t = t->next )
		t->target->actions = actionlist( t->target->actions, action );
	}
	else
	    printf( "warning: unknown rule %s\n", na->name );

	list_free( changed );
}

/*
 * DepRulePlain target : deps
 *
 * jam equivalent (windows/msvc-cpp.jam DepRule):
 *   for dep in $(>) { switch $(dep) {
 *     case ?:* : keep ; case \\* : keep ; case /* : keep ;
 *     case *   : $(Root)/$(dep) ; } }
 */

static LIST *
builtin_deprule_plain(
	PARSE	*parse,
	LOL	*args,
	int	*jmp )
{
	LIST *root = var_get( "Root" );
	LIST *l;
	LIST *changed = L0;
	const char *last = 0;

	PROF_ENTER( PROF_DR_XFORM );

	for( l = lol_get( args, 1 ); l; l = list_next( l ) )
	{
	    last = l->string;

	    if( deprule_is_abs( l->string ) )
		changed = list_new( changed, l->string, 1 );
	    else
		changed = deprule_emit_rooted( changed, root, l->string );
	}

	leak_loopvar( "dep", last );
	PROF_LEAVE( PROF_DR_XFORM );
	PROF_ENTER( PROF_DR_FINISH );
	deprule_finish( args, changed );
	PROF_LEAVE( PROF_DR_FINISH );
	return L0;
}

/*
 * DepRuleDotDot target : deps
 *
 * jam equivalent (windows/clang-cpp.jam DepRule): strip all leading
 * "../" / "..\" pairs, then
 *   case ?:* | \\* | /* : keep ;
 *   case ./*            : on $(dep) $(Root)/$(location_prefix)/$(dep) ;
 *   case *              : $(Root)/$(dep) ;
 */

static LIST *
builtin_deprule_dotdot(
	PARSE	*parse,
	LOL	*args,
	int	*jmp )
{
	LIST *root = var_get( "Root" );
	LIST *l;
	LIST *changed = L0;
	const char *last = 0;
#if defined(OS_MACOSX)
	static char _devtool_prefix[1024] = { 0 };
	static int _devtool_prefix_len = -1;
	if( _devtool_prefix_len < 0 )
	{
	    LIST *_devtool = var_get( "_DEVTOOL" );
	    _devtool_prefix_len = 0;
	    if( _devtool && _devtool->string && *_devtool->string )
	    {
	      _snprintf( _devtool_prefix, sizeof( _devtool_prefix ), "%s/mac/SDK", _devtool->string );
	      _devtool_prefix_len = strlen( _devtool_prefix );
	    }
	}
#endif
	int first_dep_str_checked = 0;

	PROF_ENTER( PROF_DR_XFORM );

	for( l = lol_get( args, 1 ); l; l = list_next( l ) )
	{
	    const char *dep = l->string;

	    // skip optional first line like 'xxx.o: \', with spaces and '\' stripped by HDRSCAN
	    if (!first_dep_str_checked)
	    {
	      first_dep_str_checked = 1;
	      size_t slen = strlen(dep);
	      if (slen > 1 && dep[slen-1] == ':')
	        continue;
	    }

	    last = l->string;

#if defined(OS_MACOSX)
	    if( *dep == '/' &&
	        ( strncmp( dep, "/Applications/Xcode.app/", 24 ) == 0 ||
	          ( _devtool_prefix_len && strncmp( dep, _devtool_prefix, _devtool_prefix_len ) == 0 ) ) )
	      continue; /* skip dep to quasi-invariant SDK */
#endif
	    /* MATCH "(\.\./|\.\.\\)*(.*)" : take group 2 */

	    while( dep[0] == '.' && dep[1] == '.'
		&& ( dep[2] == '/' || dep[2] == '\\' ) )
		dep += 3;

	    if( deprule_is_abs( dep ) )
	    {
		changed = list_new( changed, dep, 0 );
	    }
	    else if( dep[0] == '.' && dep[1] == '/' )
	    {
		/* on $(dep) changed += $(Root)/$(location_prefix)/$(dep)
		 * - location_prefix is read under the dep target's
		 * settings; the append is a triple list product */

		TARGET *t = bindtarget( dep );
		SETTINGS *s = copysettings( t->settings );
		LIST *lp;
		LIST *root_on;
		LIST *r;

		pushsettings( s );
		lp = var_get( "location_prefix" );

		/* the interpreted rule expands $(Root) INSIDE `on $(dep)`,
		 * so an on-target Root setting must win here too */
		root_on = var_get( "Root" );

		for( r = root_on; r; r = list_next( r ) )
		{
		    LIST *p;
		    for( p = lp; p; p = list_next( p ) )
		    {
			char buf[ MAXJPATH * 2 + 2 ];
			if( strlen( r->string ) + strlen( p->string )
			    + strlen( dep ) + 3 >= sizeof( buf ) )
			    continue;
			sprintf( buf, "%s/%s/%s", r->string, p->string, dep );
			changed = list_new( changed, buf, 0 );
		    }
		}

		popsettings( s );
		freesettings( s );
	    }
	    else
	    {
		changed = deprule_emit_rooted( changed, root, dep );
	    }
	}

	leak_loopvar( "dep_full", last );
	PROF_LEAVE( PROF_DR_XFORM );
	PROF_ENTER( PROF_DR_FINISH );
	deprule_finish( args, changed );
	PROF_LEAVE( PROF_DR_FINISH );
	return L0;
}

/* ------------------------------------------------------------------ */

# define P0 (PARSE *)0
# define C0 (char *)0

void
load_strrules()
{
	LIST *names = L0;

	/* What load_builtins() bound Includes to, so deprule_finish can
	 * tell the builtin from a jamfile override.  Hold a reference:
	 * redefining a rule parse_free()s the old procedure (compile.c),
	 * and a freed PARSE whose address a later parse_make() reuses
	 * would make an override compare equal to the builtin - silently
	 * dropping the override and building the wrong graph.  Includes
	 * and INCLUDES share this one node, so the extra ref also closes
	 * a dangle that predates these builtins. */

	includes_builtin = bindrule( "Includes" )->procedure;
	parse_refer( includes_builtin );

	bindrule( "SplitStringsOnSpace" )->procedure =
	    parse_make( builtin_split_on_space, P0, P0, P0, C0, C0, 0 );

	bindrule( "AddEscapesToMakeJamValidForWrite" )->procedure =
	    parse_make( builtin_add_escapes, P0, P0, P0, C0, C0, 0 );

	bindrule( "SimplifyComposedPath" )->procedure =
	    parse_make( builtin_simplify_composed, P0, P0, P0, C0, C0, 0 );

	bindrule( "MakePathAbsolute" )->procedure =
	    parse_make( builtin_make_path_absolute, P0, P0, P0, C0, C0, 0 );

	bindrule( "MakePathListAbsolute" )->procedure =
	    parse_make( builtin_make_path_list_absolute, P0, P0, P0, C0, C0, 0 );

	bindrule( "DepRulePlain" )->procedure =
	    parse_make( builtin_deprule_plain, P0, P0, P0, C0, C0, 0 );

	bindrule( "DepRuleDotDot" )->procedure =
	    parse_make( builtin_deprule_dotdot, P0, P0, P0, C0, C0, 0 );

	/* feature detection for jamfiles: guard interpreted fallback
	 * definitions with   if ! <name> in $(JAM_BUILTINS) { rule ... }
	 * VAR_DEFAULT keeps a -sJAM_BUILTINS= override intact. */

	names = list_new( names, "SplitStringsOnSpace", 0 );
	names = list_new( names, "AddEscapesToMakeJamValidForWrite", 0 );
	names = list_new( names, "SimplifyComposedPath", 0 );
	names = list_new( names, "MakePathAbsolute", 0 );
	names = list_new( names, "MakePathListAbsolute", 0 );
	names = list_new( names, "DepRulePlain", 0 );
	names = list_new( names, "DepRuleDotDot", 0 );

	var_set( "JAM_BUILTINS", names, VAR_DEFAULT );
}

