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

- ✅ Initialize the git repo (none exists yet at repo root or `github/`); wire remote `git@github.com:jim-collier/shcl.git`.
	- ✅ Done at `github/`; `main` plus feature-branch flow.

- 🛠️ Language spec rationalized from `notes.txt`: terminology, model, types, accessor/writer API, formatter, raw blocks. See `spec.md` + `grammar.abnf`.
	- ✅ Stacked (`*`) block-array form added alongside inline commas; both spellings read identical and canonicalize to inline (`spec.md` Arrays, `grammar.abnf` `array-elem-line`).
	- ✅ Date/time formats pinned to a closed whitelist (year-first or named-month only; `-`/`/`/`.` delimiters; `T`/space/`_`/delimiter date-time separator; optional `.`-fractional seconds; shape match then calendar validation; epoch and non-year-first numerics rejected). Replaces "common formats with any delimiters". Conformance cases still to add with the corpus expansion.
	- ✅ Adoption-concerns sweep: fehu rune removed entirely (raw blocks are the verbatim hatch); currency/`%`/float->int-rounding/extra boolean tokens cut from default behavior; field-name case folding restricted to ASCII A-Z; repeated-leaf "did you mean an array?" hint now mandatory (severity `hint`); design-goals wording drops "simplest possible".
	- ✅ Strictness levels: Loose/Standard/Strict per-document knob (CLI `--strictness`, aliases 1/2/3), default Standard; normative bundle table in `spec.md`; Loose re-admits the cut coercions as a closed list; Strict fails the load on any `error` diagnostic; diagnostics gained severity (`error`/`hint`).
	- ✅ Bindings re-tiered: Tier 1 = Rust reference + `shcl` CLI; Tier 2 = Go, C (+C++ veneer), Python; Tier 3 = rest, post-v1.0, corpus-gated; POSIX sh + PowerShell are CLI wrappers, not parsers; parity promise reworded to "every shipped binding is corpus-green".
	- ✅ Raw-block binding reworked: a fence is a value line for its parent field (fills an empty value, or adds an instance - repeated-leaf rule); named/anonymous split dropped; child-indent spelling is canonical. `spec.md` Raw blocks + formatter, `grammar.abnf`, `design.md`.
	- ✅ Inline-array commas made fully forgiving: leading/doubled/trailing commas drop their empty slots, an all-comma value is the empty array, none of it errors; quote `""` for a deliberate empty element. `spec.md` Arrays, `grammar.abnf` `array`.

- ✅ Resolve the open minor items listed at the end of `spec.md` (currency-symbol set, wildcard-missing behavior, `onBad` enum surface, `%`->int, fence info-string meaning). All five settled inline in `spec.md` under "Resolved minor items".
	- Currency and `%`->int later superseded: cut from default behavior, Loose-level only (see strictness levels below).

- ✅ Reference parser in Rust (Tier 1) implementing the full spec, driven by the conformance corpus; the `shcl` CLI builds from it.
	- ✅ Single-file library (`source/rust/src/lib.rs`, zero dependencies) + `shcl` CLI (`get`/`fmt`/`check`/`count`/`instances`, typed flags, strictness, stdin, status exit codes). Corpus-green; deterministic fuzz smoke (no panics, formatter is a fixpoint) runs in `cargo test`.
	- ✅ Fuzzing forced two formatter rules, now in `spec.md`: same-line block spelling when an earlier same-name empty instance would absorb the header, and a space before an info-string starting with the fence char.
	- ✅ `count` rows in case 006 corrected to instance counts (spec + case 002 define `Count`; the two rows were asserting array lengths).

- 🛠️ Ports to the remaining binding languages, in tiers: Tier 2 = Go, C, Python; Tier 3 (post-v1.0, corpus-gated) = C#, Java, JavaScript. Each single-file/drop-in where possible, corpus-green before shipping. API rule: type via typed entry point / compile-time generic, never a runtime type field.
	- ✅ Go (Tier 2): single-file library (`source/go/shcl.go`, zero deps, generics for the typed reads) + CLI (`source/go/cmd/shcl/`) with the same flags/output/exit codes as the reference. Corpus-green natively (`go test`); crosscheck now compares rust vs go on every run - corpus + fuzz set, byte-for-byte.
	- ✅ Python (Tier 2): single-file library (`source/python/shcl.py`, stdlib-only, 3.9+) + CLI (`source/python/cmd/shcl/main.py`) with the same flags/output/exit codes. Corpus-green natively (`source/python/tests/conformance.py`); crosscheck now compares rust vs go vs python every run, byte-for-byte. Float output via shortest-round-trip positional form (`Decimal(repr(v)).normalize()`), never scientific, matching the reference Display.
	- ✅ C (Tier 2): single-header drop-in library (`source/c/shcl.h`, C11, zero deps, `#define SHCL_IMPLEMENTATION` in one TU) + CLI (`source/c/cmd/shcl/main.c`) with the same flags/output/exit codes. Corpus-green natively (`source/c/tests/conformance.c`); crosscheck now compares rust vs go vs python vs c every run, byte-for-byte. Companion C++ template veneer (`source/c/shcl.hpp`, `get<T>()`/`Read<T>`) over the same core, with its own compile+behavior smoke. Float output via shortest-round-trip positional form (minimal `%e` precision reflowed to fixed notation), never scientific, matching the reference Display.
	- Remaining companion typed surfaces (not separate parsers): Kotlin reified-generic extensions over the Java core; TypeScript `.d.ts` over the JS core. (C++ over the C core: done above.)
	- 🛠️ Shell and PowerShell: thin wrappers around the `shcl` CLI, not parsers.
		- ✅ Shell wrapper landed as Bash (`source/bash/shcl.bash`), not POSIX sh: it earns Bash by being dual-purpose - run as a script or `source` it and call functions, same args, and it forwards the binary's exit code verbatim. `shcl` is the whole CLI; `shcl_get`/`shcl_int`/`shcl_bool`/`shcl_array`/... are typed sugar for sourced callers. Binary resolved via `$SHCL_BIN`, else a sibling `shcl`, else PATH, else the repo build. Strict mode is set on the run path only so sourcing can't disturb the caller. Shellchecked in cicd; rides the dogfood install.
		- 🔘 PowerShell wrapper.

- 🛠️ Expand the conformance corpus (`conformance/`) to cover the hard edges: dates/ambiguity, coercion, quoting/escapes, indentation errors, raw blocks, selectors/wildcards, strictness levels.
	- 🛠️ Case `002-stacked-array` added (inline-vs-stacked parity + array-vs-instances). Remaining edges still to cover.
	- ✅ Cases `003-coercions` and `004-strict-load` added (strictness bundles: currency/`%`/rounding/boolean sets per level; load ok-vs-fail per level). `reads.tsv` gained an optional `level` column and a `load` pseudo-call.
	- ✅ Case `006-comma-edges` added: forgiving inline arrays (leading/doubled/trailing/all-comma drop to empties; `""` keeps a real empty element).
	- ✅ Case `005-raw-blocks` added (both spellings bind as the field's value; extra fences become instances; `\n` escaping for raw values in `reads.tsv`).
	- ✅ Case `007-dates` added: the full date/time whitelist (year-first, named-month, compact, 12/24-hour, fractions, zones) plus the rejects (ambiguous numeric, invalid calendar, prose).
	- 🔘 Model diagnostic expectations (count, severity, mandatory repeated-leaf hint) once the reference parser fixes the diagnostic shape. The reference shape now exists: line + severity (`error`/`hint`) + message.

### Build, CI/CD, and install

- 🛠️ A CI/CD pipeline kicked off by a bash script (`cicd/cicd.bash`): builds, tests, and can commit and push. Packaging and publishing are opt-in.
	- ✅ Engine + `config.bash` in place; `--ci` mode is the correctness gate the GitHub workflow (`.github/workflows/ci.yml`) runs, so local and CI share one definition of passing.
	- ✅ All stages live: cargo fmt (check-only in CI), debug build, clippy `-D warnings` + shellcheck, tests (conformance corpus + 20k-iteration fuzz smoke), profiler (flamegraph + hot-spot report), native release + Windows x86_64 / Linux ARM64 / Windows ARM64 cross builds, versioned artifacts + sha256sums, README demo gif (`cicd/demo-scenario.toml` -> `assets/demo.gif`), publish via `cicd/utility/n8git_backup-and-publish`.
	- ✅ Guard rails: run log tee + rotation under `cicd/artifacts/lint/` (`lint-report.bash` reads the newest), tool-pin drift warnings, `--quick` skips cross + profiler + gif.
		- Version guard dropped (was: new code on dev needs a version bump) - versions get cut deliberately at release time instead.
	- ✅ Profiler stage: in-process sampler (feature-gated, dev-only - kernel perf is locked down on the build box) over a corpus-derived `fmt` workload -> gfs-rotated flamegraph SVG under `cicd/artifacts/profiling/` + `flame-report.py` hot-spot summary (plain mode each run, `--check` for a session-startup look).
	- ✅ Cross-binding differential check (`cicd/utility/crosscheck.bash`) wired after tests: all binding CLIs must agree byte-for-byte (stdout + exit code) on `fmt` over every corpus input, every `reads.tsv` row, and a fuzz-dumped input set (`SHCL_FUZZ_DUMP`). Reference = rust; go and python compared against it every run.
	- ✅ Per-stage config extras (`FMT/BUILD/LINT/TEST_EXTRA`) so each new binding wires its fmt/build/lint/test commands into the engine from `config.bash` only; Go uses gofmt/go build/go vet/go test; Python uses a py_compile build gate + the stdlib conformance runner. CI workflow installs Go via `setup-go` from `go.mod` and Python via `setup-python`.
	- ✅ Dogfood stage (7/9, between release and demo gif): installs the just-built native release `shcl` into the first existing+writable fixed dir (`~/synced/.../linux/bin`, same path the sister project uses) so the copy launched by hand stays current. `--no-dogfood` to skip, off under `--ci` (no side effects, no stale binary), no sudo (a non-writable dest is passed over). The bash wrapper rides along as `shcl.bash` once that tier lands.
	- 🔘 Packaging (.deb/.rpm/NSIS) still open; wire from the sister packaging stage when release cuts start.

- 🔘 Dev-environment install script (Linux bash, macOS sh, Windows PowerShell), runnable via a single `curl`/`wget` and documented under "how to develop". Clones main, installs dependencies, and states what it will do with an option to abort.

- 🔘 Release-install script per platform, runnable via a single `curl`/`wget` and documented under "how to install". Downloads, installs, and runs the latest release, with an option to abort.

### Configuration and persistence

- 🔘 Default configuration hard-coded
	- 🔘 Overridden by per-user config file, created the first time a default setting is changed.
		- 🔘 Settings live under `~/.config` (YAML or TOML), resistant to errors (e.g. don't bail on the whole thing due to one bad line).
	- 🔘 Overridden by program options at run-time.

### Performance and security

### Other

## Backlog

### Misc to-do

### Bugs

### Features and enhancements

- 🛠️ Accessor: two-tier junior-friendly surface (convenience default + full status), consistent across all bindings; a supplied default implies default-mode

- 🔘 Schema-as-SHCL validation: schema is a plain SHCL file (type, required, allowed values); `Validate(doc, schemaDoc)` returns structured diagnostics; catches unknown/misspelled fields. Schema vocabulary interpreted by the validator, not the parser. See `design.md` "Power layer".
	- Needs the reference parser first; spec the schema vocabulary alongside it.

- 🔘 Layered loading: `Load(defaults, site, user, ...)` merging later over earlier via the existing merge rule; CLI/env overrides as the top layer.

- 🔘 Schema-driven generation: Writer + schema emits a commented, typed starter config (`shcl init --schema ...`). Depends on schema validation above.

### Done

#### Done - Initial requirements

#### Done - Bugs

#### Done - Features and enhancements

- ✅ README rewrite: problem-first pitch, file + read-call examples, format comparison (JSON/YAML/TOML/XML table plus Pkl/CUE/Dhall notes), "wrong choice" section, honest alpha status.

### Future and/or deferred

### Canceled
