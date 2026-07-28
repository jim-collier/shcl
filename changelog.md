# Changelog

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## v1.0.0 - 2026-07-28

First stable release. The language, the read and write APIs, and the diagnostic codes are now covered by semantic versioning: a breaking change to any of them means 2.0.0.

### Added

- Published packages. Rust on crates.io (`cargo add shcl`), Python on PyPI (`pip install shcl`), and the Go module by tag (`github.com/jim-collier/shcl/source/go`). Binding versions move in lockstep with the product version, so each ecosystem's ordinary compatible-version operator tracks minor and patch releases without crossing a major.
- Signed releases. The `sha256sums` file now ships with a detached signature made offline with the project signing key, and both install scripts verify it before trusting any checksum out of that file. The public key is at `shcl-signing.pub` for manual verification.

### Changed

- The Windows `user` install moved from `%USERPROFILE%\bin\Shcl` to `%LOCALAPPDATA%\Programs\Shcl`, and adds that directory to `PATH` directly instead of keeping a second copy of the executable one level up.
- `install.bash` now requires `openssl` alongside `curl` or `wget`. There is no unverified-install fallback.
- The trademark policy is now built around conformance: an implementation that passes the published corpus may carry the name, without asking and without affiliation.

## v1.0.0-rc1 - 2026-07-26

### Added

- Schema validation: a schema is itself a plain SHCL file. `validate(doc, schema)` in every binding, and `shcl check --schema=FILE` with stable `V###` codes.
- Layered loading: `merge(base, over)` in every binding, plus repeatable `--layer=FILE` and `--set=PATH=VALUE` overrides on the loading subcommands.
- Schema-driven generation: `generate(schema)` and `shcl init --schema=FILE` emit a commented, typed starter config that validates clean against its own schema.
- Installer packages: a `.deb` and `.rpm` per Linux binary and a Windows setup, all covered by the release checksums.
- `paths()` in every binding and the C++ veneer, to enumerate a document's field paths in file order.
- `set --write` rewrites the base file in place, matching `fmt --write`.
- Two new diagnostics: `E016` for a document nested past the 512-level cap, and `E017` for a value that opens a quote it never closes.

### Changed

- Blank lines between bindings now survive the canonical formatter instead of being discarded. Runs of them collapse to one.
- Every subcommand rejects options it does not use instead of silently ignoring them.
- `init` exits 6 on a broken schema, matching `check --schema`, and lists the schema faults.
- Large-document performance: merging, inline `[value]` selectors, stacked lists, raw-block formatting, and schema suggestions all dropped from quadratic to linear. A 32k-key merge went from 16 seconds to 0.07 in the reference, with byte-identical output.
- C memory use is flat for read-heavy and merge-heavy workloads, where it previously grew without bound.
- The Windows installers edit `PATH` in the registry segment-wise and preserve its type. Installer downloads require https with a TLS 1.2 floor.
- The library degrades gracefully on a slipped internal invariant rather than aborting.
- The spec states that lowercase is a field name's canonical spelling.

### Fixed

- A bare section header in a higher layer merged over a base document deleted the whole subtree below it. It now merges. Clearing a leaf from a layer still works.
- A write to an unusable path (a wildcard, or a missing `[#N]`) reported success and could leave a half-created path behind. Setters now report whether the write applied, and nothing is created unless the whole path is usable.
- `fmt --write` could leave a truncated file if interrupted, and C reported success on a failed write. Both writers are now atomic.
- An in-place write no longer discards what it replaces. The target's permission bits are preserved, and a symlinked config is written through instead of being replaced by a regular file. Any other hard link to the file still keeps the old content, which a rename cannot avoid.
- The Writer could create two siblings that a reparse would merge, so its output was not a formatter fixpoint.
- `shcl set` op values were validated four different ways across the bindings. All four now use the reference's grammar.
- `shcl init --schema` could generate a file that its own schema rejected, and could emit lines that do not parse.
- Go `Validate` aborted the process on a schema path with a `[#N]` selector at or above 2^63.
- Deeply nested documents crashed three of the four bindings, each at a different depth. The nesting cap catches them at load.
- The C CLI silently capped `--layer` and `--set` at 64 each.
- `shcl.h` did not compile as a drop-in C++ translation unit under `-Wall -Wextra -Werror`.
- README and spec examples that named APIs which do not exist.

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
