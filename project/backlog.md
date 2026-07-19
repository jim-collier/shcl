<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
# Requirements

This is a product backlog just for pre-v1.0.0 release. After that, bugs, features, and enhancements will be managed in Github Issues.

<!-- TOC ignore:true -->
## Table of contents
<!-- TOC -->

- [Conventions](#conventions)
- [Initial requirements](#initial-requirements)
	- [Platform and foundations](#platform-and-foundations)
	- [Build, CI/CD, and install](#build-cicd-and-install)
	- [Configuration and persistence](#configuration-and-persistence)
	- [Performance and security](#performance-and-security)
	- [Other](#other)
- [Backlog](#backlog)
	- [Misc to-do](#misc-to-do)
	- [Bugs](#bugs)
	- [Features and enhancements](#features-and-enhancements)
	- [Done](#done)
		- [Done - Initial requirements](#done---initial-requirements)
		- [Done - Bugs](#done---bugs)
		- [Done - Features and enhancements](#done---features-and-enhancements)
	- [Future and/or deferred](#future-andor-deferred)
	- [Canceled](#canceled)

<!-- /TOC -->

## Conventions

In each section, items are listed approximately from newest to oldest.

| Icon | Status
| :--: | :--
| 🔘   | Not started
| 🛠️   | Started, and/or partially complete
| ✋   | Defer
| ✅   | Complete
| 🚫   | Canceled

## Initial requirements

### Platform and foundations

- 🛠️ Language spec rationalized from `notes.txt`: terminology, model, types, accessor and writer API, formatter, raw blocks. See `spec.md` and `grammar.abnf`.
	- Done: stacked (`*`) block-array form alongside inline commas. Both spellings read the same and canonicalize to inline.
	- Done: date and time formats pinned to a closed whitelist (year-first or named-month only, then calendar-validated).
	- Done: adoption sweep. Cut the currency, percent, float-to-int rounding, and extra boolean tokens from default behavior. Case folding restricted to ASCII. Repeated-leaf hint made mandatory.
	- Done: three strictness levels (loose, standard, strict) as a per-document knob, default standard. Loose re-admits the cut coercions; strict fails the load on any error.
	- Done: bindings re-tiered. Tier 1 reference plus CLI; Tier 2 Go, C, Python; Tier 3 the rest, post-v1.0.
	- Done: raw-block binding reworked. A fence is a value line for its parent field. Child-indent spelling is canonical.
	- Done: inline-array commas made fully forgiving. Stray commas drop their empty slots and never error.
	- 🔘 Next: model diagnostic expectations (count, severity, the mandatory repeated-leaf hint) in the corpus. See conformance item below.

- 🛠️ Ports to the remaining binding languages, in tiers. Tier 2 done; Tier 3 (C#, Java, JavaScript) after v1.0. Each drop-in where possible, corpus-green before shipping. Type via a typed entry point or compile-time generic, never a runtime type field.
	- Done: Go, C (with a C++ veneer), and Python, each an independent parser with the same flags, output, and exit codes as the reference. All corpus-green and held byte-for-byte to the reference on every build.
	- Note: remaining companion surfaces are veneers, not new parsers. Kotlin over the Java core, TypeScript over the JavaScript core.
	- ✅ Shell wrappers around the CLI, not parsers.
		- Done: Bash wrapper (`source/bash/shcl.bash`). Runs as a script or sourced for typed helpers. Forwards the binary's exit code unchanged.
		- Done: PowerShell wrapper (`source/powershell/shcl.ps1`). Runs as a script or dot-sourced for the same typed helpers; forwards the binary's exit code in `$LASTEXITCODE`. Cross-platform binary resolution (also matches `.exe`).

- 🛠️ Expand the conformance corpus (`conformance/`) to cover the hard edges: dates and ambiguity, coercion, quoting and escapes, indentation errors, raw blocks, selectors and wildcards, strictness levels.
	- Done: cases for stacked arrays, coercion bundles, strict-load behavior, forgiving commas, raw blocks, and the full date whitelist.
	- 🔘 Model diagnostic expectations (count, severity, the mandatory repeated-leaf hint). The reference diagnostic shape now exists (line, severity, message), so this can be pinned next.

### Build, CI/CD, and install

- 🛠️ A CI/CD pipeline driven by `cicd/cicd.bash`: builds, tests, and can commit and push. Packaging and publishing are opt-in. See `design.md` for the split-by-responsibility rationale.
	- Done: all stages live. Format check, build, lint, tests plus fuzz smoke, profiler, native and cross builds, versioned artifacts, README demo gif, publish.
	- Done: `--ci` mode is the correctness gate the GitHub workflow runs, so local and CI share one definition of passing.
	- Done: cross-binding differential check. Every binding CLI must agree with the reference byte-for-byte on the corpus and a fuzz-dumped input set.
	- Done: dogfood stage installs the fresh release binary to a fixed local dir. Off under `--ci`, no sudo path.
	- Done: lint stage widened to every binding. ruff and mypy for Python, cppcheck for C, markdownlint for docs, PSScriptAnalyzer for the ps1 wrapper. All gating, locally and in CI; setup steps in `contributing.md`.
	- 🔘 Packaging (.deb, .rpm, NSIS). Wire it when release cuts start.

- 🔘 Dev-environment install script (Linux, macOS, Windows), runnable via a single `curl` or `wget`. Clones, installs dependencies, states what it will do with an option to abort.

- 🔘 Release-install script per platform, runnable via a single `curl` or `wget`. Downloads, installs, and runs the latest release, with an option to abort.

### Configuration and persistence

- 🔘 Default configuration hard-coded.
	- 🔘 Overridden by a per-user config file, created the first time a default is changed.
		- 🔘 Settings live under `~/.config`, resistant to errors (do not bail on the whole file over one bad line).
	- 🔘 Overridden by program options at run time.

### Performance and security

### Other

## Backlog

### Misc to-do

### Bugs

- ✅ Code Review 20260716 item 1: C CLI reads freed memory on typed array output.
	- `get --int|--float|--datetime --array` with more than 8 elements prints from a stale pointer after the line buffer grows; large arrays segfault.
	- Fixed: owned line entries no longer store a pointer into the growable array; corpus case 008 pins 10-element typed arrays of every kind.
	- Detail: `design.md` - Code Review 20260716, item 1.

- ✅ Code Review 20260716 item 2: Rust parser panics on a multibyte char in the timezone tail of a datetime value.
	- A garbled or hostile config aborts the consumer (exit 134) instead of returning BadType.
	- Fixed: zone tail is now checked byte-wise, so no str slice can land mid-char; corpus 007 `bad5` pins BadType across all bindings.
	- Detail: `design.md` - Code Review 20260716, item 2.

- ✅ Code Review 20260716 item 3: wildcard array reads swallow per-slot NotFound/BadType.
	- A missing sub-path yields a silent zero with status Good - the exact trap the fallback design exists to prevent.
	- `count` and `instances` also disagree on the same wildcard path, breaking index alignment.
	- Fixed in all four bindings: array reads carry per-slot statuses, aggregate = worst slot, `instances` keeps unresolved slots as "", CLI grows `--slots` and per-slot `--default` substitution. Spec pinned, corpus case 009 + a slots column in reads.tsv, crosscheck replays `--slots`.
	- Detail: `design.md` - Code Review 20260716, item 3.

- 🔘 Code Review 20260716 item 7: `fmt` ignores `--strictness`.
	- A strict-mode load failure prints silently repaired output and exits 0; `check`/`get` on the same file exit 6.
	- Same in all four CLIs. Detail: `design.md` - Code Review 20260716, item 7.

- 🔘 Code Review 20260716 item 8: mixed `*`/field child lines silently build a block array.
	- Spec and grammar both say a mix at the child indent is not a block array; the parser accepts it with zero diagnostics.
	- Detail: `design.md` - Code Review 20260716, item 8.

- 🔘 Code Review 20260716 item 9: `field[disc]` matches differently at parse-time vs query-time.
	- An array-valued discriminator creates a duplicate instance instead of selecting the existing one.
	- Detail: `design.md` - Code Review 20260716, item 9.

- 🔘 Code Review 20260716 item 10: raw-block merge identity ignores the info-string.
	- Two same-content blocks with different labels merge silently; one label is lost with no diagnostic.
	- Spec never says whether the info-string is part of value identity - decide, then pin. Detail: `design.md` - Code Review 20260716, item 10.

- 🔘 Code Review 20260716 item 11: reading an array as a string drops quoting and escapes.
	- `tags: "a, b", c` reads back as `a, b, c` - not round-trippable, and escape handling differs from the scalar path.
	- Detail: `design.md` - Code Review 20260716, item 11.

- 🔘 Code Review 20260716 item 12: parse time is quadratic in siblings.
	- 100k flat lines take minutes in the reference; Python is far worse. A big valid config effectively hangs the load.
	- Detail: `design.md` - Code Review 20260716, item 12.

- 🔘 Code Review 20260716 item 13: Python formatter recurses and crashes on deep nesting.
	- The reference handles depth the Python CLI dies on (RecursionError, empty stdout, exit 1) - a parity break.
	- Library callers at the default recursion limit hit it around depth 1000. Detail: `design.md` - Code Review 20260716, item 13.

- 🔘 Code Review 20260716 item 14: PowerShell wrapper exits 0 when the resolved binary will not launch.
	- Resolution accepts any plain file (no executable check); a stale non-executable `shcl` yields empty output and exit 0.
	- Detail: `design.md` - Code Review 20260716, item 14.

- 🔘 Code Review 20260716 item 15: crosscheck cannot see trailing-newline differences.
	- Command substitution strips them before compare, so a binding that drops or doubles the final newline ships green.
	- Detail: `design.md` - Code Review 20260716, item 15.

- 🔘 Code Review 20260716 item 16: crosscheck passes with zero comparisons.
	- An empty fuzz dump or a corpus layout change silently drops most of the 1764 comparisons and the gate still passes.
	- Detail: `design.md` - Code Review 20260716, item 16.

- 🔘 Code Review 20260716 item 17: demo gif generator ignores step exit codes.
	- A renamed flag renders the error text into the gif and the pipeline publishes it onto the committed asset.
	- Detail: `design.md` - Code Review 20260716, item 17.

- 🔘 Code Review 20260716 item 21: `fmt --write -` silently drops `--write` and exits 0.
	- Should be a usage error pointing at piping stdout instead. Same in all four CLIs.

- 🔘 Code Review 20260716 item 23: `field[sel]: value` is grammar-legal but has no spec'd meaning.
	- The value is dropped with an Error diagnostic, so strict loads fail on a line the grammar allows. Align spec, grammar, and code.
	- Detail: `design.md` - Code Review 20260716, item 23.

- 🔘 Code Review 20260716 item 24: invalid-UTF-8 command-line args abort the reference.
	- Rust exits 134 (panic); Go/Python/C all exit 3. The reference is the outlier on its own exit-code contract.
	- Detail: `design.md` - Code Review 20260716, item 24.

- 🔘 Code Review 20260716 item 25: broken stdout pipe gives three different exit codes.
	- `shcl fmt big | head`: Rust 134, Go 141, Python 0. Pick one behavior and pin it.
	- Detail: `design.md` - Code Review 20260716, item 25.

- 🔘 Code Review 20260716 item 26: C CLI has unchecked `realloc` on input/output paths.
	- OOM segfaults instead of taking the clean exit-70 path the arena already has.

- 🔘 Code Review 20260716 item 27: crosscheck skips the last `reads.tsv` row if the file lacks a trailing newline.
	- One-line `|| [[ -n "$query" ]]` guard fixes it.

- 🔘 Code Review 20260716 item 30: NUL-joined merge key conflates distinct values.
	- `x: a, b` and `x: "a<NUL>b"` merge to one instance; the second value is silently lost. Make the key injective.
	- Detail: `design.md` - Code Review 20260716, item 30.

- 🔘 Code Review 20260716 item 32: wrappers invoked via symlink lose the sibling-binary and repo-build fallbacks.
	- Both wrappers compute the script dir without resolving links; resolve the real path first.

- 🔘 Code Review 20260716 item 33: ps1 header's own usage example assigns to read-only `$host`.
	- Copying the documented example fails; rename the example variable.

- 🔘 Code Review 20260716 item 34: ps1 `SHCL_BIN` probe skips the `.exe` fallback its header promises.
	- Route the pin through the same `_shcl_exe` helper the other probes use.

### Features and enhancements

- 🔘 Code Review 20260716 item 4: `fmt` deletes every comment with no warning, and the spec never discloses it.
	- Direct hit on the hand-author audience; retrofitting comment storage later touches all five codebases.
	- Decide before 1.0: preserve comments as trivia, or spec the loss and warn on `fmt --write`. Detail: `design.md` - Code Review 20260716, item 4.

- 🔘 Code Review 20260716 item 5: the Writer half of the spec'd API exists in no binding and has no backlog item.
	- Spec presents Accessor+Writer as the two halves; schema-driven generation depends on it.
	- Decide before 1.0: implement in the reference with write-corpus cases, or descope it from the 1.0 spec. Detail: `design.md` - Code Review 20260716, item 5.

- 🔘 Code Review 20260716 item 6: README lead code examples call APIs that do not exist.
	- `GetIntOr(...)` (Go), `get_int(..., default=)` (Python), `get_or<T>` (C++) are all missing; a new user's first copy-paste fails.
	- Either implement the spec's convenience tier (already 🛠️ above) or rewrite the examples to the shipped API.

- 🔘 Code Review 20260716 item 18: query-side behavior is barely pinned.
	- No corpus rows for wildcards, on-bad modes, or defaults; the fuzz differential compares `fmt` only.
	- The accessor side is where five hand-written ports diverge most easily. Detail: `design.md` - Code Review 20260716, item 18.

- 🔘 Code Review 20260716 item 19: diagnostic wording became a byte-for-byte 5-way contract by accident.
	- `check` prints prose to stdout and crosscheck compares it, so every English message is frozen across bindings - contradicting design.md's per-binding-voice rule.
	- Give diagnostics stable codes; compare codes, free the prose. Detail: `design.md` - Code Review 20260716, item 19.

- 🔘 Code Review 20260716 item 20: README still says no tagged release exists.
	- Contradicts the v1.0.0-beta1 tag, the changelog, and the published prerelease binaries; badge still says Alpha.
	- Add a README status pass to the release-cut checklist.

- 🔘 Code Review 20260716 item 22: `--on-bad=error` messages are bare enum names.
	- `app.name: BadType` - no value, no requested type, no file, no suggested fix. Stderr is not contract, so this is free to improve.

- 🔘 Code Review 20260716 item 28: dogfood install is a non-atomic in-place `cp`.
	- A launch during the copy sees a torn binary, and the synced dest dir can propagate it. Copy to a temp name, then `mv` over.

- 🔘 Code Review 20260716 item 29: selector index parses as `usize` in the reference but `u64` in Go.
	- Latent divergence on any future 32-bit build. Pin the reference to `u64`.

- 🔘 Code Review 20260716 item 31: spec prose contradicts grammar and code on bare non-ASCII field names.
	- Prose says only reserved chars need quotes (and uses `Strasse` with a sharp s as an example); grammar and parser reject them.
	- Also: hex `-0x8000000000000000` (i64 min) reads BadType while the decimal spelling works. Detail: `design.md` - Code Review 20260716, item 31.

- 🔘 Code Review 20260716 item 35: value-taking options reject the space-separated form with a misleading error.
	- `--default 99` reports "unknown option: --default". Accept the space form or explain the `=` requirement.

- 🔘 Code Review 20260716 item 36: `check` reports "ok" and exits 0 even when diagnostics include Errors.
	- A CI gate on `check` passes configs whose lines were dropped. Nonzero exit or clearer wording; note it is corpus-pinned, so change everywhere at once.

- 🔘 Code Review 20260716 item 37: `--version`/`-h` are undiscoverable.
	- Help text never mentions them; `shcl help` and `shcl version` are rejected; `-w` is accepted but undocumented.

- 🔘 Code Review 20260716 item 38: wrapper documentation drift.
	- README omits the PowerShell wrapper; spec says "POSIX sh" but the shell wrapper is deliberately Bash. Align the words with the artifacts.

- 🛠️ Accessor: two-tier junior-friendly surface (convenience default plus full status), consistent across all bindings. A supplied default implies default mode.

- 🔘 Schema-as-SHCL validation. The schema is a plain SHCL file (type, required, allowed values). `Validate(doc, schemaDoc)` returns structured diagnostics and catches unknown or misspelled fields. See `design.md` "Power layer".
	- Note: needs the reference parser first, then spec the schema vocabulary alongside it.

- 🔘 Layered loading. `Load(defaults, site, user, ...)` merges later over earlier via the existing merge rule, with CLI and env overrides on top.

- 🔘 Schema-driven generation. Writer plus schema emits a commented, typed starter config (`shcl init --schema ...`). Depends on schema validation.

### Done

#### Done - Initial requirements

- ✅ Initialize the git repo at `github/` and wire the remote. `main` plus a feature-branch flow.

- ✅ Resolve the open minor items at the end of `spec.md` (currency set, wildcard-missing behavior, `onBad` surface, percent-to-int, fence info-string). All settled inline under "Resolved minor items".

- ✅ Rust reference parser (Tier 1) implementing the full spec, driven by the corpus. The `shcl` CLI builds from it.
	- Done: single-file zero-dependency library plus the CLI (`get`, `fmt`, `check`, `count`, `instances`). Corpus-green, with fuzz smoke in the test run.
	- Note: fuzzing surfaced two formatter rules, now in `spec.md`.

#### Done - Bugs

#### Done - Features and enhancements

- ✅ README rewrite: problem-first pitch, file and read-call examples, a format comparison table, a "wrong choice" section, and honest alpha status.

### Future and/or deferred

### Canceled
