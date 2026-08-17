/*
 * statecache.h - persistent parse-state cache
 *
 * Jam re-reads and re-interprets every jamfile on every invocation --
 * on dagor-sized trees that is seconds of pure re-derivation of a
 * rule/target graph that almost never changes.  Jam serializes the
 * complete post-parse state (targets, dependency edges, actions,
 * settings, rules, variables) and on later runs restores it instead
 * of parsing, provided nothing that influenced parsing has changed:
 *
 *   - the jam binary version and format version,
 *   - the full command line,
 *   - the environment block (hashed),
 *   - every jamfile read (mtime+size) and every include attempt that
 *     was skipped because the NOCARE'd file was missing (still absent),
 *   - the name listing of every directory read by GLOB.
 *
 * Anything unexpected (unknown format, stat mismatch, rule referenced
 * by HDRRULE whose body is not a C builtin, ...) falls back to a full
 * parse; a fresh state is saved after a successful parse.
 *
 * Volatile variables (JAMDATE, JAMUNAME) are not restored from the
 * cache; they keep the values computed at startup.
 *
 * A cache hit replays the parse's ECHO output verbatim; parse-phase
 * -d debug tracing and warnings (including a missing include's
 * perror) are produced only by a real parse.
 *
 * Known limitations, all pathological: values DERIVED from volatile
 * variables at parse time (STAMP = $(JAMDATE)) are frozen at the
 * cached parse; a NOTFILE'd target that is also `include`d keeps a
 * zero timestamp on warm runs; duplicated names inside the process
 * environment block may alias between getenv and the startup import.
 *
 * Where the cache lives - the variable JamStateCachePath (via -s or
 * the environment; it must be known before parsing, so a jamfile
 * cannot set it):
 *
 *   unset          default slot: a per-(build id, cwd, command line)
 *                  file under %LOCALAPPDATA%\jam, $XDG_CACHE_HOME/jam
 *                  or ~/.cache/jam.  Slots untouched for 14 days are
 *                  pruned after a save.  If no cache directory can be
 *                  derived, the feature quietly stays off.
 *   a path         that file, exactly; the directory is the user's
 *                  and is never pruned.  Refusal diagnostics print
 *                  only for an explicit path (or JAM_SC_DEBUG=1) -
 *                  whoever names a path gets told why it is not used.
 *   *              autonamed .#<JAMFILE>~parsed file next to JAMFILE used.
 *   empty / none   fully disabled, zero change.  `none` exists
 *                  because Windows shells cannot express an empty
 *                  environment variable (`set X=` deletes it).
 */

#ifndef JAM_STATECACHE_H
#define JAM_STATECACHE_H

/* NOTE: include after lists.h (jam headers have no guards) */

/* remember the full original command line and the jam build id
 * (call once from main) */
void statecache_note_argv( int argc, char **argv, const char *jamver );

/* record an include-file attempt (boundname; existed=0 if skipped) */
void statecache_note_include( const char *boundname, int existed );

/* bracket compile_include's search() so unresolved SEARCH candidates
 * are recorded as must-stay-missing manifest entries */
void statecache_include_search_begin( void );
void statecache_include_search_end( void );
void statecache_note_search_miss( const char *path );

/* record a var_get() miss: the parse read a variable that was not set,
 * so the cache must be invalidated if that name appears in the
 * environment later (called by var_get) */
void statecache_note_varmiss( const char *symbol );

/* record parse-time ECHO output, replayed verbatim on a cache hit */
void statecache_note_echo( const char *text );

/* snapshot the set of rules that exist right after load_builtins();
 * anything defined later is a jamfile rule */
void statecache_builtin_barrier( void );

/* try to restore state; returns 1 on cache hit (skip parsing) */
int statecache_try_load( const char *jamfile_name );

/* serialize state after a successful parse (no-op on miss/disabled) */
void statecache_save( void );

#endif
