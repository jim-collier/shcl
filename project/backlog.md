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
	- Next: model diagnostic expectations (count, severity, the mandatory repeated-leaf hint) in the corpus. See conformance item below.

- 🛠️ Ports to the remaining binding languages, in tiers. Tier 2 done; Tier 3 (C#, Java, JavaScript) after v1.0. Each drop-in where possible, corpus-green before shipping. Type via a typed entry point or compile-time generic, never a runtime type field.
	- Done: Go, C (with a C++ veneer), and Python, each an independent parser with the same flags, output, and exit codes as the reference. All corpus-green and held byte-for-byte to the reference on every build.
	- Note: remaining companion surfaces are veneers, not new parsers. Kotlin over the Java core, TypeScript over the JavaScript core.
	- 🛠️ Shell wrappers around the CLI, not parsers.
		- Done: Bash wrapper (`source/bash/shcl.bash`). Runs as a script or sourced for typed helpers. Forwards the binary's exit code unchanged.
		- 🔘 PowerShell wrapper.

- 🛠️ Expand the conformance corpus (`conformance/`) to cover the hard edges: dates and ambiguity, coercion, quoting and escapes, indentation errors, raw blocks, selectors and wildcards, strictness levels.
	- Done: cases for stacked arrays, coercion bundles, strict-load behavior, forgiving commas, raw blocks, and the full date whitelist.
	- 🔘 Model diagnostic expectations (count, severity, the mandatory repeated-leaf hint). The reference diagnostic shape now exists (line, severity, message), so this can be pinned next.

### Build, CI/CD, and install

- 🛠️ A CI/CD pipeline driven by `cicd/cicd.bash`: builds, tests, and can commit and push. Packaging and publishing are opt-in. See `design.md` for the split-by-responsibility rationale.
	- Done: all stages live. Format check, build, lint, tests plus fuzz smoke, profiler, native and cross builds, versioned artifacts, README demo gif, publish.
	- Done: `--ci` mode is the correctness gate the GitHub workflow runs, so local and CI share one definition of passing.
	- Done: cross-binding differential check. Every binding CLI must agree with the reference byte-for-byte on the corpus and a fuzz-dumped input set.
	- Done: dogfood stage installs the fresh release binary to a fixed local dir. Off under `--ci`, no sudo path.
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

### Features and enhancements

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
