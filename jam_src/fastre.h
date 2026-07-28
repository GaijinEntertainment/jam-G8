/*
 * fastre.h - cached + fast-path regexp matching for jam
 *
 * re_match( pattern, string ) is a drop-in for
 *     regexp *re = regcomp( pattern ); regexec( re, string );
 * except the compiled form is cached (never freed) and a few
 * pattern shapes that dominate dagor builds are executed by
 * specialized linear-time matchers instead of the Spencer engine.
 *
 * The returned regexp* holds valid startp[]/endp[] capture pointers
 * (same contract as regexec).  The pointer is owned by the cache;
 * do NOT free it.  Contents are valid until the next re_match call
 * with the same pattern.
 *
 * Set env JAM_FASTRE=0 to disable fast paths (still caches compiles).
 * Set env JAM_FASTRE_CHECK=1 to run both engines and abort on any
 * disagreement (validation mode).
 */

#ifndef JAM_FASTRE_H
#define JAM_FASTRE_H

#include "regexp.h"

/* Returns NULL if the pattern failed to compile. */
regexp *re_match( const char *pattern, const char *string, int *matched );

#endif
