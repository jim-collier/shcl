<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- hard tabs -->
<!-- markdownlint-disable MD033 -- inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
# Style guide

The canonical coding and prose style for this repo. `contributing.md` covers process; this file covers how the code and docs are written, and why. Where this guide and a general-purpose "best practice" disagree, this guide wins on purpose - the deviations are listed per language, each with its reason.

## Parity over idiom

This is the one rule that overrides everything else in this file, and the reason parts of this codebase are deliberately *not* written the way a style checker for each language would suggest.

The bindings are one program written several times. Rust is the reference; Go, Python, and C are independent ports of it. All of them must produce byte-for-byte identical output - same stdout, same exit codes, same diagnostic codes - and the conformance corpus plus the cross-binding differential check enforce that on every build.

To keep that promise cheap to maintain, every port mirrors the reference's *structure*, not just its behavior:

- Same function inventory and call flow. The reference's `read_string` is Go's `ReadString`, Python's `read_string`, C's `shcl_read_string`. One concept, one name, spelled in each language's case convention.

- Same parse pipeline, same order of operations, same helper boundaries. A line of the reference has a recognizable sibling line in every port.

- Same user-visible strings where output is the contract (diagnostic codes, `check` summaries, CLI usage errors).

Why: when a bug is fixed or a feature is added to the reference, the port is a mechanical diff of the same function in each sibling - not a re-derivation. Structural divergence is where behavioral divergence hides; two "equivalent" idiomatic implementations of the same rule will eventually disagree on an edge case, and in this project that is a defect by definition.

The cost is accepted openly: some code reads as slightly foreign to a native of its language. A Go reader will notice reads return a status struct instead of an `error`; a Python reader will notice explicit character-class loops where `str.strip()` would be shorter. Those are not oversights - each one exists because the shorter idiom behaves differently from the reference in some corner (the specific cases are listed below).

Scope of the rule: parity governs structure and behavior. Idiom still governs the surface - each language keeps its own formatter, naming case, error-propagation syntax, and doc-comment conventions. The goal is "obviously the same program, written by someone fluent in each language", not transliterated Rust.

New bindings (Tier 3) follow the same recipe: port the reference function-for-function, run the corpus, and don't release until the crosscheck is green.

## Rules for every language

- Formatters win. Where a canonical formatter exists (rustfmt, gofmt), its output is the law - run it, don't hand-format against it. Intentional data tables get the formatter's skip pragma rather than a fight.

- Tabs for indentation, spaces for alignment. The test is whether the language prevents it or strongly pushes the other way; none of the six here do, so all six use tabs. This repo pins rustfmt to `hard_tabs`; gofmt is tabs natively; Python, C and both shells follow suit. PEP 8 does prefer spaces, but it also says to stay consistent with code already indented with tabs, and one indentation style across a multi-language repo is worth more than per-language purity. The Python linter's tab rule (`W191`) is switched off for exactly this reason, not overlooked.

- Names are meaningful and searchable: `upperBound`, not `ub`. Short conventional names are fine where they are idiomatic - loop indices (`i`), a local `err`, a receiver letter in Go, and few-line locals in the compact parser cores (`t` for a just-trimmed line, `q` for the current quote char) where the same short name means the same thing at the same site in every binding. The test is "can a reader find and search-replace what you mean", not maximal length.

- Comments are terse and explain *why*, not *what*. No narration of the next line, ASCII only (`->` not arrows; `©` is fine). If a line needs a what-comment, rewrite the line.

- **Section rules are the one sanctioned banner.** In a single-file drop-in binding of a few thousand lines they are the only navigation aid, so each binding uses exactly one spelling for its major-section dividers and nothing else: Rust and Go a full-width `// ----...` line with the section title on the following comment line; Python the same three-line form spelled `# ----...` / `# <title>` / `# ----...`; C `// --- <title> ---...` padded to the margin, with `// ====...` reserved for the header/implementation split; shell keeps the house `#•••` rule. No other decorative comment forms.

- Every source file starts with the SPDX line and copyright, then a short purpose block. Library files also state the drop-in story and the parity contract.

- Single file per binding, zero dependencies. That is the product ("copy this file into your tree"), so no module splits, no helper crates/packages, and no dependency however good.

- Small standalone utility scripts are MIT regardless of anything else, and carry their license in the header.

- What analysis runs, and what deliberately does not. Gating: rustfmt and clippy, gofmt, go vet, staticcheck, govulncheck, cargo-deny, ruff, mypy, cppcheck, PSScriptAnalyzer, shellcheck, markdownlint. Declined, each for a reason that is not going to change:
	- `clang-format` and `clang-tidy`. Reformatting the C drop-in rewrites roughly nine lines in ten, which throws away the hand-aligned tables and the structural match with the other bindings.
	- `golangci-lint`. It wraps checks that already gate individually, so it would add a dependency and no signal.
	- `shfmt`. Its output fights the shell style the pipeline scripts are written in.
	- Pedantic clippy. Satisfying it means restructuring away from the reference's shape, which is the one thing parity forbids.

## Per-language

### Rust (reference)

- rustfmt is canonical, with the repo's `rustfmt.toml` (`hard_tabs = true`). Clippy gates at its default level; pedantic is advisory.

- Errors as values via `Result` and `?` where a real error exists. But most "failure" here is not an error: a missing or mistyped value is a normal, expected outcome, so reads return a `Read<T>` carrying a `Status` - by spec, not exception-shaped. No `panic!`/`unwrap`/`expect` outside tests, and none in released code: the only ones left sit behind the `profiling` feature, which never compiles into a normal build.

- No `thiserror`/`anyhow`, no error-crate ergonomics: the zero-dependency rule outranks them, and `Status` + structured diagnostics already cover the domain.

- Derive (`Debug`, `Clone`, `PartialEq`) rather than hand-roll. Public items get `///` docs. Early returns and `let .. else` over nesting.

- The setters are `#[must_use]`. Surface-only, so parity is untouched - the other three have no equivalent and say the same thing in prose. A dropped `false` means the save that follows writes a config missing the edit and reports success, which is the one failure here that leaves no trace anywhere; the compiler catches it for free in the one language that can.

### Go

- gofmt and `go vet` gate. Standard library only.

- Reads return `Read[T]` (generics) with a `Status` field, not `(T, error)`. Deliberate deviation: the reference models read failure as data, and an `error` return would push every port toward different control flow. Real errors (I/O, bad UTF-8) still use `error` normally.

- No interfaces: there is one implementation of everything and no test seam that needs one. No goroutines: parsing is single-pass and single-threaded by design.

- Exported identifiers get doc comments starting with the name. Short names in small scopes, descriptive in the public API.

- Deliberate deviation: the datetime type is `DateTime`, not `ShclDateTime`; the package name already carries the prefix. The reference and Python export `DateTime` as an alias so the two spellings meet.

### Python

- Python 3.9+, stdlib only. ruff and mypy (default mode) gate; black does not run - the repo's tab indentation stands (see the repo-wide rule above).

- Status-carrying reads instead of exceptions, matching the reference; exceptions are for real faults, not for "value not found".

- Several stdlib shortcuts are banned because they diverge from the reference's semantics: `str.strip()` (Python's whitespace set differs from Rust's), `str.lower()` (locale/Unicode folding; ASCII-only folding is the spec), bare `round()` (banker's rounding; the spec is half-away-from-zero), unbounded `int` (range-check against i64). Each ban is commented at the site it matters.

- Control flow mirrors the reference, so it leans more LBYL than idiomatic EAFP. That is the parity rule at work.

- Deliberate deviation: the parser's child/display accelerator maps key on exact tuples of the strings already in hand, where the other three bindings stream an FNV hash and verify hits against the arena. CPython's dict and tuple machinery runs at C speed while a hand-rolled per-byte hash loop does not, and the tuple keys are exactly as injective - same first-wins semantics, same behavior.

- The public surface is type-hinted (every public method, function and attribute); private helpers are hinted where it pays, and mypy strict is not a gate.

### C (and the C++ veneer)

- C11, single header, STB-style (`#define SHCL_IMPLEMENTATION` in one TU). The gate is `-Wall -Wextra -Werror` plus cppcheck.

- Memory model: one bump arena per document, `shcl_free` frees everything, no per-object ownership. Raw pointers are fine here - this is C working as designed, not a RAII gap. The one exception is the node vector, which lives in malloc/realloc storage: a bump arena cannot reclaim the abandoned half of each doubling, and the node array is the biggest thing that doubles.

- Strings are length-delimited byte spans with explicit UTF-8 iteration helpers, because the reference iterates `char`s and byte-wise shortcuts mis-handle multibyte input.

- The C++ veneer (`shcl.hpp`) is a thin typed wrapper over the C core - not a second parser, and kept intentionally small. C++17 (the gate's pin, for broad compiler reach), no exceptions: it returns the same `Status` values the core does.

- Deliberate deviation: Python carries three hot-path shortcuts the other bindings do not need - a length-one branch in the value display and the merge key, an identity test before the source-attach guard's key compare, and the path scanner's two helpers at module level rather than nested. Rust and Go stream these fields through a hash and build nothing, and a closure per call costs nothing there. The shortcuts change no output and are asserted structurally in the Python runner, not by a clock.

- Deliberate deviation: the convenience read tier covers only the value types (`shcl_get_int`/`_float`/`_bool` and the `_or` spelling of each; `get_or<T>` for the veneer's four `get<T>` types). String, raw, raw-info, datetime, and array reads hand back borrowed memory or lengths that a value-or-default signature cannot express, so those stay on the full `shcl_read_*` tier. The spec's ergonomic-tier section says the same. The other three bindings do carry all of them, raw-info included.

- Deliberate deviation: an allocation failure inside a parse or a validate unwinds and the call returns NULL, where the other three abort. Their languages abort on allocation failure and there is nothing there to mirror, while a C consumer embedding the header has a process that is not the library's to end. Everywhere else - a read, a write, a merge on a document already built - the `SHCL_OOM()` hook is still the answer.

- Deliberate deviation: `shcl_compact` has no counterpart in the other three. A write lands in the document's bump arena and the value it replaced stays there until `shcl_free`, so a process rewriting one field in a loop grows by a few dozen bytes per write; the other three reclaim the old value through their runtimes. Compaction rebuilds the document into fresh arenas, carrying the diagnostics, the lost count and the strictness with it, so a save or a strict gate afterwards reads the same. The C++ veneer exposes it as `compact()`.

- Deliberate deviation: `shcl_reads_release` has no counterpart in the other three. Read results are copied into the document's read arena, which only `shcl_free` reclaims - the right default for a read-once consumer and the documented contract. A process polling one document in a loop needs a way out, and the other three bindings have one for free because they hand back owned collections their runtime reclaims. The C++ veneer calls it on every read, since it copies each result into owned std types the moment it gets it.

- The read structs stay value+status, where the other three carry `raw`, `line` and `quoted` on the read result. Those two of the three that a C consumer can still ask for are separate accessors - `shcl_line`, `shcl_quoted` - because widening a by-value struct to carry a borrowed span costs every read that never looks at it.

### Bash and PowerShell (wrappers)

- These are front ends to the compiled `shcl` binary, not parsers - they inherit conformance for free.

- shellcheck gates the bash wrapper; shfmt does not rewrite (its output fights the house shell style). PSScriptAnalyzer gates the ps1.

- The ps1 deviates from PowerShell convention on purpose: no `param()` block (it would eat `get`/`--int` before they reach the binary in dual-mode use) and `shcl_*` snake_case helper names instead of Verb-Noun (the sourced call surface is identical across bash and pwsh, which outranks the local convention for a two-line forwarder).

- Shell scripts keep the house compact style: `#•••` section rules, minified helper functions, the header layout the existing files use.

## Performance

Correctness comes first, and in this repo parity comes with it: an optimization that makes one binding faster and the four of them disagree is a bug, not a win. Beyond that, speed and memory are treated as four separate stages, not as one thing to worry about at the end.

The wins that matter were locked in by the architecture, before any code was hot. Know these before reshaping a parser:

- Parsing is one forward pass, single-threaded, no backtracking. The line is the unit of error recovery, which is also what makes the forgiveness story cheap.

- C owns memory with a bump arena per document, plus a scratch arena that resets on every read call. No per-object ownership, no free list - `shcl_free` drops the whole thing.

- The parser builds its `(name, value)` child index as it goes, so in-file duplicate folding and merges never rescan siblings. It is discarded once the parse is done.

- Path lookups use a second index, keyed on `(parent, name)` and built the first time a lookup needs one, so reading every key in a large document does not scan siblings per key. The Writer keeps it current as it places, folds and removes nodes; only a merge drops it, and the next lookup rebuilds.

- Both indexes hold hashes and node indices only. Key strings are never built, and a hit is verified against the arena, so a hash collision costs a comparison rather than a wrong answer.

Then there are the habits, which cost nothing to write and are not premature:

- Reserve a collection when the size is knowable, and grow it geometrically rather than by one.

- Hoist anything invariant out of a loop. Look it up once.

- Build strings with the language's builder, never by repeated concatenation.

- In shell, no forks inside a loop. One pass of one tool beats a thousand subshells, and the pipeline's own scripts are held to that.

Optimizing early is fine when there is named evidence for it. The profiler stage runs on every non-quick pipeline run and writes a flamegraph plus a hot-spot summary, so "this path is already slow" can be a fact rather than a hunch. Without something like that, leave it alone.

Everything else waits for the end, and is driven by measurement only. Micro-optimization belongs here and nowhere else: profile an optimized build on a realistic workload, attack the top of the list, measure again, revert anything that did not move the number. The 20260725 performance tranche is the worked example - a 32k-node merge went from 16 seconds to 0.07, all of it accelerators rather than rewrites.

Release builds are tuned for size and speed together. The Rust release profile uses fat LTO, one codegen unit, `panic = "abort"` and symbol stripping. The binary is deliberately not packed - packers trip antivirus heuristics.

## Prose and docs

- Markdown never hard-wraps: one paragraph or bullet is one physical line.

- Short sentences. Nested bullets over comma-chained clauses. Minimal bold/italics/caps, no drama, no unicode beyond what the content needs.

- Filenames are lowercase, except where a tool or convention fixes the casing: `README.md`, `Cargo.toml`/`Cargo.lock`, `CODEOWNERS`, `FUNDING.yml`, `PSScriptAnalyzerSettings.psd1`.

- Docs update in the same change as the code they describe, not after.
