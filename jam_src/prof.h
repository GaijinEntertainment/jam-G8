/*
 * prof.h - lightweight phase/function profiling for jam (JAM_PROF builds only)
 *
 * Usage: PROF_ENTER(name) / PROF_LEAVE(name) around code, PROF_COUNT(name)
 * for counters.  Recursion-safe: time only accumulates at outermost entry.
 * prof_dump() prints a report to stderr at exit.
 */

#ifndef JAM_PROF_H
#define JAM_PROF_H

#ifdef JAM_PROF

typedef struct {
	const char	*name;
	double		sec;		/* accumulated seconds (outermost) */
	long long	count;		/* enter count */
	long long	aux;		/* free-form auxiliary counter */
	int		depth;		/* live nesting depth */
	long long	t0;		/* start tick of outermost entry */
} PROFSLOT;

enum {
	PROF_PARSE,
	PROF_MAKE0,
	PROF_SEARCH,
	PROF_HEADERS,
	PROF_HDRSCAN1,
	PROF_HDRRULE,
	PROF_MATCH,
	PROF_REGCOMP,
	PROF_REGEXEC,
	PROF_GLOB,
	PROF_DIRSCAN,
	PROF_FILETIME,
	PROF_COPYSET,
	PROF_VAREXPAND,
	PROF_EVALRULE,
	PROF_NEWSTR,
	PROF_HASH,
	PROF_MAKE1,
	PROF_DR_XFORM,
	PROF_DR_FINISH,
	PROF_MAX
};

extern PROFSLOT prof_slots[PROF_MAX];

long long prof_now( void );
void prof_dump( void );

#define PROF_ENTER( i ) do { PROFSLOT *_p = &prof_slots[i]; _p->count++; \
	if( !_p->depth++ ) _p->t0 = prof_now(); } while( 0 )

#define PROF_LEAVE( i ) do { PROFSLOT *_p = &prof_slots[i]; \
	if( !--_p->depth ) _p->sec += prof_ticks2sec( prof_now() - _p->t0 ); } while( 0 )

#define PROF_COUNT( i ) ( prof_slots[i].count++ )
#define PROF_AUX( i, n ) ( prof_slots[i].aux += (n) )

double prof_ticks2sec( long long t );

#else

#define PROF_ENTER( i ) ((void)0)
#define PROF_LEAVE( i ) ((void)0)
#define PROF_COUNT( i ) ((void)0)
#define PROF_AUX( i, n ) ((void)0)

#endif /* JAM_PROF */

#endif
