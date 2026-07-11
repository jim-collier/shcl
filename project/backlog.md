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

- 🛠️ Language spec rationalized from `notes.txt`: terminology, model, types, reader API, formatter, raw blocks. See `spec.md` + `grammar.abnf`.

- ✅ Resolve the open minor items listed at the end of `spec.md` (currency-symbol set, wildcard-missing behavior, `onBad` enum surface, `%`->int, fence info-string meaning). All five settled inline in `spec.md` under "Resolved minor items".

- 🔘 Reference parser (pick one language first, likely Go or Rust) implementing the full spec, driven by the conformance corpus.

- 🔘 Ports to the remaining reader languages (Go, Rust, C, C#, Java, Python, JavaScript, PowerShell, POSIX sh), each single-file/drop-in where possible, all green on the corpus. API rule: type via typed entry point / compile-time generic, never a runtime type field.
	- Companion typed surfaces (not separate parsers): C++ template header over the C core; Kotlin reified-generic extensions over the Java core; TypeScript `.d.ts` over the JS core.

- 🔘 Expand the conformance corpus (`conformance/`) to cover the hard edges: dates/ambiguity, coercion, quoting/escapes, fehu, indentation errors, raw blocks, selectors/wildcards.

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

- 🛠️ Reader API: two-tier junior-friendly surface (convenience default + full status), consistent across all bindings; a supplied default implies default-mode

### Done

#### Done - Initial requirements

#### Done - Bugs

#### Done - Features and enhancements

### Future and/or deferred

### Canceled
