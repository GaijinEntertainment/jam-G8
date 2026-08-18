/*
 * Copyright 1993-2002 Christopher Seiwald and Perforce Software, Inc.
 *
 * This file is part of Jam - see jam.c for Copyright information.
 */

/*
 * make1.c - execute command to bring targets up to date
 *
 * This module contains make1(), the entry point called by make() to 
 * recursively decend the dependency graph executing update actions as
 * marked by make0().
 *
 * External routines:
 *
 *	make1() - execute commands to update a TARGET and all its dependents
 *
 * Internal routines, the recursive/asynchronous command executors:
 *
 *	make1a() - recursively traverse target tree, calling make1b()
 *	make1b() - dependents of target built, now build target with make1c()
 *	make1c() - launch target's next command, call make1b() when done
 *	make1d() - handle command execution completion and call back make1c()
 *
 * Internal support routines:
 *
 *	make1cmds() - turn ACTIONS into CMDs, grouping, splitting, etc
 *	make1list() - turn a list of targets into a LIST, for $(<) and $(>)
 * 	make1settings() - for vars that get bound, build up replacement lists
 * 	make1bind() - bind targets that weren't bound in dependency analysis
 *
 * 04/16/94 (seiwald) - Split from make.c.
 * 04/21/94 (seiwald) - Handle empty "updated" actions.
 * 05/04/94 (seiwald) - async multiprocess (-j) support
 * 06/01/94 (seiwald) - new 'actions existing' does existing sources
 * 12/20/94 (seiwald) - NOTIME renamed NOTFILE.
 * 01/19/95 (seiwald) - distinguish between CANTFIND/CANTMAKE targets.
 * 01/22/94 (seiwald) - pass per-target JAMSHELL down to execcmd().
 * 02/28/95 (seiwald) - Handle empty "existing" actions.
 * 03/10/95 (seiwald) - Fancy counts.
 * 02/07/01 (seiwald) - Fix jam -d0 return status.
 * 01/21/02 (seiwald) - new -q to quit quickly on build failure
 * 02/28/02 (seiwald) - don't delete 'actions updated' targets on failure
 * 02/28/02 (seiwald) - merge EXEC_xxx flags in with RULE_xxx 
 * 07/17/02 (seiwald) - TEMPORARY sources for headers now get built
 * 09/23/02 (seiwald) - "...using temp..." only displayed on -da now.
 * 10/22/02 (seiwald) - list_new() now does its own newstr()/copystr()
 * 11/04/02 (seiwald) - const-ing for string literals
 * 12/03/02 (seiwald) - fix odd includes support by grafting them onto depends
 */

# include "jam.h"

# include "lists.h"
# include "parse.h"
# include "variable.h"
# include "rules.h"

# include "search.h"
# include "newstr.h"
# include "make.h"
# include "command.h"
# include "execcmd.h"
# include "prof.h"
# ifdef OS_NT
# include <windows.h>
# include <io.h>
# endif

static void make1a( TARGET *t, TARGET *parent );
static void make1b( TARGET *t );
static void make1c( TARGET *t );
static void make1d( void *closure, int status, int time_spent_msec );

static CMD *make1cmds( ACTIONS *a0 );
static LIST *make1list( LIST *l, TARGETS *targets, int flags );
static SETTINGS *make1settings( LIST *vars );
static void make1bind( TARGET *t, int warn );

/* Ugly static - it's too hard to carry it through the callbacks. */

static struct {
	int	failed;
	int	skipped;
	int	total;
	int	made;
	int totalToUpdate;
	unsigned perfDoneTargets;
	unsigned long long perfDoneTargetsTimeMsec;
} counts[1] ;

/*
 * make1() - execute commands to update a TARGET and all its dependents
 */

static int intr = 0;
static long long build_start_reft = 0;
static long long last_report_reft = 0;
static int last_report_done_targets = 0;
static int tty_colored = -1;

# ifdef OS_NT
static HANDLE tty_vt_enabled_by_us = INVALID_HANDLE_VALUE;

static void tty_restore_mode( void )
{
	DWORD mode;

	if( tty_vt_enabled_by_us == INVALID_HANDLE_VALUE )
	    return;

	if( GetConsoleMode( tty_vt_enabled_by_us, &mode ) )
	    SetConsoleMode( tty_vt_enabled_by_us, mode & ~ENABLE_VIRTUAL_TERMINAL_PROCESSING );

	tty_vt_enabled_by_us = INVALID_HANDLE_VALUE;
}
# endif

static void tty_check_colored()
{
# ifdef OS_NT
	HANDLE h = (HANDLE)_get_osfhandle( _fileno( stdout ) );
	DWORD mode;

	tty_colored = 0;

	if( h == INVALID_HANDLE_VALUE || !GetConsoleMode( h, &mode ) )
	    return; /* not a console: no colours */

	if( mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING )
	{
	    tty_colored = 1; /* the host enabled it: leave it be */
	    return;
	}

	if( !SetConsoleMode( h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING ) )
	    return; /* console too old for VT */

	tty_vt_enabled_by_us = h;
	atexit( tty_restore_mode );
	tty_colored = 1;
# else
	tty_colored = isatty( fileno( stdout ) ) != 0;
# endif
}
static const char *tty_green_text_string()
{
	if (tty_colored < 0)
	  tty_check_colored();
	return (tty_colored > 0) ? "\033[32m" : "";
}
static const char *tty_reset_text_color_string()
{
	if (tty_colored < 0)
	  tty_check_colored();
	return (tty_colored > 0) ? "\033[0m" : "";
}

static const char *seconds2str(char *buf, int buflen, double t_sec)
{
	if( t_sec > 3600.0 )
	{
		int t = (int)( t_sec + 0.5 );
		_snprintf( buf, buflen, "%d:%02d:%02d", t / 3600, ( t % 3600 ) / 60, t % 60 );
	}
	else if( t_sec > 60.0 )
	{
		int t = (int)( t_sec + 0.5 );
		_snprintf( buf, buflen, "%d:%02d", t / 60, t % 60 );
	}
	else
		_snprintf( buf, buflen, "%.1f sec", t_sec );
	return buf;
}

int
make1( TARGET *t, int total_targets_to_update )
{
	int base_failed = 0, base_skipped = 0, base_made = 0, base_total = 0;
	static int run_started = 0;
	if( !run_started )
	{
	    run_started = 1;
	    memset( (char *)counts, 0, sizeof( *counts ) );
	    counts->totalToUpdate = total_targets_to_update;
	    build_start_reft = last_report_reft = perf_timer_now();
	    last_report_done_targets = 0;
	}
	else
  	base_failed = counts->failed, base_skipped = counts->skipped, base_made = counts->made, base_total = counts->total;

	/* Recursively make the target and its dependents */

	make1a( t, (TARGET *)0 );

	/* Wait for any outstanding commands to finish running. */

	while( execwait() )
	    ;

	/* Talk about it */

	if( DEBUG_MAKE && counts->failed > base_failed )
	    printf( "...failed updating %d target(s)...\n", counts->failed - base_failed );

	if( DEBUG_MAKE && counts->skipped > base_skipped )
	    printf( "...skipped %d target(s)...\n", counts->skipped - base_skipped );

	if( DEBUG_MAKE && counts->made > base_made )
	    printf( "...updated %d target(s)...\n", counts->made - base_made );

	return ( counts->total - base_total ) != ( counts->made - base_made );
}

/*
 * make1a() - recursively traverse target tree, calling make1b()
 */

static void
make1a( 
	TARGET	*t,
	TARGET	*parent )
{
	TARGETS	*c;

	/* If the parent is the first to try to build this target */
	/* or this target is in the make1c() quagmire, arrange for the */
	/* parent to be notified when this target is built. */

	if( parent )
	    switch( t->progress )
	{
	case T_MAKE_INIT:
	case T_MAKE_ACTIVE:
	case T_MAKE_RUNNING:
	    t->parents = targetentry( t->parents, parent );
	    parent->asynccnt++;
	}

	if( t->progress != T_MAKE_INIT )
	    return;

	/* Asynccnt counts the dependents preventing this target from */
	/* proceeding to make1b() for actual building.  We start off with */
	/* a count of 1 to prevent anything from happening until we can */
	/* call all dependents.  This 1 is accounted for when we call */
	/* make1b() ourselves, below. */

	t->asynccnt = 1;

	/* Recurse on our dependents, manipulating progress to guard */
	/* against circular dependency. */

	t->progress = T_MAKE_ONSTACK;

	for( c = t->depends; c && !intr; c = c->next )
	    make1a( c->target, t );

	t->progress = T_MAKE_ACTIVE;

	/* Now that all dependents have bumped asynccnt, we now allow */
	/* decrement our reference to asynccnt. */ 

	make1b( t );
}

/*
 * make1b() - dependents of target built, now build target with make1c()
 */

static void
make1b( TARGET *t )
{
	TARGETS	*c;
	const char *failed = "dependents";

	/* If any dependents are still outstanding, wait until they */
	/* call make1b() to signal their completion. */

	if( --t->asynccnt )
	    return;

	/* Now ready to build target 't'... if dependents built ok. */

	/* Collect status from dependents */

	for( c = t->depends; c; c = c->next )
	    if( c->target->status > t->status )
	{
	    failed = c->target->name;
	    t->status = c->target->status;
	}

	/* If actions on deps have failed, bail. */
	/* Otherwise, execute all actions to make target */

	if( t->status == EXEC_CMD_FAIL && t->actions )
	{
	    ++counts->skipped;
	    printf( "...skipped %s for lack of %s...\n", t->name, failed );
	}

	if( t->status == EXEC_CMD_OK )
	    switch( t->fate )
	{
	case T_FATE_INIT:
	case T_FATE_MAKING:
	    /* shouldn't happen */

	case T_FATE_STABLE:
	case T_FATE_NEWER:
	    break;

	case T_FATE_CANTFIND:
	case T_FATE_CANTMAKE:
	    t->status = EXEC_CMD_FAIL;
	    break;

	case T_FATE_ISTMP:
	    if( DEBUG_MAKEQ )
		printf( "...using %s...\n", t->name );
	    break;

	case T_FATE_TOUCHED:
	case T_FATE_MISSING:
	case T_FATE_NEEDTMP:
	case T_FATE_OUTDATED:
	case T_FATE_UPDATE:
	    /* Set "on target" vars, build actions, unset vars */
	    /* Set "progress" so that make1c() counts this target among */
	    /* the successes/failures. */

	    if( t->actions )
	    {
		++counts->total;

		if( DEBUG_MAKE && ( !( counts->total % 1000 ) && ( globs.noexec || perf_timer_usec_since( last_report_reft ) > 50 * 1000 ) ) )
		{
		    int tgt_left = counts->totalToUpdate - counts->made;
		    if(  globs.noexec )
		      printf( "...on %dth target...\n", counts->total );
		    else if( !counts->perfDoneTargets )
		      printf( "...on %dth target (%d targets pending)...\n", counts->total, counts->totalToUpdate - counts->total );
		    else if (tgt_left)
		    {
		      char tbuf[16];
		      int threads = globs.jobs <= 1 ? 1 : (tgt_left > globs.jobs ? globs.jobs : tgt_left);
		      printf( "%s...on %dth target (%d targets pending), %d%% done,  ETA: %s to build %d targets...%s\n",
		        tty_green_text_string(), counts->total, counts->totalToUpdate - counts->total,
		        (counts->totalToUpdate - tgt_left) * 100 / counts->totalToUpdate,
		        seconds2str( tbuf, sizeof( tbuf ), (counts->perfDoneTargetsTimeMsec / 1000.0) * tgt_left / counts->perfDoneTargets / threads ),
		        tgt_left, tty_reset_text_color_string() );
		    }
		    last_report_reft = perf_timer_now();
		}

		pushsettings( t->settings );
		t->cmds = (char *)make1cmds( t->actions );
		popsettings( t->settings );

		t->progress = T_MAKE_RUNNING;
	    }

	    break;
	}

	/* Call make1c() to begin the execution of the chain of commands */
	/* needed to build target.  If we're not going to build target */
	/* (because of dependency failures or because no commands need to */
	/* be run) the chain will be empty and make1c() will directly */
	/* signal the completion of target. */

	make1c( t );
}

/*
 * make1c() - launch target's next command, call make1b() when done
 */

static void
make1c( TARGET *t )
{
	CMD	*cmd = (CMD *)t->cmds;
	/* If there are (more) commands to run to build this target */
	/* (and we haven't hit an error running earlier comands) we */
	/* launch the command with execcmd(). */
	
	/* If there are no more commands to run, we collect the status */
	/* from all the actions then report our completion to all the */
	/* parents. */

	if( cmd && t->status == EXEC_CMD_OK )
	{
	    if( DEBUG_MAKE )
		if( DEBUG_MAKEQ || ! ( cmd->rule->flags & RULE_QUIETLY ) )
	    {
		printf( "%s ", cmd->rule->name );
		list_print( lol_get( &cmd->args, 0 ) );
		printf( "\n" );
	    }

	    if( DEBUG_EXEC )
		printf( "%s\n", cmd->buf );

	    if( globs.cmdout )
		fprintf( globs.cmdout, "%s", cmd->buf );

	    if( globs.noexec )
	    {
		make1d( t, EXEC_CMD_OK, 0 );
	    } 
	    else
	    {
		fflush( stdout );
		execcmd( cmd->buf, make1d, t, cmd->shell );
	    }
	}
	else
	{
	    TARGETS	*c;
	    ACTIONS	*actions;

	    /* Collect status from actions, and distribute it as well */

	    for( actions = t->actions; actions; actions = actions->next )
		if( actions->action->status > t->status )
		    t->status = actions->action->status;

	    for( actions = t->actions; actions; actions = actions->next )
		if( t->status > actions->action->status )
		    actions->action->status = t->status;

	    /* Tally success/failure for those we tried to update. */

	    if( t->progress == T_MAKE_RUNNING )
		switch( t->status )
	    {
	    case EXEC_CMD_OK:
		++counts->made;
		if( t->time_spent_msec > 0 )
		{
			counts->perfDoneTargets ++;
			counts->perfDoneTargetsTimeMsec += t->time_spent_msec;
			int tgt_left = counts->totalToUpdate - counts->made;
			if( DEBUG_MAKE && tgt_left > 0 && counts->perfDoneTargets > 10 && last_report_done_targets < counts->perfDoneTargets &&
			    perf_timer_usec_since( last_report_reft ) > 10 * 1000 * 1000 )
			{
				char tbuf1[16], tbuf2[16];
				int threads = globs.jobs <= 1 ? 1 : (tgt_left > globs.jobs ? globs.jobs : tgt_left);
				printf( "%s... %d%% done (%d targets),  ETA: %s to build %d targets [%s passed] ...%s\n",
				  tty_green_text_string(), (counts->totalToUpdate - tgt_left) * 100 / counts->totalToUpdate, counts->made,
				  seconds2str(tbuf1, sizeof(tbuf1), (counts->perfDoneTargetsTimeMsec / 1000.0) * tgt_left / counts->perfDoneTargets / threads),
				  tgt_left, seconds2str(tbuf2, sizeof(tbuf2), perf_timer_ticks2sec( perf_timer_now() - build_start_reft ) ),
				  tty_reset_text_color_string() );
				last_report_reft = perf_timer_now();
				last_report_done_targets = counts->perfDoneTargets;
			}
		}
		break;
	    case EXEC_CMD_FAIL:
		++counts->failed;
		break;
	    }

	    /* Tell parents dependent has been built */

	    t->progress = T_MAKE_DONE;

	    for( c = t->parents; c; c = c->next )
		make1b( c->target );
	}
}

/*
 * make1d() - handle command execution completion and call back make1c()
 */

static void
make1d( 
	void	*closure,
	int	status,
	int time_spent_msec )
{
	TARGET	*t = (TARGET *)closure;
	CMD	*cmd = (CMD *)t->cmds;

	/* Execcmd() has completed.  All we need to do is fiddle with the */
	/* status and signal our completion so make1c() can run the next */
	/* command.  On interrupts, we bail heavily. */

	if( status == EXEC_CMD_FAIL && ( cmd->rule->flags & RULE_IGNORE ) )
	    status = EXEC_CMD_OK;

	/* On interrupt, set intr so _everything_ fails */

	if( status == EXEC_CMD_INTR )
	    ++intr;

	if( status == EXEC_CMD_FAIL && DEBUG_MAKE )
	{
	    /* Print command text on failure */

	    if( !DEBUG_EXEC )
		printf( "%s\n", cmd->buf );

	    printf( "...failed %s ", cmd->rule->name );
	    list_print( lol_get( &cmd->args, 0 ) );
	    printf( "...\n" );

	    if( globs.quitquick ) ++intr;
	}

	/* If the command was interrupted or failed and the target */
	/* is not "precious", remove the targets. */
	/* Precious == 'actions updated' -- the target maintains state. */

	if( status != EXEC_CMD_OK && !( cmd->rule->flags & RULE_UPDATED ) )
	{
	    LIST *targets = lol_get( &cmd->args, 0 );

	    for( ; targets; targets = list_next( targets ) )
		if( !unlink( targets->string ) )
		    printf( "...removing %s\n", targets->string );
	}

	/* Free this command and call make1c() to move onto next command. */

	t->time_spent_msec += time_spent_msec;
	t->status = status;
	t->cmds = (char *)cmd_next( cmd );

	cmd_free( cmd );

	make1c( t );
}

/*
 * make1cmds() - turn ACTIONS into CMDs, grouping, splitting, etc
 *
 * Essentially copies a chain of ACTIONs to a chain of CMDs, 
 * grouping RULE_TOGETHER actions, splitting RULE_PIECEMEAL actions,
 * and handling RULE_UPDATED actions.  The result is a chain of
 * CMDs which can be expanded by var_string() and executed with
 * execcmd().
 */

static CMD *
make1cmds( ACTIONS *a0 )
{
	CMD *cmds = 0;
	LIST *shell = var_get( "JAMSHELL" );	/* shell is per-target */

	/* Step through actions */
	/* Actions may be shared with other targets or grouped with */
	/* RULE_TOGETHER, so actions already seen are skipped. */

	for( ; a0; a0 = a0->next )
	{
	    RULE    *rule = a0->action->rule;
	    SETTINGS *boundvars;
	    LIST    *nt, *ns;
	    ACTIONS *a1;
	    int	    start, chunk, length, maxline;

	    /* Only do rules with commands to execute. */
	    /* If this action has already been executed, use saved status */

	    if( !rule->actions || a0->action->running )
		continue;

	    a0->action->running = 1;
	    
	    /* Make LISTS of targets and sources */
	    /* If `execute together` has been specified for this rule, tack */
	    /* on sources from each instance of this rule for this target. */

	    nt = make1list( L0, a0->action->targets, 0 );
	    ns = make1list( L0, a0->action->sources, rule->flags );

	    if( rule->flags & RULE_TOGETHER )
		for( a1 = a0->next; a1; a1 = a1->next )
		    if( a1->action->rule == rule && !a1->action->running )
	    {
		ns = make1list( ns, a1->action->sources, rule->flags );
		a1->action->running = 1;
	    }

	    /* If doing only updated (or existing) sources, but none have */
	    /* been updated (or exist), skip this action. */

	    if( !ns && ( rule->flags & ( RULE_UPDATED | RULE_EXISTING ) ) )
	    {
		list_free( nt );
		continue;
	    }

	    /* If we had 'actions xxx bind vars' we bind the vars now */

	    boundvars = make1settings( rule->bindlist );
	    pushsettings( boundvars );

	    /*
	     * Build command, starting with all source args. 
	     *
	     * If cmd_new returns 0, it's because the resulting command
	     * length is > MAXLINE.  In this case, we'll slowly reduce
	     * the number of source arguments presented until it does
	     * fit.  This only applies to actions that allow PIECEMEAL 
	     * commands.
	     *
	     * While reducing slowly takes a bit of compute time to get
	     * things just right, it's worth it to get as close to MAXLINE
	     * as possible, because launching the commands we're executing 
	     * is likely to be much more compute intensive!
	     *
	     * Note we loop through at least once, for sourceless actions.
	     *
	     * Max line length is the action specific maxline or, if not 
	     * given or bigger than MAXLINE, MAXLINE.
	     */

	    start = 0;
	    chunk = length = list_length( ns );
	    maxline = rule->flags / RULE_MAXLINE;
	    maxline = maxline && maxline < MAXLINE ? maxline : MAXLINE;

	    do
	    {
		/* Build cmd: cmd_new consumes its lists. */

		CMD *cmd = cmd_new( rule, 
			list_copy( L0, nt ), 
			list_sublist( ns, start, chunk ),
			list_copy( L0, shell ),
			maxline );

		if( cmd )
		{
		    /* It fit: chain it up. */

		    if( !cmds ) cmds = cmd;
		    else cmds->tail->next = cmd;
		    cmds->tail = cmd;
		    start += chunk;
		}
		else if( ( rule->flags & RULE_PIECEMEAL ) && chunk > 1 )
		{
		    /* Reduce chunk size slowly. */

		    chunk = chunk * 9 / 10;
		}
		else
		{
		    /* Too long and not splittable. */

		    printf( "%s actions too long (max %d)!\n", 
			rule->name, maxline );
		    exit( EXITBAD );
		}
	    }
	    while( start < length );

	    /* These were always copied when used. */

	    list_free( nt );
	    list_free( ns );

	    /* Free the variables whose values were bound by */
	    /* 'actions xxx bind vars' */

	    popsettings( boundvars );
	    freesettings( boundvars );
	}

	return cmds;
}

/*
 * make1list() - turn a list of targets into a LIST, for $(<) and $(>)
 */

static LIST *
make1list( 
	LIST	*l,
	TARGETS	*targets,
	int	flags )
{
    for( ; targets; targets = targets->next )
    {
	TARGET *t = targets->target;

	/* Sources to 'actions existing' are never in the dependency */
	/* graph (if they were, they'd get built and 'existing' would */
	/* be superfluous, so throttle warning message about independent */
	/* targets. */

	if( t->binding == T_BIND_UNBOUND )
	    make1bind( t, !( flags & RULE_EXISTING ) );

	if( ( flags & RULE_EXISTING ) && t->binding != T_BIND_EXISTS )
	    continue;

	if( ( flags & RULE_UPDATED ) && t->fate <= T_FATE_STABLE )
	    continue;

	/* Prohibit duplicates for RULE_TOGETHER */

	if( flags & RULE_TOGETHER )
	{
	    LIST *m;

	    for( m = l; m; m = m->next )
		if( !strcmp( m->string, t->boundname ) )
		    break;

	    if( m )
		continue;
	}

	/* Build new list */

	l = list_new( l, t->boundname, 1 );
    }

    return l;
}

/*
 * make1settings() - for vars that get bound values, build up replacement lists
 */

static SETTINGS *
make1settings( LIST *vars )
{
	SETTINGS *settings = 0;

	for( ; vars; vars = list_next( vars ) )
	{
	    LIST *l = var_get( vars->string );
	    LIST *nl = 0;

	    for( ; l; l = list_next( l ) ) 
	    {
		TARGET *t = bindtarget( l->string );

		/* Make sure the target is bound, warning if it is not in the */
		/* dependency graph. */

		if( t->binding == T_BIND_UNBOUND )
		    make1bind( t, 1 );

		/* Build new list */

		nl = list_new( nl, t->boundname, 1 );
	    }

	    /* Add to settings chain */

	    settings = addsettings( settings, 0, vars->string, nl );
	}

	return settings;
}

/*
 * make1bind() - bind targets that weren't bound in dependency analysis
 *
 * Spot the kludge!  If a target is not in the dependency tree, it didn't 
 * get bound by make0(), so we have to do it here.  Ugly.
 */

static void
make1bind( 
	TARGET	*t,
	int	warn )
{
	if( t->flags & T_FLAG_NOTFILE )
	    return;

	/* Sources to 'actions existing' are never in the dependency */
	/* graph (if they were, they'd get built and 'existing' would */
	/* be superfluous, so throttle warning message about independent */
	/* targets. */

	if( warn )
	    printf( "warning: using independent target %s\n", t->name );

	pushsettings( t->settings );
	t->boundname = search( t->name, &t->time );
	t->binding = t->time ? T_BIND_EXISTS : T_BIND_MISSING;
	popsettings( t->settings );
}
