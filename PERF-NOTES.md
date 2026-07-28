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

7. **strrules.c** — C builtins for jBuild's hottest string/dep rules
   (SplitStringsOnSpace, AddEscapesToMakeJamValidForWrite,
   MakePath[List]Absolute, DepRulePlain/DepRuleDotDot), bit-exact with
   the interpreted originals, quirks included.  Feature-detected via
   `JAM_BUILTINS`; a jamfile definition still overrides the builtin.
   See tests/strrules and tests/deprule.

8. **statecache.c/h** — persistent parse-state cache: with
   `JamStateCachePath` set, the whole post-parse state (variables,
   rules, actions, targets, edges, settings) is serialized and
   restored instead of re-reading the jamfiles, guarded by a manifest
   (jam build id, command line, cwd, every jamfile mtime+size, every
   NOCARE'd missing include, GLOB directory listings, and the env
   value of every variable the parse read — including reads of unset
   names).  Any anomaly falls back to a full parse.

9. **graph pooling + interned binding** — with parsing cached,
   building the graph itself dominated: ~2M edges per run, each a
   `bindtarget()` hash over a 60+ character path plus a malloc.
   `TARGETS` and `LIST` nodes are now bump-allocated from blocks
   (neither is ever freed individually), and `bindtarget_interned()`
   memoizes on the *address* of the newstr-interned name.
   1.34 -> 0.96 s; make0 0.68 -> 0.36 s.

## Results (active_matter/prog, no-op, median of 3)

All binaries interleaved on the same machine state:

| configuration | wall |
|---|---|
| stock jam.exe | **7.31 s** |
| main branch (drop-in, no variables set) | **3.97 s** |
| + `JamDepCachePath` | 3.23 s |
| + string/DepRule builtins (`string-builtins`) | 2.65 s |
| + `JamStateCachePath` (`parse-state-cache`) | **1.34 s** |
| ninja on gen_ninja.py output | 0.49 s |

i.e. 5.5x faster than stock, and 2.7x ninja rather than 15x.
Parse phase 5.19 -> 0.30 s; evaluate_rule 774k -> 19k calls;
regexec 1.42M -> 10k.  Real incremental builds (touch main.cpp ->
compile+link) work through both caches; graph phase 0.6 s.

## Validation

- `JAM_FASTRE_CHECK=1` full run: 0 mismatches across ~1.4M matches.
- `jam -n -a` command dump (141708 lines) and `jam -n -dd` dependency
  edge dump (889561 lines) byte-identical to stock jam.exe at every
  commit, with every combination of caches cold/warm.
- Touched force-included header (`dag_memBase.h`): stock and new jam
  report the same 5019 targets to update.
- Appended a dep line to a `.d`: rescanned, new dependency takes
  effect.
- State cache invalidation: editing a jamfile, editing a nested
  include, adding a source file, deleting a source file, changing a
  read environment variable and changing the command line each force
  a reparse and reproduce a fresh parse exactly; unrelated
  environment noise does not invalidate.
- Truncated cache and 200 random corrupted bytes: fall back to a full
  parse, identical output.
- Builtins: 52-case corpus (tests/strrules) plus the DepRule edge rig
  (tests/deprule); old jam.exe, new jam.exe, guarded and unguarded
  jamfiles all agree.

## What's left (the remaining ~0.45 s over ninja)

Profile with everything on (0.96 s wall): state-cache load 0.34 s,
make0 0.36 s (bind/stat 0.17 s, of which file_dirscan 0.18 s across
4534 directories; headers() 0.15 s), make1 0.05 s, the rest process
startup/teardown.

Remaining ideas, in rough value order:

- **state-cache load (0.34 s)** - the format still interns every
  string through `newstr()` and rebuilds lists node by node.  Storing
  the string table as one blob and pointing into it (jam strings are
  never freed) plus laying the restored lists out contiguously would
  remove most of it.
- **directory scanning (0.18 s)** - 4534 `FindFirstFileEx` walks.
  Unavoidable for correctness, but they are independent and could run
  on a small thread pool.
- **teardown** - measured, not worth it: once the nodes are pooled,
  skipping the exit frees changes nothing (0.950 vs 0.954 s), so that
  change was dropped rather than kept on speculation.

## Build

`build.vc.cmd` / `build.sh` / `CMakeLists.txt` / `jam_src/jamfile`
updated with the new sources. Profiling build:
`cl ... -DJAM_PROF ... prof.c` (see `_build` helpers).
