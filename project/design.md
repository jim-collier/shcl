<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
# Design

Design, requirements, and direction. The pre-v1.0.0 task list is in `backlog.md`. The full language definition is in `spec.md` (with `grammar.abnf`); this file stays high-level - the *why*, not the letter of the rules.

## Assumptions

- The raw, by-example origin of the language is `../../notes.txt`. It was rationalized into a coherent model through a decision pass; where the two disagree, `spec.md` wins.
- The language ships as tiered bindings, plus single-file drop-in source and compiled binaries per platform. We decided the guarantee is the corpus, not the count: every *shipped* binding is corpus-green, and nothing ships before it is. Tier 1: the Rust reference implementation and the `shcl` CLI built from it (Rust wins on the stated priorities - minimal binary size, instant startup for the CLI-wrapper bindings, clean `cdylib` for the shared-library mode, and compile-time strictness that forces spec precision). Tier 2: Go, C (+ C++ veneer), Python. Tier 3: the rest (C#, Java (+ Kotlin), JavaScript (+ TypeScript)) after v1.0, corpus-gated, designed-for from the start. POSIX sh and PowerShell are thin wrappers around the CLI, not independent parsers - they inherit conformance for free. Companion typed surfaces (C++/Kotlin/TypeScript) remain one core plus a veneer, not separate parsers.

## Direction decisions

The guiding tension is "simplest possible" versus "expressive enough for anything". We resolved it by fixing two audiences and moving all difficulty onto the parser:

- Optimize for the hand-authoring user and the value-consuming programmer; burden neither. Any ambiguity a modern parser can resolve from context, it must - the user is never made to satisfy the machine.
- The data model is relational, not a map. The left-of-colon token is a *field* (column), not a unique key; repeating it with different values yields *instances*. One rule covers wrappers, leaves, and valued instances: nodes are `(name, value, children)` and merge on matching `(name, value)`.
- Typing is accessor-driven: the parser stores raw text and never guesses; the consumer requests a type on read and the library coerces intelligently but safely, reporting problems without ever refusing to keep working.
- Forgiveness is a feature: never bail on a whole file for one bad line; skip/repair and diagnose; never error when a value is legitimately reachable.
- Forgiveness is also a knob, not dogma. We decided on three strictness levels - Loose/Standard/Strict, per-document, default Standard - instead of a binary strict flag. Standard keeps the defaults clean (no currency stripping, no `%` fractions, no float->int rounding, trimmed boolean set); Loose re-admits those conversions as a closed list for those who want maximum forgiving; Strict fails the load on any error diagnostic, the StrictYAML-style answer. Defaults are what adopters judge; the party tricks survive as opt-in. The normative bundle table is in `spec.md`.
- Coercion earns trust by refusing to surprise: silent lossy conversion (rounding a float on an int read, `$1200` as a number) was cut from the default level for exactly that reason. Same logic killed the fehu anti-escape rune (raw blocks are the verbatim escape hatch) and restricted field-name case folding to ASCII (full Unicode folding is a locale trap and a cross-binding parity risk).
- Raw (fenced) blocks give verbatim escape hatches (DDL, code, templates) without contorting the config syntax.
- A fence is just a value line for its parent field. We decided against a separate named/anonymous block split: the same-line spelling (`name: ~~~sql`) read badly, so the canonical spelling puts the fence on the next line at child indent - and rather than special-case that, the rule is uniform. A fence fills the parent's empty value, or creates a new instance if the parent already has one (the repeated-leaf rule). Blocks are then ordinary instances: no index-addressing syntax to invent, and the existing `[0]`/`[#N]` selectors just work.
- Positioning: the pitch is "forgiving to write, predictable to read, with the friendliest read API in the space" - not "simplest possible", which overpromises and invites the takedown. Versus schema-bearing languages (Pkl, CUE): the file stays dumb, the library is powerful (see Power layer).

Full, itemized decisions live in project memory (`shcl-spec-decisions`); `spec.md` is their normative form.

## Architecture

### Software stack

Many bindings with a shared conformance corpus (`conformance/`) as the contract between them. A key portability constraint shapes the Accessor: the requested value type is expressed by a typed entry point or a compile-time generic, never a runtime `type` field, because static languages (Go, Rust, C, C++, C#) cannot let a runtime value drive a return type. Compiled targets: Linux, BSD, macOS, Windows on x86_64 and ARM64.

### Configuration model

See `spec.md` - fields/instances, accessor-driven types, arrays vs instances, raw blocks.

### Consumer API

The consumer-facing surface is the **Accessor** (read) and the **Writer** (emit). The Accessor's one conceptual operation is a **lookup** (query) - get a value at a path, coerced to a type, with a default and an on-bad policy - realized idiomatically per language. The type is chosen by a typed entry point or compile-time generic (not a runtime field), so results land in a strongly-typed variable with no consumer cast everywhere. For consuming a file as a whole there is **traversal** (scan): materialize the document - merge duplicates, order deterministically - then walk it (wildcards, instance enumeration). The Writer is the reverse: write out defaults, values, and comments. Structured diagnostics ride alongside.

The consuming programmer is assumed to be a junior in *every* binding, not just the dynamic ones, so ergonomics are a design constraint, not an afterthought. Decided:

- Two tiers, junior-first. A convenience tier is the documented default: one value, one baked-in fallback, one return, no status to inspect. In languages with a native idiom for it, the convenience tier *is* that idiom (Rust `unwrap_or`, Python default arg, sh `--default=`). The full tier - the status-returning form - is there when the caller needs to know *why* a read failed.
- A supplied default implies default-behavior. The caller never writes a fallback *and* an explicit on-bad; explicit `Error` mode is reserved for "I want this to blow up". This kills the redundancy of asking for both.
- The convenience tier defuses the one real hazard of a forgiving accessor - a junior discarding the status and trusting a `0`/`""` that was actually empty or missing. Making the fallback mandatory and visible at the call site means an unwanted zero can't slip in unseen.
- Portability is unaffected: the type is still fixed by the entry point/generic. `default` and `onBad` are ordinary parameters and may ride in an options struct where that reads better to a beginner than functional options.

### Integration modes

One question - "how do you pull SHCL into your project?" - with two kinds of answer: *run it* or *embed it*. Named plainly and ordered easiest-first, so a beginner starts at the top:

- **Command** - run the `shcl` CLI from a shell or script. Nothing to compile, nothing to link.
- **Drop-in** - copy one source file into your project. No dependency and no build step; you own the copy.
- **Package** - add it as an ordinary dependency and let the package manager fetch it (`go get`, `pip install`, `npm i`, ...).
- **Shared library** - link the prebuilt `.so`/`.dll`/`.dylib` at runtime; the library stays a separate file.
- **Bundled** - static-link it straight into your binary, so you ship one self-contained file.

The last two are the same compiled code linked two ways - "shared" stays a separate file loaded at runtime, "bundled" is baked into your binary. Every mode reaches the same Accessor/Writer surface; the choice is packaging, not capability.

### Power layer (library-level, grammar untouched)

Compared to schema-bearing config languages (Pkl, CUE), SHCL is deliberately weaker in-language - that is the simplicity trade. To close most of the practical gap we decided the power lives in the library, never in the grammar: Pkl makes the config file powerful; SHCL keeps the file dumb and makes the library powerful. Everything below is optional - a consumer doing a bare `GetIntOr` never sees any of it - and none of it adds a rule a file author must learn.

- **Schema-as-SHCL validation.** A schema is itself a plain SHCL file describing expected paths (type, required, allowed values, ranges). One library call - `Validate(doc, schemaDoc)` - returns the same structured diagnostics loading already produces. The schema vocabulary (`int`, `required`, ...) is interpreted by the validator, not the parser, so the core language stays free of reserved words (same pattern as the fence info-string). This also closes the forgiving-parser typo hazard: the schema knows the legal field names, so unknown/misspelled fields get caught ("unknown field `enabeld`, did you mean `enabled`?").
- **Layered loading.** `Load(defaults, site, user, ...)` merges later files over earlier ones using the merge rule the language already defines (nodes merge on matching `(field-name, value)`); layering is the existing rule applied across files. CLI/env overrides (`--set a.b=v`, env-var mapping) sit on top as the final layer. Covers the defaults-plus-overrides composition story without in-language imports.
- **Schema-driven generation.** The Writer plus a schema yields a fully commented, correctly typed starter config (`shcl init --schema ...`). Composition of two things already specified - the Writer's "emit defaults and comments" and the schema above.

Explicitly out of scope, with finality unless something big changes: in-language expressions, functions, inheritance, interpolation, imports, anchors/references. The moment config files can compute, they need debugging - that is the complexity cliff we are staying off of.

### Formatter

Structure-only canonicalizer: block form, tabs, insertion order, minimal quoting, redundancy collapsed, value text untouched (it cannot know types).

### Testing

The conformance corpus is the primary cross-language guarantee: each case is an input, its canonical formatting, and expected typed reads with status sentinels. Every parser runs it in CI.

Passing the corpus independently is necessary but not sufficient once there is more than one binding: two implementations can each satisfy the expectations yet still disagree on the details the corpus never pinned (float rendering, diagnostic-free edge behavior). So the pipeline also runs a differential check: every binding's CLI is replayed over the same inputs - the whole corpus plus a freshly fuzz-generated input set - and all bindings must agree with the reference byte for byte on stdout and exit code. It went live when the Go binding landed: rust is the reference, go is compared against it on every run. stderr is deliberately outside the contract (diagnostic wording and OS error text are per-binding voice); stdout and exit codes are the contract.

Two portability rules fell out of making the first port agree byte for byte, and now bind every future binding: floats render as shortest round-trip decimal, never scientific notation (the reference's native float formatting; Go had to opt into it explicitly), and diagnostic order must be deterministic - the reference's repeated-leaf hints originally grouped via a randomized-order hash map and were fixed to first-appearance order, since a port can match a rule but not a coin flip.

### CI/CD

We decided to split by responsibility rather than duplicate the pipeline:

- The GitHub workflow (`.github/workflows/ci.yml`) is a correctness gate only - format check, build, lint, tests on push/PR. Minimal permissions, cancels superseded runs, times out.
- Everything else (cross-compile, packaging, publish) stays in the local pipeline, `cicd/cicd.bash`, config-driven via `cicd/config.bash`.
- Both share one definition of "passing": the workflow just runs `cicd.bash --ci`. Per-language toolchain setup lives in the workflow YAML; what passing means lives in the engine, so the two cannot drift.
- The formatter rewrites in place locally but is check-only (fail on diff) in CI.
- Branch flow: `dev` is the integration target (feature branches merge there, `--no-ff`); `main` is release-only. A dev -> main merge is a release cut.
- One canonical version source: `source/rust/Cargo.toml`. The pipeline reads it for artifact names and release tags. (An automatic bump-before-push guard was tried and dropped: dev is the integration branch, and versions there are cut deliberately at release time, not policed per push.)
- Toolchain pins: `rust-toolchain.toml` (rustc + clippy + cross targets) and warn-only pins for cargo-installed helpers, so a box update cannot silently change results.
- Fuzzing is in the regression suite, not a separate rig: a deterministic, seed-fixed mutator over the corpus inputs asserts two invariants for any input - never panic at any strictness, and the canonical formatter is a fixpoint. It found three real formatter bugs before first release. The same mutator doubles as the input generator for the cross-binding differential check (see Testing).
- Profiling is a standing pipeline stage, not an occasional exercise: every full run samples an optimized-with-symbols build over a heavy parse/format workload and emits a flamegraph SVG (rotated like the run logs) plus a hot-spot summary, so a performance regression shows up in the artifacts the run it happens. Kernel perf is locked down on the build box, so sampling is in-process via a feature-gated dev-only dependency that never reaches a shipped binary.

### Reference implementation

- Rust crate at `source/rust/`, zero dependencies, single-file library (`src/lib.rs`) so the drop-in integration mode stays honest; the `shcl` CLI builds from the same crate. Later bindings get sibling folders (`source/go/`, ...).
- The conformance runner and fuzz smoke are plain `cargo test` targets, so "the corpus passes" and "the build passes" are the same command everywhere.

### Go binding (Tier 2)

- Module at `source/go/`: single-file library (`shcl.go`, zero dependencies, generics for the typed reads) plus the CLI under `cmd/shcl/` - same flags, output, and exit codes as the reference.
- Conformance runs natively as `go test` (a port of the Rust runner over the same corpus), so the Go binding is corpus-green on its own, and the cicd crosscheck holds it byte-for-byte to the reference besides.
- The pipeline stays engine-generic: the Go fmt/build/vet/test commands ride the config's per-stage extras, and the binding registers in `BINDING_CLIS` - a pattern each further binding (C, Python) repeats without engine changes.

### C binding (Tier 2)

- Sources at `source/c/`: a single-header drop-in library (`shcl.h`, C11, zero dependencies) plus the CLI under `cmd/shcl/` - same flags, output, and exit codes as the reference. The single-header story is the C analog of the other bindings' single-file libraries: copy `shcl.h` into a tree and, in one translation unit, `#define SHCL_IMPLEMENTATION` before including it.
- The C++ typed surface (`shcl.hpp`) is a veneer, not a second parser: a header of `Read<T>` / `get<T>()` templates over the same C functions, so it inherits the core's conformance and only needs a compile-plus-behavior smoke to keep it honest.
- Conformance runs natively (a C port of the runner over the same corpus), so the C binding is corpus-green on its own, and the cicd crosscheck holds it byte-for-byte to the reference besides.
- Memory is a per-document bump arena whose growable vectors grow by copy, so teardown is a single free with no per-object bookkeeping - a short-lived-tool trade that keeps the port readable. Two portability details the reference implies for free but C must make explicit: byte strings are length-delimited with codepoint-accurate UTF-8 iteration (the reference works in Unicode scalars, so plain byte scanning would mishandle multibyte text and the backslash-shields-next rule), and float output is reproduced by taking the smallest printf precision that round-trips and reflowing it into fixed notation, matching the reference's shortest-decimal-never-scientific rule. C has no committed zero-dependency formatter, so its quality gate is a `-Wall -Wextra -Werror` compile rather than a separate fmt/lint stage.

### Shell wrapper

- The shell binding is a wrapper around the `shcl` CLI, not a parser, so it inherits conformance for free. Given the choice between a thin POSIX `sh` and a fuller Bash, we chose Bash (`source/bash/shcl.bash`): the wrapper earns its keep only by being dual-purpose - runnable as a script *or* sourced so a caller invokes functions - and that dual mode plus the ergonomic sourced surface reads far cleaner with Bash's `BASH_SOURCE`, `local`, and arrays than with portable `sh`. A truly thin passthrough would give a sourcing caller nothing over calling the binary directly; the typed helpers are the reason to source.
- One `shcl` function is the whole CLI; `shcl_get`/`shcl_int`/`shcl_bool`/`shcl_array`/... are one-line typed sugar over it. Script mode and function mode take the same arguments and hand back the binary's exit code unchanged, so a not-found or empty read stays a distinct nonzero rather than collapsing to a generic failure.
- Two things a sourced tool must not do, and doesn't: leak shell options into the caller (strict mode is armed on the run path only, and `-e` is deliberately off there because a nonzero read is normal, not a fault), and let its own `shcl` function shadow the binary during lookup (`type -P` searches PATH for the executable, ignoring the function). The binary is found via `$SHCL_BIN`, then a co-located `shcl`, then PATH, then the repo build - so the dogfooded install (wrapper beside binary) and in-repo dev both resolve without configuration. shellcheck is the quality gate, wired through the same cicd target list as the pipeline's own scripts.
