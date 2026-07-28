/*
 * depcache.h - persistent header-scan (HDRSCAN) result cache
 *
 * Jam re-reads every dependency (.d) file and regex-scans it on every
 * invocation, even when the file is unchanged -- for large trees this
 * is seconds of pure re-parsing.  This cache stores the extracted
 * include list per scanned file, keyed by the file's bound name and
 * validated by its full-resolution mtime + size (stat'ed by
 * depcache_get before the caller reads the file) plus a hash of the
 * active HDRSCAN patterns.  On a hit the file is not opened at all;
 * the cached list is fed to HDRRULE exactly as a fresh scan result
 * would be, so rule semantics are unchanged.
 *
 * Path selection (opt-out design):
 *   - JamDepCachePath set non-empty: use that file.
 *   - JamDepCachePath set EMPTY:     cache disabled.
 *   - unset: a per-invocation default under JamCacheDir (default
 *     %LOCALAPPDATA%\jam-g8 / $XDG_CACHE_HOME/jam-g8), named by a
 *     hash of the cwd and the -f/-s options, so different trees and
 *     configurations never share a file while target subsets do.
 *     Env JAM_NO_CACHE=1 suppresses the default (explicit paths
 *     still win).
 */

#ifndef JAM_DEPCACHE_H
#define JAM_DEPCACHE_H

/* NOTE: include after lists.h and rules.h (jam headers have no guards) */

/* remember the command line (call once from main); only -f/-s
 * options participate in the default cache file name */
void depcache_note_args( int argc, char **argv );

/* On a hit stores a fresh LIST in *deps (caller owns; may be 0 for
 * "no includes") and returns 1.  Returns 0 on miss or disabled.
 * Also stats the file so a following depcache_put() records the
 * pre-read signature -- call put only right after a missing get. */
int depcache_get( TARGET *t, LIST *hdrscan, int hdrfs, LIST **deps );

/* Record a COMPLETE fresh scan result (deps may be 0 for "no
 * includes"); callers must not record failed or short scans. */
void depcache_put( TARGET *t, LIST *hdrscan, int hdrfs, LIST *deps );

/* Save the cache if anything changed.  Call once before exit. */
void depcache_done( void );

#endif
