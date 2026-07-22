# Changelog

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## v1.0.0-beta2 - 2026-07-21

### Added

- Writer API in every binding (typed `set_*`, array and `_default` forms, `set_raw`, `remove`, comments, empty-doc constructor) plus a `shcl set` CLI subcommand driven by a tab-delimited ops script.
- Comments are preserved through the canonical formatter as node trivia rather than being discarded.
- Stable diagnostic codes (`E###`/`H###`) on `check`, printed as `line N: severity: CODE` on stdout with human prose on stderr.
- Convenience read tier with whole-value fallbacks (`get_*_or` / `default=`) across the bindings.
- `--rawinfo` accessor for a raw block's info string.
- Per-slot statuses on wildcard array reads, plus `--slots` and per-slot `--default` on the CLI.

### Changed

- `check` exits nonzero whenever any error diagnostic is present, not only on strict-load failure.
- Value options accept both `--opt=VALUE` and the space-separated `--opt VALUE` spelling; `help` and `version` are also accepted as subcommands.
- Uniform behavior across bindings for broken-pipe exit, invalid argument encoding, and out-of-memory.
- Dogfood install is now atomic (stage to a temp file in the destination, then rename over the target).

### Fixed

- Hex integer bounds now read `i64::MIN` correctly and reject out-of-range magnitudes.
- Merge keys made injective so distinct arrays and raw blocks can no longer collide.
- Large-document formatting no longer risks a stack overflow (iterative emit).

## v1.0.0-beta1 - 2026-07-15

First tagged pre-release.

### Added

- Language spec and formal grammar (`project/spec.md`, `project/grammar.abnf`).
- Conformance corpus of golden cases that every binding must pass.
- Rust reference parser and the `shcl` CLI (`get`, `fmt`, `check`, `count`, `instances`).
- Independent parsers for Go, C (with a C++ veneer), and Python, each corpus-green.
- Bash wrapper around the CLI, usable as a script or sourced for typed helpers.
- Three strictness levels (loose, standard, strict) as a per-document knob.
- Raw fenced blocks for verbatim multi-line content.
