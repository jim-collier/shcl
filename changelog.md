# Changelog

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Unreleased

### Added

- Schema validation: a schema is itself a plain SHCL file; `validate(doc, schema)` in every binding and `shcl check --schema=FILE` with stable `V###` codes.
- Layered loading: `merge(base, over)` in every binding; repeatable `--layer=FILE` plus `--set=PATH=VALUE` overrides on the loading subcommands.
- Schema-driven generation: `generate(schema)` and `shcl init --schema=FILE` emit a commented, typed starter config that validates clean against its own schema.
- Installer packages: `.deb` and `.rpm` per Linux binary and an NSIS Windows setup, covered by the release checksums.
- `set --write` rewrites the base file in place; `fmt --write` and `set --write` are both atomic (temp file + rename).
- A nesting-depth cap (512 levels, `E016`) so deep or hostile documents fail loading cleanly instead of crashing formatting or merging.
- An `E017` diagnostic for a value that opens a quote it never closes (previously silent, and it swallowed the trailing comment).
- Writer setters report whether the write applied; an unusable path (wildcard, missing `[#N]`) is a loud failure with nothing half-created.

### Changed

- A bare section header in a higher layer now merges instead of silently deleting the base subtree; clearing a leaf from a layer still works.
- `shcl set` op values are validated with the reference's grammar in every binding (malformed, out-of-range, or non-ASCII-digit values are rejected identically).
- Every subcommand rejects options it does not use instead of silently ignoring them.
- `init` exits 6 on a broken schema, matching `check --schema`; its stderr lists the schema faults in every binding.
- The C binding's memory use is flat for read-heavy and merge-heavy workloads (per-call scratch arena, in-place merges, geometric stacked-list growth).
- The Windows installers edit `PATH` at the registry, segment-wise and type-preserving; installer downloads pin https with a TLS 1.2 floor.

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
