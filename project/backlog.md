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

- ✅ Language spec rationalized from `notes.txt`: terminology, model, types, accessor and writer API, formatter, raw blocks. See `spec.md` and `grammar.abnf`.
	- ✅ stacked (`*`) block-array form alongside inline commas. Both spellings read the same and canonicalize to inline.
	- ✅ date and time formats pinned to a closed whitelist (year-first or named-month only, then calendar-validated).
	- ✅ adoption sweep. Cut the currency, percent, float-to-int rounding, and extra boolean tokens from default behavior. Case folding restricted to ASCII. Repeated-leaf hint made mandatory.
	- ✅ three strictness levels (loose, standard, strict) as a per-document knob, default standard. Loose re-admits the cut coercions; strict fails the load on any error.
	- ✅ bindings re-tiered. Tier 1 reference plus CLI; Tier 2 Go, C, Python; Tier 3 the rest, post-v1.0.
	- ✅ raw-block binding reworked. A fence is a value line for its parent field. Child-indent spelling is canonical.
	- ✅ inline-array commas made fully forgiving. Stray commas drop their empty slots and never error.
	- ✅ Model diagnostic expectations (count, severity, the mandatory repeated-leaf hint) in the corpus. See conformance item below.

- 🛠️ Ports to the remaining binding languages, in tiers. Tier 2 done; Tier 3 (C#, Java, JavaScript) after v1.0. Each drop-in where possible, corpus-green before shipping. Type via a typed entry point or compile-time generic, never a runtime type field.
	- ✅ Go, C (with a C++ veneer), and Python, each an independent parser with the same flags, output, and exit codes as the reference. All corpus-green and held byte-for-byte to the reference on every build.
	- Note: remaining companion surfaces are veneers, not new parsers. Kotlin over the Java core, TypeScript over the JavaScript core.
	- ✅ Shell wrappers around the CLI, not parsers.
		- ✅ Bash wrapper (`source/bash/shcl.bash`). Runs as a script or sourced for typed helpers. Forwards the binary's exit code unchanged.
		- ✅ PowerShell wrapper (`source/powershell/shcl.ps1`). Runs as a script or dot-sourced for the same typed helpers; forwards the binary's exit code in `$LASTEXITCODE`. Cross-platform binary resolution (also matches `.exe`).

- ✅ Expand the conformance corpus (`conformance/`) to cover the hard edges: dates and ambiguity, coercion, quoting and escapes, indentation errors, raw blocks, selectors and wildcards, strictness levels.
	- Done: cases for stacked arrays, coercion bundles, strict-load behavior, forgiving commas, raw blocks, and the full date whitelist.
	- ✅ Model diagnostic expectations (count, severity, the mandatory repeated-leaf hint). Every case now carries an `expected-diags.txt` golden (exact `check` stdout at standard: line/severity/code per diagnostic + the summary line), verified natively by all four runners. Pins the `H001` repeated-leaf hint and the zero-diagnostic cases too.

### Build, CI/CD, and install

- 🛠️ A CI/CD pipeline driven by `cicd/cicd.bash`: builds, tests, and can commit and push. Packaging and publishing are opt-in. See `design.md` for the split-by-responsibility rationale.
	- ✅ all stages live. Format check, build, lint, tests plus fuzz smoke, profiler, native and cross builds, versioned artifacts, README demo gif, publish.
	- ✅ `--ci` mode is the correctness gate the GitHub workflow runs, so local and CI share one definition of passing.
	- ✅ cross-binding differential check. Every binding CLI must agree with the reference byte-for-byte on the corpus and a fuzz-dumped input set.
	- ✅ dogfood stage installs the fresh release binary to a fixed local dir. Off under `--ci`, no sudo path.
	- ✅ lint stage widened to every binding. ruff and mypy for Python, cppcheck for C, markdownlint for docs, PSScriptAnalyzer for the ps1 wrapper. All gating, locally and in CI; setup steps in `contributing.md`.
	- ✅ Packaging (.deb, .rpm, NSIS). Wire it when release cuts start.
		- Done: stage 6 builds .deb + .rpm (nfpm) per Linux binary and an NSIS setup per Windows binary into the release artifact dir, covered by the same sha256sums. `--no-package` to skip; off under `--ci` and `--quick`. Packages use distro layout (/usr/bin + /usr/share/shcl); payload matches install.bash.

### Configuration and persistence

- 🚫 Default configuration hard-coded.
	- 🚫 Overridden by a per-user config file, created the first time a default is changed.
		- 🚫 Settings live under `~/.config`, resistant to errors (do not bail on the whole file over one bad line).
	- 🚫 Overridden by program options at run time.
	- Dropped: strictness and on-bad are the consuming program's contract, not the user's - a user-level override would silently weaken guarantees an app makes about its own config handling, and would make the same `shcl` command mean different things on different machines. Nothing else the CLI exposes is presentation-only, so there is nothing left for a config file to hold. Rationale in `design.md`; runtime options and the library's per-document strictness argument stay as they are.

## Backlog

### Misc to-do

### Bugs

### Features and enhancements

- 🔘 Glossary of terms

- ✅ Dev-environment install script (Linux, macOS, Windows), runnable via a single `curl` or `wget`. Clones, installs dependencies, states what it will do with an option to abort.
	- Done: `install-dev.bash` at repo root. Linux + macOS directly; Windows via WSL (the dev pipeline is bash). Clones (or detects an existing clone), installs the no-sudo pieces itself (rustup, ruff/mypy/cppcheck via pipx, markdownlint via npm, PSScriptAnalyzer via pwsh), and prints the exact package-manager hint for what needs root (go, python3, gcc, shellcheck). Shellcheck-gated.

- ✅ Release-install script, `install.bash` and cross-platform `install.ps1`. In [repo] root, usage instructions in README.md.
	- Done: both at repo root, README Installing has the one-liners. Latest release via the GitHub API (`--release dev` = newest incl. pre-releases, default; `stable` = newest full release), binary picked by OS/arch, sha256-verified, `code/` (drop-in files) and `scripts/` (wrappers) pulled from the tag's source tarball. Idempotent (atomic binary swap); states the plan and confirms first.
	- Decisions along the way: `objects/` skipped - nothing statically-linkable is published yet (revisit with packaging). Linux user install lands in `~/.local/share/shcl` with the `~/.local/bin/shcl` symlink (one path can't be both the dir and the symlink). Windows user install copies the exe beside the dir instead of symlinking (symlinks need elevation) and adds to the user PATH. macOS/BSD get a clear "no prebuilt binaries yet" pointer at build-from-source.
	- Behavior:
		- Runnable via a single `curl` or `wget`. Downloads, installs, and runs the latest release, with an option to abort.
		- Idempotent. Updates existing.
	- Args:
		- `--release[=]<dev[elopment]|stable>` Install latest dev rather than
		- `--target[=]<user|system>`
			- System (default):
				- Linux:
					- /opt/shcl/
						- shcl  ## Rust binary
						- code/  ## Drop-in code files.
						- objects/  ## Or a better name for per-language statically-linkable objects)
						- scripts/  ## .bash and cross-platform .ps1 wrappers
					- /usr/local/sbin/shcl  ## Symlink
				- Windows:
					- C:\Program Files\Shcl\
						- shcl.exe  ## Rust binary
						- code/  ## Drop-in code files.
						- objects/  ## Or a better name for per-language statically-linkable objects)
						- scripts/  ## .ps1 wrapper
					- Add C:\Program Files\Shcl to %PATH%.
				- macOS: ?
				- BSD: ?
			- User:
				- Linux:
					- ~/.local/bin/shcl/
						- (Same subdirs under here as system install)
					~/.local/bin/shcl  ## Symlink
				- Windows:
					- %USERPROFILE%\bin\Shcl
						- (Same subdirs under here as system install)
					- %USERPROFILE%\bin\shcl.exe  ## Symlink
					- Add %USERPROFILE%\bin\ to %PATH%.
				- macOS: ?
				- BSD: ?

- 🛠️ Schema-as-SHCL validation. The schema is a plain SHCL file (type, required, allowed values). `Validate(doc, schemaDoc)` returns structured diagnostics and catches unknown or misspelled fields. See `design.md` "Schema validation".
	- Designed, awaiting review before any code. Schema is a flat list of `field: <path>` instances with constraints as children - no grammar change, no reserved words, prototyped against the shipped binary. Regex constraints rejected (no two target languages share an engine, so byte-for-byte parity is impossible). `V###` diagnostic range; `shcl check --schema S FILE` rather than a new subcommand. Two questions left open in `design.md`: drop-in file placement, and whether length bounds are worth the byte-vs-code-point question.
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

- ✅ Code Review 20260716 item 7: `fmt` ignores `--strictness`.
	- Fixed: `fmt` loads at the requested strictness in all four CLIs; strict failure exits 6 with diagnostics, no output. Crosscheck now replays `fmt` at each `load` row's level.
	- Same in all four CLIs. Detail: `design.md` - Code Review 20260716, item 7.

- ✅ Code Review 20260716 item 8: mixed `*`/field child lines silently build a block array.
	- Fixed: uniform-or-nothing enforced in all four parsers - first mixed field diagnoses an Error (field kept), later `*` lines under that parent are Errors and dropped. Corpus case 010.
	- Detail: `design.md` - Code Review 20260716, item 8.

- ✅ Code Review 20260716 item 9: `field[disc]` matches differently at parse-time vs query-time.
	- Fixed: parse-side selectors match the display form like queries do; create only when nothing matches. Corpus case 011; spec's selector bullet updated.
	- Detail: `design.md` - Code Review 20260716, item 9.

- ✅ Code Review 20260716 item 10: raw-block merge identity ignores the info-string.
	- Decided: info-string is part of a block's identity (fence style is not); equal bodies with different infos stay two instances. All four parsers; corpus case 012; spec updated.
	- Detail: `design.md` - Code Review 20260716, item 10.

- ✅ Code Review 20260716 item 11: reading an array as a string drops quoting and escapes.
	- Fixed: array-as-string is the canonical inline form (minimal quoting, escapes intact, re-parses to the same array) in all four bindings. Corpus rows in case 011; spec's Strings section updated.
	- Detail: `design.md` - Code Review 20260716, item 11.

- ✅ Code Review 20260716 item 12: parse time is quadratic in siblings.
	- Fixed: per-parent (name, value-key) lookup map in all four parsers; sibling scan and hint grouping are linear now. 100k flat lines: reference went from minutes to 0.2s, Python to 1.5s.
	- Detail: `design.md` - Code Review 20260716, item 12.

- ✅ Code Review 20260716 item 13: Python formatter recurses and crashes on deep nesting.
	- Fixed: emit walks an explicit stack; the CLI recursion-limit bump is gone. Depth 25000 formats fine from both the CLI and library callers.
	- Detail: `design.md` - Code Review 20260716, item 13.

- ✅ Code Review 20260716 item 14: PowerShell wrapper exits 0 when the resolved binary will not launch.
	- Resolution accepts any plain file (no executable check); a stale non-executable `shcl` yields empty output and exit 0.
	- Fixed: every ps1 resolution site now goes through `_shcl_executable` (Unix requires an execute bit, Windows keeps a bare leaf); the run-path passthrough is `exit ($LASTEXITCODE ?? 1)`.
	- Detail: `design.md` - Code Review 20260716, item 14.

- ✅ Code Review 20260716 item 15: crosscheck cannot see trailing-newline differences.
	- Command substitution strips them before compare, so a binding that drops or doubles the final newline ships green.
	- Fixed: capture helpers append a trailing sentinel so `$()` has nothing to strip; a dropped or doubled final newline is now a divergence.
	- Detail: `design.md` - Code Review 20260716, item 15.

- ✅ Code Review 20260716 item 16: crosscheck passes with zero comparisons.
	- An empty fuzz dump or a corpus layout change silently drops most of the 1764 comparisons and the gate still passes.
	- Fixed: exits 2 when no comparison ran, when `--extra` matches no `*.shcl`, or below an optional `--min N` floor.
	- Detail: `design.md` - Code Review 20260716, item 16.

- ✅ Code Review 20260716 item 17: demo gif generator ignores step exit codes.
	- A renamed flag renders the error text into the gif and the pipeline publishes it onto the committed asset.
	- Fixed: a step whose exit differs from `expect_exit` (default 0) aborts the render, so cicd skips the publish `cp`.
	- Detail: `design.md` - Code Review 20260716, item 17.

- ✅ Code Review 20260716 item 21: `fmt --write -` silently drops `--write` and exits 0.
	- Should be a usage error pointing at piping stdout instead. Same in all four CLIs.
	- Fixed: `--write` with stdin is a usage error (exit 1) in all four CLIs; `--write FILE` still rewrites.

- ✅ Code Review 20260716 item 23: `field[sel]: value` is grammar-legal but has no spec'd meaning.
	- The value is dropped with an Error diagnostic, so strict loads fail on a line the grammar allows. Align spec, grammar, and code.
	- Fixed by spec'ing the code's existing behavior: a value after a last-segment selector is defined as an `error` (instance kept from the discriminator, value dropped; fails Strict). spec.md Selectors + grammar.abnf note updated; corpus case 018.
	- Detail: `design.md` - Code Review 20260716, item 23.

- ✅ Code Review 20260716 item 24: invalid-UTF-8 command-line args abort the reference.
	- Rust exits 134 (panic); Go/Python/C all exit 3. The reference is the outlier on its own exit-code contract.
	- Fixed: all four validate argv as UTF-8 up front and exit 1 (`invalid argument encoding`); the reference uses `args_os` instead of the panicking `args`.
	- Detail: `design.md` - Code Review 20260716, item 24.

- ✅ Code Review 20260716 item 25: broken stdout pipe gives three different exit codes.
	- `shcl fmt big | head`: Rust 134, Go 141, Python 0. Pick one behavior and pin it.
	- Fixed: uniform die-by-SIGPIPE (141). Rust and Python restore the default SIGPIPE disposition; Go and C already died by signal.
	- Detail: `design.md` - Code Review 20260716, item 25.

- ✅ Code Review 20260716 item 26: C CLI has unchecked `realloc` on input/output paths.
	- OOM segfaults instead of taking the clean exit-70 path the arena already has.
	- Fixed: every CLI allocation goes through `xrealloc`, which now exits 70 with the library's message on OOM.

- ✅ Code Review 20260716 item 27: crosscheck skips the last `reads.tsv` row if the file lacks a trailing newline.
	- One-line `|| [[ -n "$query" ]]` guard fixes it.
	- Fixed with exactly that guard on the read loop.

- ✅ Code Review 20260716 item 30: NUL-joined merge key conflates distinct values.
	- `x: a, b` and `x: "a<NUL>b"` merge to one instance; the second value is silently lost. Make the key injective.
	- Fixed in all four parsers: each cell element (and the raw info-string) is length-prefixed, so the key is injective. Corpus case 017 (count = 2) pins it; the crosscheck skips it (bash can't carry a NUL) and the native runners do the pinning.
	- Detail: `design.md` - Code Review 20260716, item 30.

- ✅ Code Review 20260716 item 32: wrappers invoked via symlink lose the sibling-binary and repo-build fallbacks.
	- Both wrappers compute the script dir without resolving links; resolve the real path first.
	- Fixed: bash follows `${BASH_SOURCE[0]}` through symlinks by hand (bare `readlink` loop, POSIX); ps1 resolves `$PSCommandPath` via `ResolveLinkTarget` into `$script:_SHCL_ROOT`.

- ✅ Code Review 20260716 item 33: ps1 header's own usage example assigns to read-only `$host`.
	- Copying the documented example fails; rename the example variable.
	- Fixed: header example now uses `$svrhost`.

- ✅ Code Review 20260716 item 34: ps1 `SHCL_BIN` probe skips the `.exe` fallback its header promises.
	- Route the pin through the same `_shcl_exe` helper the other probes use.
	- Fixed: the `SHCL_BIN` pin resolves through `_shcl_exe`, so a base name matches its `.exe`.

#### Done - Features and enhancements

- ✅ Style guide: write `style-guide.md` (canonical coding + prose style) and point everything at it.
	- Key decision made prominent everywhere: parity over idiom - bindings mirror the reference's structure over per-language idiom, so byte-for-byte sync stays maintainable and fixes port by diff.
	- Done: guide at repo root with per-language sections and each accepted deviation explained; pointers added in README (Docs), contributing.md (Style), design.md (Direction decisions), and all four binding headers.

- ✅ Code Review 20260716 item 4: `fmt` deletes every comment with no warning, and the spec never discloses it.
	- Direct hit on the hand-author audience; retrofitting comment storage later touches all five codebases.
	- Decide before 1.0: preserve comments as trivia, or spec the loss and warn on `fmt --write`. Detail: `design.md` - Code Review 20260716, item 4.
	- Done: comments survive `fmt` in all four parsers - whole-line comments re-emit above the node the next line binds, trailing ones stay on their line, end-of-file comments land at the end. Spec'd under Comments + Canonical formatter; corpus case 013 pins it and the older cases' expected files now keep their comments.

- ✅ Code Review 20260716 item 5: the Writer half of the spec'd API exists in no binding and has no backlog item.
	- Spec presents Accessor+Writer as the two halves; schema-driven generation depends on it.
	- Done: Writer implemented in all four bindings (full CRUD - typed `set_<T>`/arrays/`raw`/`empty`, `_default` only-if-absent forms, `exists`, `set_comment`, `remove`), each setter the exact inverse of its read. New `shcl set` CLI applies a tab-delimited write-ops script from stdin. Corpus cases 014-016 pair `write.ops` with golden `expected-write.shcl` (matched by every binding's runner + a fixpoint check), the cross-binding differential replays `set`, and a 50k reference fuzz pins the string round-trip. Spec Writer bullet + `design.md` item 5 updated.

- ✅ Code Review 20260716 item 6: README lead code examples call APIs that do not exist.
	- `GetIntOr(...)` (Go), `get_int(..., default=)` (Python), `get_or<T>` (C++) are all missing; a new user's first copy-paste fails.
	- Done: implemented the spec's convenience tier (not gutted the examples), so the README calls are now real as written. Go `GetIntOr`/`Get*Or` + array forms; Python `get_*`/`get_*_array` gained a `default=` param (must-exist and raises without one); C `shcl_get_int/float/bool`; C++ `get_or<T>`. Rust's is the native `get_int(path).unwrap_or(def)` and gained matching `get_*_array` so arrays have the same tier. Semantic pinned everywhere: value only on `Good`, else the call-site fallback (Empty falls back too). Reference unit test + Go test + C++ veneer CHECKs added.

- ✅ Code Review 20260716 item 18: query-side behavior is barely pinned.
	- No corpus rows for wildcards, on-bad modes, or defaults; the fuzz differential compares `fmt` only.
	- The accessor side is where five hand-written ports diverge most easily. Detail: `design.md` - Code Review 20260716, item 18.
	- Done: added a `--rawinfo` CLI type (+ the `rawinfo` reads.tsv type in all four runners) so the info-string read is pinnable; the reference `Document::paths()` drives a fuzz-dump-derived `<name>.reads.tsv` that the crosscheck `--extra` replays (reads over fuzz soup, not just fmt); every scalar read row is also replayed under `--on-bad=error` and `--default=<x>`; corpus case 020-accessor-surface pins wildcards (with a missing slot), a `[value]` selector, and both raw reads. Crosscheck ~1983 -> ~4203 comparisons.

- ✅ Code Review 20260716 item 19: diagnostic wording became a byte-for-byte 5-way contract by accident.
	- `check` prints prose to stdout and crosscheck compares it, so every English message is frozen across bindings - contradicting design.md's per-binding-voice rule.
	- Give diagnostics stable codes; compare codes, free the prose. Detail: `design.md` - Code Review 20260716, item 19.
	- Done: `Diagnostic` carries a stable `code` (`E001..`/`H001..`) in all four bindings; `check` prints `line N: severity: CODE` to stdout and the prose to stderr, so the differential check compares codes (not wording). C exposes `shcl_diag_code`. Includes item 36 (below).

- ✅ Code Review 20260716 item 20: README still says no tagged release exists.
	- Contradicts the v1.0.0-beta1 tag, the changelog, and the published prerelease binaries; badge still says Alpha.
	- Done: lifecycle badge Alpha -> Beta, Status/Installing sections now reflect the `v1.0.0-beta1` pre-release and its prebuilt binaries; release-cut checklist in `design.md` gained a "README status pass" step so it can't drift again.

- ✅ Code Review 20260716 item 22: `--on-bad=error` messages are bare enum names.
	- `app.name: BadType` - no value, no requested type, no file, no suggested fix. Stderr is not contract, so this is free to improve.
	- Done in all four CLIs: `shcl: cannot read <path> as <type>: <reason> (in <file>)`, where a BadType names the offending raw value (`value "$1200" is not a valid int`), and NotFound/Empty/Multiple get a plain-English reason. Array reads say `<type> array`. Stderr, so not crosscheck-pinned.

- ✅ Code Review 20260716 item 28: dogfood install is a non-atomic in-place `cp`.
	- A launch during the copy sees a torn binary, and the synced dest dir can propagate it. Copy to a temp name, then `mv` over.
	- Done: the dogfood stage now copies the binary and each wrapper to a hidden temp name in the same dest dir, then `mv -f` (atomic rename) over the target, so a hand-launched copy only ever sees the complete old or new file; a failed copy is cleaned up and warned, not left torn.

- ✅ Code Review 20260716 item 29: selector index parses as `usize` in the reference but `u64` in Go.
	- Latent divergence on any future 32-bit build. Pin the reference to `u64`.
	- Done: reference `Selector::ByIndex` is now `u64` (cast to `usize` only at the Vec index sites); C's `Selector.index` likewise moved from `size_t` to `uint64_t` (`parse_usize` -> `parse_u64`), closing the same latent 32-bit truncation. Go was already `u64`, Python is unbounded. No 64-bit behavior change.

- ✅ Code Review 20260716 item 31: spec prose contradicts grammar and code on bare non-ASCII field names.
	- Prose says only reserved chars need quotes (and uses `Strasse` with a sharp s as an example); grammar and parser reject them.
	- Also: hex `-0x8000000000000000` (i64 min) reads BadType while the decimal spelling works. Detail: `design.md` - Code Review 20260716, item 31.
	- Done: aligned prose to the code (grammar, `is_bare_name_char`, and the emit predicate already agreed) - a bare field name is ASCII letters/digits/`-`/`_` only; anything else, including non-ASCII, must be quoted, and the `Straße` examples are now shown quoted. Hex fixed in all four parsers: parse the magnitude as u64, then range-check against the sign, so `-0x8000000000000000` reads i64-min like its decimal spelling and `+0x8000000000000000` stays BadType. Corpus case 019-hex-int-bounds pins it (crosscheck now 585).

- ✅ Code Review 20260716 item 35: value-taking options reject the space-separated form with a misleading error.
	- `--default 99` reports "unknown option: --default". Accept the space form or explain the `=` requirement.
	- Done: all four CLIs now accept both `--default=VALUE` and `--default VALUE` (same for `--on-bad`/`--strictness`), via an index loop that consumes the next arg; a value option with nothing after it says `missing value for --default (try --default=VALUE)`. Help text notes both spellings.

- ✅ Code Review 20260716 item 36: `check` reports "ok" and exits 0 even when diagnostics include Errors.
	- A CI gate on `check` passes configs whose lines were dropped. Nonzero exit or clearer wording; note it is corpus-pinned, so change everywhere at once.
	- Done (folded into item 19): `check` exits 6 whenever any error diagnostic is present, at any strictness - `failed: N diagnostic(s), M error(s)` for a standard/loose load that dropped lines, `strict load failed: N diagnostic(s)` for a strict failure, `ok (N diagnostic(s))` + exit 0 only when clean. Same strings and exit in all four bindings.

- ✅ Code Review 20260716 item 37: `--version`/`-h` are undiscoverable.
	- Help text never mentions them; `shcl help` and `shcl version` are rejected; `-w` is accepted but undocumented.
	- Done in all four CLIs: the usage block gained a `shcl help | version` line noting `-h/--help` and `-V/--version`; `shcl help` and `shcl version` are now accepted subcommands; the `fmt` line shows `[--write|-w]`.

- ✅ Code Review 20260716 item 38: wrapper documentation drift.
	- README omits the PowerShell wrapper; spec says "POSIX sh" but the shell wrapper is deliberately Bash. Align the words with the artifacts.
	- Done: README blurb + Status now name both the Bash and PowerShell wrappers; spec's two concrete "shipped wrapper" claims say Bash instead of POSIX sh (the illustrative two-tier table row stays, like the other not-yet-shipped language rows).

- ✅ Accessor: two-tier junior-friendly surface (convenience default plus full status), consistent across all bindings. A supplied default implies default mode.
	- Done alongside review item 6: the full status tier (`Read`/`read_*`) already shipped; the convenience default tier now ships in every library binding (`GetIntOr`/`get_*(default=)`/`shcl_get_*`/`get_or<T>`/native `unwrap_or`), plus the CLI `--default` for the wrapper bindings. Supplying the fallback is Default mode - value on `Good`, fallback otherwise.

- ✅ README rewrite: problem-first pitch, file and read-call examples, a format comparison table, a "wrong choice" section, and honest alpha status.

### Future and/or deferred

### Canceled
