/*
 * prof.c - lightweight profiling for jam (JAM_PROF builds only)
 */

#ifdef JAM_PROF

# include <stdio.h>
# include <windows.h>
# include "prof.h"

PROFSLOT prof_slots[PROF_MAX] = {
	{ "parse" },
	{ "make0" },
	{ "search/bind" },
	{ "headers" },
	{ "hdr scan (read+regex)" },
	{ "hdr rule (interp)" },
	{ "MATCH builtin" },
	{ "regcomp" },
	{ "regexec" },
	{ "GLOB builtin" },
	{ "file_dirscan" },
	{ "file_time" },
	{ "copysettings" },
	{ "var_expand" },
	{ "evaluate_rule" },
	{ "newstr" },
	{ "hashitem" },
	{ "make1" },
	{ "  deprule xform" },
	{ "  deprule finish" },
};

static double prof_freq;

long long prof_now( void )
{
	LARGE_INTEGER li;
	QueryPerformanceCounter( &li );
	return li.QuadPart;
}

double prof_ticks2sec( long long t )
{
	if( !prof_freq )
	{
	    LARGE_INTEGER f;
	    QueryPerformanceFrequency( &f );
	    prof_freq = (double)f.QuadPart;
	}
	return (double)t / prof_freq;
}

void prof_dump( void )
{
	int i;
	fprintf( stderr, "\n--- jam profile ---\n" );
	fprintf( stderr, "%-24s %12s %14s %14s\n", "slot", "sec", "count", "aux" );
	for( i = 0; i < PROF_MAX; i++ )
	{
	    PROFSLOT *p = &prof_slots[i];
	    if( !p->count && !p->aux )
		continue;
	    fprintf( stderr, "%-24s %12.3f %14lld %14lld\n",
		p->name, p->sec, p->count, p->aux );
	}
}

#else

/* keep the translation unit non-empty for pedantic compilers */
typedef int jam_prof_unused_t;

#endif
