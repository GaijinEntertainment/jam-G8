/*
 * Copyright 2025 Gaijin Games KFT.
 *
 * This file is part of jam-G8 - see jam.c for Copyright information.
 */

/*
 * changedPaths.h - early-exit decision based on a list of git-changed paths
 *
 * When the jam variable JamChangedFilesPath is set to a path on the command
 * line (e.g. "jam -sJamChangedFilesPath=changed.txt target"), the file is
 * loaded as the authoritative list of source paths the caller considers
 * "changed in this commit".  After make0() has built the dependency graph,
 * the requested top-level targets are walked.  If no transitive dependency
 * is in the loaded set, jam exits with code 0 without running make1().
 * If at least one dep matches, make1() runs as today.
 *
 * mtime is NOT consulted in this decision.
 *
 * Public API:
 *   changed_paths_load   load the list file; returns 1 on success, 0 on
 *                        unrecoverable error (caller exits with EXITBAD).
 *   changed_paths_test   walk t's transitive deps; return 1 if any bound
 *                        path is in the set, 0 otherwise.  Each call uses
 *                        a fresh visited generation, so calls are
 *                        independent.
 *   changed_paths_free   release loaded set.
 */

#ifndef CHANGED_PATHS_H
#define CHANGED_PATHS_H

struct _target;

int  changed_paths_load( const char *list_file );
int  changed_paths_test( struct _target *t );
void changed_paths_free( void );

#endif /* CHANGED_PATHS_H */
