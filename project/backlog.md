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

- ✅ Resolve the open minor items listed at the end of `spec.md` (currency-symbol set, wildcard-missing behavior, `onBad` enum surface, `%`->int, fence info-string meaning). All five settled inline in `spec.md` under "Resolved minor items".
	- Currency and `%`->int later superseded: cut from default behavior, Loose-level only (see strictness levels below).

- 🔘 Reference parser in Rust (Tier 1) implementing the full spec, driven by the conformance corpus; the `shcl` CLI builds from it.

- 🔘 Ports to the remaining binding languages, in tiers: Tier 2 = Go, C, Python; Tier 3 (post-v1.0, corpus-gated) = C#, Java, JavaScript. Each single-file/drop-in where possible, corpus-green before shipping. API rule: type via typed entry point / compile-time generic, never a runtime type field.
	- Companion typed surfaces (not separate parsers): C++ template header over the C core; Kotlin reified-generic extensions over the Java core; TypeScript `.d.ts` over the JS core.
	- POSIX sh and PowerShell: thin wrappers around the `shcl` CLI, not parsers.

- 🛠️ Expand the conformance corpus (`conformance/`) to cover the hard edges: dates/ambiguity, coercion, quoting/escapes, indentation errors, raw blocks, selectors/wildcards, strictness levels.
	- 🛠️ Case `002-stacked-array` added (inline-vs-stacked parity + array-vs-instances). Remaining edges still to cover.
	- ✅ Cases `003-coercions` and `004-strict-load` added (strictness bundles: currency/`%`/rounding/boolean sets per level; load ok-vs-fail per level). `reads.tsv` gained an optional `level` column and a `load` pseudo-call.
	- 🔘 Model diagnostic expectations (count, severity, mandatory repeated-leaf hint) once the reference parser fixes the diagnostic shape.

### Build, CI/CD, and install

- 🛠️ A CI/CD pipeline kicked off by a bash script (`cicd/cicd.bash`): builds, tests, and can commit and push. Packaging and publishing are opt-in.
	- 🛠️ Engine + `config.bash` in place; `--ci` mode is the correctness gate the GitHub workflow (`.github/workflows/ci.yml`) runs, so local and CI share one definition of passing. Build/lint/test stages are stubs until the reference parser lands; shellcheck of the pipeline itself is live now.

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

### Future and/or deferred

### Canceled
