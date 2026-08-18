/*
 * prof.c - lightweight profiling for jam (JAM_PROF builds only)
 */

# include "prof.h"
# include "jam.h"

# if defined(OS_NT)
# include <windows.h>
static long long perf_timer_freq_ticks_per_usec = 0;
static double perf_timer_freq = 0;

static inline void setup_freq()
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency( &f );
	perf_timer_freq_ticks_per_usec = f.QuadPart / 1000000ll;
	if( !perf_timer_freq_ticks_per_usec )
		perf_timer_freq_ticks_per_usec = 1;
	perf_timer_freq = (double)f.QuadPart;
}

long long perf_timer_now( void )
{
	LARGE_INTEGER li;
	QueryPerformanceCounter( &li );
	return li.QuadPart;
}

double perf_timer_ticks2sec( long long t )
{
	if( !perf_timer_freq_ticks_per_usec )
		setup_freq();
	return (double)t / perf_timer_freq;
}

long long perf_timer_ticks2usec( long long t )
{
	if( !perf_timer_freq_ticks_per_usec )
		setup_freq();
	return t / perf_timer_freq_ticks_per_usec;
}

# elif defined(OS_MACOSX)
# include <mach/mach_time.h>

static long long perf_timer_numer = 0;	/* ticks -> ns: * numer / denom */
static long long perf_timer_denom = 1;

static void setup_timebase( void )
{
	mach_timebase_info_data_t tb;

	/* preset, so a failing call leaves ticks == nanoseconds */
	tb.numer = tb.denom = 1;
	mach_timebase_info( &tb );

	perf_timer_numer = tb.numer ? tb.numer : 1;
	perf_timer_denom = tb.denom ? tb.denom : 1;
}

long long perf_timer_now( void ) { return (long long)mach_absolute_time(); }
double perf_timer_ticks2sec( long long t )
{
	if( !perf_timer_numer )
		setup_timebase();
	return (double)( t * perf_timer_numer ) / (double)perf_timer_denom / 1e9;
}
long long perf_timer_ticks2usec( long long t )
{
	if( !perf_timer_numer )
		setup_timebase();
	return t * perf_timer_numer / perf_timer_denom / 1000ll;
}

# else
# include <unistd.h>
long long perf_timer_now( void )
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000ll + ts.tv_nsec / 1000ll;
}
double perf_timer_ticks2sec( long long t ) { return (double)t / 1e6; }
long long perf_timer_ticks2usec( long long t ) { return t; }
# endif

#ifdef JAM_PROF

# include <stdio.h>
# include <windows.h>

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
