# Style guide

The canonical coding and prose style for this repo. `contributing.md` covers process; this file covers how the code and docs are written, and why. Where this guide and a general-purpose "best practice" disagree, this guide wins on purpose - the deviations are listed per language, each with its reason.

## Parity over idiom

This is the one rule that overrides everything else in this file, and the reason parts of this codebase are deliberately *not* written the way a style checker for each language would suggest.

The bindings are one program written several times. Rust is the reference; Go, Python, and C are independent ports of it. All of them must produce byte-for-byte identical output - same stdout, same exit codes, same diagnostic codes - and the conformance corpus plus the cross-binding differential check enforce that on every build.

To keep that promise cheap to maintain, every port mirrors the reference's *structure*, not just its behavior:

- Same function inventory and call flow. The reference's `read_string` is Go's `ReadString`, Python's `read_string`, C's `shcl_read_string`. One concept, one name, spelled in each language's case convention.
- Same parse pipeline, same order of operations, same helper boundaries. A line of the reference has a recognizable sibling line in every port.
- Same user-visible strings where output is the contract (diagnostic codes, `check` summaries, CLI usage errors).

Why: when a bug is fixed or a feature lands in the reference, the port is a mechanical diff of the same function in each sibling - not a re-derivation. Structural divergence is where behavioral divergence hides; two "equivalent" idiomatic implementations of the same rule will eventually disagree on an edge case, and in this project that is a shipped bug by definition.

The cost is accepted openly: some code reads as slightly foreign to a native of its language. A Go reader will notice reads return a status struct instead of an `error`; a Python reader will notice explicit character-class loops where `str.strip()` would be shorter. Those are not oversights - each one exists because the shorter idiom behaves differently from the reference in some corner (the specific cases are listed below).

Scope of the rule: parity governs structure and behavior. Idiom still governs the surface - each language keeps its own formatter, naming case, error-propagation syntax, and doc-comment conventions. The goal is "obviously the same program, written by someone fluent in each language", not transliterated Rust.

New bindings (Tier 3) follow the same recipe: port the reference function-for-function, run the corpus, and don't ship until the crosscheck is green.

## Rules for every language

- Formatters win. Where a canonical formatter exists (rustfmt, gofmt), its output is the law - run it, don't hand-format against it. Intentional data tables get the formatter's skip pragma rather than a fight.
- Tabs for indentation, spaces for alignment, in every language that allows it. This repo pins rustfmt to `hard_tabs`; gofmt is tabs natively; Python and shell follow suit. Yes, PEP 8 prefers spaces - one indentation style across a multi-language repo is worth more than per-language purity here.
- Names are meaningful and human-searchable: `upperBound`, not `ub`. Short conventional names are fine where they are idiomatic - loop indices (`i`), a local `err`, a receiver letter in Go, and few-line locals in the compact parser cores (`t` for a just-trimmed line, `q` for the current quote char) where the same short name means the same thing at the same site in every binding. The test is "can a reader find and search-replace what you mean", not maximal length.
- Comments are terse and explain *why*, not *what*. No narration of the next line, no banner dividers, ASCII only (`->` not arrows; `©` is fine). If a line needs a what-comment, rewrite the line.
- Every source file starts with the SPDX line and copyright, then a short purpose block. Library files also state the drop-in story and the parity contract.
- Single file per binding, zero dependencies. That is the product ("copy this file into your tree"), so no module splits, no helper crates/packages, and no dependency however good.
- Small standalone utility scripts are MIT regardless of anything else, and carry their license in the header.

## Per-language

### Rust (reference)

- rustfmt is canonical, with the repo's `rustfmt.toml` (`hard_tabs = true`). Clippy gates at its default level; pedantic is advisory.
- Errors as values via `Result` and `?` where a real error exists. But most "failure" here is not an error: a missing or mistyped value is a normal, expected outcome, so reads return a `Read<T>` carrying a `Status` - by spec, not exception-shaped. No `panic!`/`unwrap`/`expect` outside tests.
- No `thiserror`/`anyhow`, no error-crate ergonomics: the zero-dependency rule outranks them, and `Status` + structured diagnostics already cover the domain.
- Derive (`Debug`, `Clone`, `PartialEq`) rather than hand-roll. Public items get `///` docs. Early returns and `let .. else` over nesting.

### Go

- gofmt and `go vet` gate. Standard library only.
- Reads return `Read[T]` (generics) with a `Status` field, not `(T, error)`. Deliberate deviation: the reference models read failure as data, and an `error` return would push every port toward different control flow. Real errors (I/O, bad UTF-8) still use `error` normally.
- No interfaces: there is one implementation of everything and no test seam that needs one. No goroutines: parsing is single-pass and single-threaded by design.
- Exported identifiers get doc comments starting with the name. Short names in small scopes, descriptive in the public API.

### Python

- Python 3.9+, stdlib only. ruff and mypy (default mode) gate; black does not run - the repo's tab indentation stands (see the repo-wide rule above).
- Status-carrying reads instead of exceptions, matching the reference; exceptions are for real faults, not for "value not found".
- Several stdlib shortcuts are banned because they diverge from the reference's semantics: `str.strip()` (Python's whitespace set differs from Rust's), `str.lower()` (locale/Unicode folding; ASCII-only folding is the spec), bare `round()` (banker's rounding; the spec is half-away-from-zero), unbounded `int` (range-check against i64). Each ban is commented at the site it matters.
- Control flow mirrors the reference, so it leans more LBYL than idiomatic EAFP. That is the parity rule at work.
- Type hints exist where they pay; the public surface is not yet fully hinted and mypy strict is not a gate.

### C (and the C++ veneer)

- C11, single header, STB-style (`#define SHCL_IMPLEMENTATION` in one TU). The gate is `-Wall -Wextra -Werror` plus cppcheck.
- Memory model: one bump arena per document, `shcl_free` frees everything, no per-object ownership. Raw pointers are fine here - this is C working as designed, not a RAII gap.
- Strings are length-delimited byte spans with explicit UTF-8 iteration helpers, because the reference iterates `char`s and byte-wise shortcuts mis-handle multibyte input.
- The C++ veneer (`shcl.hpp`) is a thin typed wrapper over the C core - not a second parser, and kept intentionally small. C++17 (the gate's pin, for broad compiler reach), no exceptions: it surfaces the same `Status` values the core returns.

### Bash and PowerShell (wrappers)

- These are front ends to the compiled `shcl` binary, not parsers - they inherit conformance for free.
- shellcheck gates the bash wrapper; shfmt does not rewrite (its output fights the house shell style). PSScriptAnalyzer gates the ps1.
- The ps1 deviates from PowerShell convention on purpose: no `param()` block (it would eat `get`/`--int` before they reach the binary in dual-mode use) and `shcl_*` snake_case helper names instead of Verb-Noun (the sourced call surface is identical across bash and pwsh, which outranks the local convention for a two-line forwarder).
- Shell scripts keep the house compact style: `#•••` section rules, minified helper functions, the header layout you see in the existing files.

## Prose and docs

- Markdown never hard-wraps: one paragraph or bullet is one physical line.
- Short sentences. Nested bullets over comma-chained clauses. Minimal bold/italics/caps, no drama, no unicode beyond what the content needs.
- Filenames are lowercase, except `README.md` and `CLAUDE.md`.
- Docs update in the same change as the code they describe, not after.
