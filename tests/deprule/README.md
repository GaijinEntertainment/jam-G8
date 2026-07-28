# DepRule builtin equivalence test

`DepRulePlain` / `DepRuleDotDot` are C versions of the HDRRULE bodies in
dagor's `prog/_jBuild/windows/{msvc,vc1x,clang}-cpp.jam`. They must
produce exactly the same dependency edges as the interpreted rules,
quirks included — in particular `case \\*` in those jamfiles does NOT
match backslash-rooted paths (the scanner turns the token into the glob
pattern `\*`, which matches only the literal string `*`), so such paths
fall through to the `$(Root)/` branch.

`t_deprule.jam` feeds `fake.d` (drive-absolute, slash-rooted,
backslash-rooted, `./`-relative with `location_prefix`, over-long `../`
runs, empty and degenerate entries) through either implementation and
dumps the resulting graph edges.

Run from this directory with the jam binary under test:

```bash
jam -sIMPL=interp -f t_deprule.jam -n -dd 2>&1 | grep -E '^(Includes|Depends)' | sort > out.txt && diff expected.txt out.txt
```

Then the same with `-sIMPL=builtin`; both must match `expected.txt`.
The rig covers on-target `Root` / `location_prefix` (both must be read
inside `on $(dep)` scope).  `t_depruleplain.jam` is the same rig for
`DepRulePlain` (msvc-cpp.jam DepRule), golden `expected_plain.txt`.
`-sIMPL=interp` also proves that a jamfile-defined rule of that name
still overrides the builtin, i.e. older build scripts keep their exact
behavior on a newer jam.exe.

`t_incoverride.jam` covers the other override the builtins must honor:
`Includes` itself. The builtins inline that step, so both ways of
overriding the rule have to defeat the inlining — `rule Includes` and
`actions Includes`, the latter leaving the procedure untouched while
`evaluate_rule()` attaches its ACTION regardless. Goldens
`expected_inc_{plain,proc,act}.txt`, invocations in the file's header.
