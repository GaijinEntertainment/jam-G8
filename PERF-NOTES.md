# jam-G8 performance work (2026-07)

Why does jam take ~7-10 s on an up-to-date tree where ninja takes ~0.5-0.7 s?
Measured on `D:/dagor2/active_matter/prog`, windows vc17, 30 292 targets,
6 459 compiles. All numbers are no-op (fully built tree) wall time.

## Where the time went (instrumented profile, before changes)

| phase | sec | detail |
|---|---|---|
| jamfile parse/interpretation | 5.19 | 774 k rule invocations, **560 k MATCH calls**, 16.3 M var_expand |
| make0 graph phase | 2.45 | headers() 2.18 (re-reading every `.d` file: 6 484 files, ~1 M lines), bind/stat 0.22 |
| MATCH builtin | 2.65 | regcomp **per call** + Spencer regexec; mostly from jBuild digest rules |
| regexp engine total | 2.90 | 567 k regcomp + 1.42 M regexec |

The 560 k parse-phase MATCH calls come from `jBuild.jam` digest rules
(`ProcessTargetDigest` → `SplitStringsOnSpace`,
`ReplaceRootPrefixForKnownOptionAndStripHarmless`,
`AddEscapesToMakeJamValidForWrite`) which push every compiler-option token
of every target through regex split loops, plus `SimplifyComposedPath`.
The graph phase re-reads and re-regexes every compiler-generated `.d`
file on every run — jam has no equivalent of ninja's `.ninja_deps`.

## Changes (jam_src)

1. **fastre.c / fastre.h** — pattern cache + fast-path matchers.
   `re_match(pattern, string, &matched)` replaces
   `regcomp`+`regexec`+`free` in `builtin_match()` and `headers1[fs]()`.
   Compiled patterns are cached forever (regcomp: 567 k → 24 calls).
   Four structural pattern shapes are recognized from the pattern text
   and executed by linear-time matchers instead of the Spencer engine
   (which is O(n²) for the failing unanchored split loops):
   - `([^X])+Y+(.*)` — token split
   - `([^X]*)([X])(.*)` — escape-char split
   - `^(a|b|...|)LIT(LIT2.*)` — prefix alternation (incl. `^../(.*)` etc.)
   - `^(.|[..])* LIT skip* ( [..]* ) .*$` — the msvc and clang `.d` dep-line scans
   Anything not strictly matching a shape falls back to Spencer, so
   semantics are unchanged (including Spencer quirks: `\` literal inside
   `[...]`, `(x)+` capturing the last repetition, branch order, leftmost
   match). `JAM_FASTRE=0` env disables fast paths; `JAM_FASTRE_CHECK=1`
   runs both engines on every call and aborts on any disagreement — ran
   clean over the full active_matter parse+graph (≈1.4 M matches).

2. **depcache.c / depcache.h** — persistent HDRSCAN result cache
   (jam's analog of `.ninja_deps`). Keyed by bound name, validated by
   the scanned file's full-resolution mtime (raw FILETIME on NT —
   whole seconds cannot tell same-second rewrites apart) + size,
   stat'ed *before* the scan reads the file, plus a hash of the
   active HDRSCAN patterns (+HDRFS flag). On a hit the `.d` file is
   not opened at all; the cached include list is fed to HDRRULE
   exactly like a fresh scan, so DepRule semantics are untouched.
   Only complete scans are recorded (a failed or short read feeds
   HDRRULE as before but is never cached). **Opt-in**: enabled only
   when `JamDepCachePath` is set (e.g. `jam -sJamDepCachePath=.jamdeps`,
   or set it in jBuild defaults per output dir). String-table binary
   format (~4.8 MB for active_matter, one order smaller than naive),
   protected by a payload checksum; entries unused for 32 saves are
   dropped. Written only when scans happened, to a per-process temp
   file published by atomic replace (MoveFileEx / rename).
   Use ONE cache file per build tree and configuration: entries are
   keyed by (often relative) bound names, so sharing a path across
   trees or configs thrashes the cache (never wrong results -- the
   signature check catches mismatches -- just zero hit rate).

3. **headers.c** — `headers1()` reads the file in one `fread` and
   iterates lines in memory (exact `fgets` replication: 1023-char
   chunking everywhere; on NT also the text-mode translations
   `\r\n`→`\n`, lone `\r` kept, stop at `^Z`) instead of ~1 M
   `fgets` calls.

4. **builtins.c** — `GLOB` directory listings memoized per run
   (AutoscanBuildLists globs each source dir up to 3×; the FS does not
   change during parsing — actions only run in make1).

5. **filent.c** — `file_dirscan()` uses
   `FindFirstFileExA(FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH)`
   instead of `_findfirst` (skips 8.3-name generation, batches pages).
   FILETIME→time_t conversion matches the UCRT (UTC).

6. **hash.c/h** — new `hashiterate()` (used by depcache save).
   **regexp.h** — include guard. **prof.c/h** — optional `-DJAM_PROF`
   instrumentation build (per-phase timers/counters).

## Results (active_matter/prog, no-op, median of 3)

| configuration | wall | graph phase |
|---|---|---|
| production jam.exe | **7.19 s** | 2.2 s |
| new jam (drop-in, no cache var) | **3.92 s** (−46 %) | ~1.5 s |
| new jam + `JamDepCachePath` | **3.18 s** (−56 %) | 0.8 s |
| ninja on gen_ninja.py output | ~0.5 s | |

Incremental build (touch main.cpp → compile+link): works, graph 0.8 s.

## Validation

- `JAM_FASTRE_CHECK=1` full run: 0 mismatches across ~1.4 M matches.
- `jam -n -a` full command dump (141 691 lines) byte-identical to
  production jam.exe, with and without the dep cache.
- Touched force-included header (`dag_memBase.h`): production and new
  jam (cache warm and cold) all report the same 5 019 targets to update.
- Appended a dep line to a `.d` file: cache detects mtime change,
  rescans, dump identical to a fresh-scan run; new dep takes effect.

## What's left (the remaining ~3 s) — jBuild-side, not jam.exe

~2.6 s is jamfile interpretation, dominated by the digest machinery
(`ProcessTargetDigest` runs its triple regex/list pass over every
option token of all 604 lib targets on every invocation, even when
nothing changed) plus generic rule-invocation overhead (774 k
`evaluate_rule`). Options: skip digest work when the stored digest
file is up-to-date, or move `SplitStringsOnSpace` /
`AddEscapesToMakeJamValidForWrite` / `MakePathListAbsolute` into C
builtins (each is a trivial string op done 100 k+ times via regex).
That could bring jam to ~1.5 s; matching ninja's 0.5 s would need
avoiding re-interpretation entirely (e.g. the gen_ninja.py flow).

## Build

`build.vc.cmd` / `build.sh` / `CMakeLists.txt` / `jam_src/jamfile`
updated with the new sources. Profiling build:
`cl ... -DJAM_PROF ... prof.c` (see `_build` helpers).
