# strrules builtin equivalence tests

The C builtins in `jam_src/strrules.c` must be bit-exact with the
interpreted rules they replace (from dagor's `prog/_jBuild/jBuild.jam`
and `jCommonRules.jam`), including their historical quirks — otherwise
digests and command lines would change between jam versions.

`t_corpus.jam` exercises the edge cases (last-repetition capture,
trailing separators, escape tail-drop, `../` pop saturation at the
filesystem root, the two-character-segment double-pop, empty prefixes).
The three drivers run it against different implementations:

| driver | rules come from | works on |
|---|---|---|
| `t_rules.jam` | interpreted definitions (verbatim copies) | any jam |
| `t_builtin.jam` | C builtins | new jam only |
| `t_guarded.jam` | `JAM_BUILTINS`-guarded definitions | both |

All runs must produce identical output. `expected.txt` is the filtered
reference captured from an unmodified jam.exe running `t_rules.jam`.

Run (from this directory; use any jam binary to be tested):

```bash
jam -sCORPUS=t_corpus.jam -f t_rules.jam   2>&1 | grep -E '^(====|<|----|CORPUS_DONE)' > out.txt && diff expected.txt out.txt
jam -sCORPUS=t_corpus.jam -f t_builtin.jam 2>&1 | grep -E '^(====|<|----|CORPUS_DONE)' > out.txt && diff expected.txt out.txt
jam -sCORPUS=t_corpus.jam -f t_guarded.jam 2>&1 | grep -E '^(====|<|----|CORPUS_DONE)' > out.txt && diff expected.txt out.txt
```

(The jam run exits non-zero with "don't know how to make all" — only
the Echo output matters.)

`t_override.jam` (golden `expected_override.txt`, filter `^(O[0-9]|OVERRIDE_DONE)`)
pins the override story: jamfile definitions of `SimplifyComposedPath`
or `MakePathAbsolute` keep winning on a new jam because the builtins
re-dispatch by name whenever the named rule has a procedure.  The
corpus also probes the replicated FOR-variable leak (`s` holds the
last processed element after the string rules, as with the
interpreted bodies).

`t_rules.jam` on a NEW jam additionally proves that jamfile rule
definitions override the builtins, i.e. unmodified older build scripts
keep their exact behavior on a newer jam.exe.
