<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
# Design

Design, requirements, and direction. The pre-v1.0.0 task list is in `backlog.md`. The full language definition is in `spec.md` (with `grammar.abnf`); this file stays high-level - the *why*, not the letter of the rules.

## Assumptions

- The raw, by-example origin of the language is `../../notes.txt`. It was rationalized into a coherent model through a decision pass; where the two disagree, `spec.md` wins.
- The language ships as many bindings that must behave identically - Go, Rust, C (+ C++), C#, Java (+ Kotlin), Python, JavaScript (+ TypeScript), PowerShell, POSIX sh - plus single-file drop-in source and compiled binaries per platform. A smaller set lands first; the rest are designed-for from the start. Three of these are one core plus a thin companion typed surface, not separate parsers: C++ is a template header over the C core, Kotlin is generic extensions over the Java core, TypeScript is a `.d.ts` over the JS core.

## Direction decisions

The guiding tension is "simplest possible" versus "expressive enough for anything". We resolved it by fixing two audiences and moving all difficulty onto the parser:

- Optimize for the hand-authoring user and the value-consuming programmer; burden neither. Any ambiguity a modern parser can resolve from context, it must - the user is never made to satisfy the machine.
- The data model is relational, not a map. The left-of-colon token is a *field* (column), not a unique key; repeating it with different values yields *instances* (rows). One rule covers wrappers, leaves, and valued instances: nodes are `(name, value, children)` and merge on matching `(name, value)`.
- Typing is accessor-driven: the parser stores raw text and never guesses; the consumer requests a type on read and the library coerces intelligently but safely, reporting problems without ever refusing to keep working.
- Forgiveness is a feature: never bail on a whole file for one bad line; skip/repair and diagnose; never error when a value is legitimately reachable.
- Raw (fenced) blocks give verbatim escape hatches (DDL, code, templates) without contorting the config syntax.

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

### Formatter

Structure-only canonicalizer: block form, tabs, insertion order, minimal quoting, redundancy collapsed, value text untouched (it cannot know types).

### Testing

The conformance corpus is the primary cross-language guarantee: each case is an input, its canonical formatting, and expected typed reads with status sentinels. Every parser runs it in CI.

### CI/CD

We decided to split by responsibility rather than duplicate the pipeline:

- The GitHub workflow (`.github/workflows/ci.yml`) is a correctness gate only - format check, build, lint, tests on push/PR. Minimal permissions, cancels superseded runs, times out.
- Everything else (cross-compile, packaging, publish) stays in the local pipeline, `cicd/cicd.bash`, config-driven via `cicd/config.bash`.
- Both share one definition of "passing": the workflow just runs `cicd.bash --ci`. Per-language toolchain setup lives in the workflow YAML; what passing means lives in the engine, so the two cannot drift.
- The formatter rewrites in place locally but is check-only (fail on diff) in CI.
