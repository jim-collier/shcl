<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
# Design

Design, requirements, and direction. The pre-v1.0.0 task list is in `backlog.md`. The full language definition is in `spec.md` (with `grammar.abnf`); this file stays high-level - the *why*, not the letter of the rules.

<!-- TOC ignore:true -->
## Table of contents
<!-- TOC -->

- [Assumptions](#assumptions)
- [Guiding principles and decisions](#guiding-principles-and-decisions)
- [Architecture](#architecture)
	- [Software stack](#software-stack)
	- [Configuration model](#configuration-model)
	- [Consumer API](#consumer-api)
	- [Integration modes](#integration-modes)
	- [Power layer library-level, grammar untouched](#power-layer-library-level-grammar-untouched)
	- [Schema validation](#schema-validation)
	- [Formatter](#formatter)
	- [Testing](#testing)
	- [CI/CD](#cicd)
	- [Reference implementation](#reference-implementation)
	- [Go binding Tier 2](#go-binding-tier-2)
	- [C binding Tier 2](#c-binding-tier-2)
	- [Shell wrappers](#shell-wrappers)

<!-- /TOC -->

## Assumptions

- The language began by example in `../../notes.txt`. Where the two disagree (and they do), `spec.md` wins.

- The language ships as tiered bindings, plus single-file drop-in source and compiled binaries per platform.

	- The guarantee is the corpus, not the binding count. Every shipped binding is corpus-green, and nothing ships before it is.

	- Tier 1: the Rust reference and the `shcl` CLI built from it. Rust wins on the stated priorities: small binary, instant CLI startup, a clean shared-library build, and compile-time strictness that forces spec precision.

	- Tier 2: Go, C (with a C++ veneer), Python.

	- Tier 3: the rest (C#, Java with Kotlin, JavaScript with TypeScript), after v1.0, corpus-gated, designed for from the start.

	- POSIX sh and PowerShell are thin wrappers around the CLI, not independent parsers. They inherit conformance for free. The companion typed surfaces (C++, Kotlin, TypeScript) are one core plus a veneer, not separate parsers.

## Guiding principles and decisions

The inescapable core tradeoff (for any config language) is to acknowledge and  optimally balance "simplest possible" versus "expressive enough for anything" - including a simple DDL language. Ultimately it comes down to two factors:

1. Ideologically-driven human opinion on where the balance should be - informed by technical experience, related PTSD gained along the way, childhood trauma - all of it.

1. The observation that processing power is now dirt-cheap even on the smallest embedded systems, validation and "correctness" tools are now incredibly powerful - and the subsequent decision to move as much of the heavy-lifting of the language on to this code, and away from end users and programmers.

Other points

- Optimize for the hand-authoring user and the value-consuming programmer; burden neither. Any ambiguity a modern parser can resolve from context, it must - the user is never made to satisfy the machine.

- The data model is relational, not a map. The left-of-colon token is a *field* (column), not a unique key; repeating it with different values yields *instances*. One rule covers wrappers, leaves, and valued instances: nodes are `(name, value, children)` and merge on matching `(name, value)`.

- Typing is accessor-driven: the parser stores raw text and never guesses; the consumer requests a type on read and the library coerces intelligently but safely, reporting problems without ever refusing to keep working.

- Forgiveness is a feature: never bail on a whole file for one bad line; skip/repair and diagnose; never error when a value is legitimately reachable.

- Forgiveness is also a knob, not dogma. There are three strictness levels - Loose/Standard/Strict, per-document, default Standard - instead of a binary strict flag. Standard keeps the defaults clean (no currency stripping, no `%` fractions, no float->int rounding, trimmed boolean set); Loose re-admits those conversions as a closed list for those who want maximum forgiving; Strict fails the load on any error diagnostic, the StrictYAML-style answer. Defaults are what adopters judge; the party tricks survive as opt-in. The normative bundle table is in `spec.md`.

- The knob belongs to the consuming program, not to the end user. Strictness and bad-read handling are part of the contract an application makes about its own config handling, so they are set at the call site (or the CLI flag) and nowhere else. A per-user defaults file was considered and rejected: it would let a user silently weaken guarantees an app relies on, and would make an identical `shcl` invocation mean different things on different machines - the `GREP_OPTIONS` mistake. Nothing else the CLI exposes is presentation-only, so SHCL ships no user config file at all.

- Coercion earns trust by refusing to surprise: silent lossy conversion (rounding a float on an int read, `$1200` as a number) was cut from the default level for exactly that reason. Same logic killed the fehu anti-escape rune (raw blocks are the verbatim escape hatch) and restricted field-name case folding to ASCII (full Unicode folding is a locale trap and a cross-binding parity risk).

- Raw (fenced) blocks give verbatim escape hatches (DDL, code, templates) without contorting the config syntax.

- A fence is just a value line for its parent field. The same-line spelling (`name: ~~~sql`) read badly, so the canonical spelling puts the fence on the next line at child indent - and rather than special-case that, the rule is uniform. A fence fills the parent's empty value, or creates a new instance if the parent already has one (the repeated-leaf rule). Blocks are then ordinary instances: no index-addressing syntax to invent, and the existing `[0]`/`[#N]` selectors just work.

- Parity over idiom in the bindings: each port deliberately mirrors the reference's structure - same function inventory, same call flow, same contract strings - even where the host language would idiomatically do it differently. Byte-for-byte agreement is the product's core guarantee, and structural parallelism is what makes it maintainable: a fix ports by mechanical diff instead of re-derivation. Per-language idiom keeps the surface (formatters, naming case, doc comments); it yields on structure. The full rules and each accepted deviation live in `../style-guide.md`.

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

Compared to schema-bearing config languages (Pkl, CUE), SHCL is deliberately weaker in-language - that is the simplicity trade. To close most of the practical gap - the power lives in the library, never in the grammar: Pkl makes the config file powerful; SHCL keeps the file dumb and makes the library powerful. Everything below is optional - a consumer doing a bare `GetIntOr` never sees any of it - and none of it adds a rule a file author must learn.

- **Schema-as-SHCL validation.** A schema is itself a plain SHCL file describing expected paths (type, required, allowed values, ranges). One library call - `Validate(doc, schemaDoc)` - returns the same structured diagnostics loading already produces. The schema vocabulary (`int`, `required`, ...) is interpreted by the validator, not the parser, so the core language stays free of reserved words (same pattern as the fence info-string). This also closes the forgiving-parser typo hazard: the schema knows the legal field names, so unknown/misspelled fields get caught ("unknown field `enabeld`, did you mean `enabled`?"). Design below.

- **Layered loading.** (Implemented; see spec.md "Layered loading".) `Load(defaults, site, user, ...)` is a left fold of `merge(base, over)` across files. Container instances merge on matching `(field-name, value)` - the existing in-file rule - but a leaf name in a higher layer *overrides* (replaces) the lower layer's same-named leaves, so defaults-plus-override works for scalars, arrays, and raw blocks, not just structure. CLI `--set PATH=VALUE` sits on top as the final layer, written as literal text through the Writer. Env-var mapping was deliberately dropped: the env namespace and its convention belong to the consuming program, which can map env onto `--set` itself (the same reasoning that canceled the per-user config file). `check` takes no layers - its diagnostics are single-file.

- **Schema-driven generation.** (Implemented; see spec.md "Schema-driven generation".) `generate(schema)` + `shcl init --schema` emit a commented, typed starter config: `desc` becomes a comment, a generated annotation line summarizes type/constraints, required fields are live (their `default` or an empty value), optional fields are the same line commented out, and wildcard paths go in a trailing comment block. Output is flat dotted form (mirrors the schema shape) and always loads clean. The annotation line is a byte-for-byte cross-binding contract, so its format is fixed and its numbers use the canonical formatters.

Explicitly out of scope, with finality unless something big changes: in-language expressions, functions, inheritance, interpolation, imports, anchors/references. The moment config files can compute, they need debugging - that is the complexity cliff to avoid.

### Schema validation

The schema is a plain SHCL file, read with the ordinary parser and the ordinary Accessor. No grammar change, no reserved words, no new parser feature - the whole design was prototyped against the shipped binary before being written down.

**Shape: a flat list of path descriptions, not a mirror of the document.** Each constraint is one instance of a field named `field`, whose *value* is the path it describes and whose children are the constraints:

```shcl
field: server.port
	type: int
	required: yes
	min: 1
	max: 65535

field: "server[*].host"
	type: string
```

The alternative - a schema that mirrors the document's tree, with constraints as children of each leaf - was rejected: it cannot tell a constraint named `type` from a real document field named `type`, so the schema vocabulary would collide with the user's namespace. The flat form has no such ambiguity, because the document's paths appear as *values*, never as field names. It also falls straight out of the relational model the language is already built on - `field` is the column, each path is a row - and reads as an ordinary SHCL file to someone who has never seen a schema.

Consequences of the flat form, all verified against the current binary:

- The validator needs no new lookup machinery: `Instances("field")` enumerates the described paths, and `field[<path>].type` reads a constraint.

- A path containing a bracket selector must be quoted (`field: "server[*].host"`), because a bare selector's scan ends at the first `]`. The canonical form writes the path as a value rather than a selector, so the formatter applies that quoting itself.

- A schema file is a formatter fixpoint like any other SHCL document, so `shcl fmt` works on schemas for free.

**Wildcards carry the repeated-instance story.** `server[*].port` constrains every instance of `server`, which is how a schema says "each server needs a port" in a language whose core idea is repeated instances. `repeat` (on the parent path) bounds the instance count itself - the one constraint with no equivalent in tree-shaped schema languages.

**The vocabulary stays small and closed**, in the same spirit as the Loose coercion list: `type`, `required`, `allowed` (an enum, written as an ordinary array), `min`/`max` (numeric ranges, inclusive), `repeat`, plus `default` and `desc` which only the generator reads. Nothing joins that list without a spec change. Datetime ranges were considered and dropped for the same reason as regex below: comparing datetimes across zone suffixes needs calendar arithmetic (an offset can roll the date), no parser has any, and four hand-written implementations of it is a parity minefield.

**Regular-expression constraints are rejected outright**, and this is the one real capability given up. No two of the target languages share a regex engine - character classes, Unicode properties, and anchoring all differ - so a `pattern` key could not hold byte-for-byte agreement across bindings, which is the product's core guarantee. An enum covers the common case; anything past that belongs in the consuming program.

**Validation reuses the reads, so strictness composes automatically.** `type: int` against `3.5` passes at Loose (which rounds) and fails at Standard. That falls out of the validator calling the same typed reads a consumer would, and it is the correct behavior: the schema says what the program needs, and strictness says how forgiving that program is about getting it.

**Diagnostics reuse the existing structure** (line, severity, stable code, prose message), so a consumer inspects validation results exactly as it inspects load results. Two additions:

- A `V###` code range, disjoint from the parser's `E###`/`H###`, so a validation failure is never confused with a parse failure. Unknown field, missing required, wrong type, value outside the allowed set, out of range, and wrong instance count each get one.

- Line 0 means "no line" - the document-scope report a missing-required-field diagnostic needs, since the whole point is that nothing was written.

The "did you mean `enabled`?" suggestion rides in the prose message, not the code. Edit-distance implementations would otherwise have to agree byte-for-byte across four bindings for a string that is explicitly per-binding voice.

**A broken schema is reported against the schema.** Codes `V090+` cover schema faults (unknown constraint key, unusable type name), their line numbers refer to the schema file, and their presence suppresses data validation entirely - there is nothing meaningful to say about a document checked against a schema that does not parse. This keeps one line-number space per result set without adding a source field to the shared `Diagnostic` type across every binding.

**CLI surface: `shcl check --schema SCHEMA FILE`**, rather than a new subcommand. Loading and validating are the same question ("is this file good?"), the output shape and exit codes are already defined by `check`, and folding it in avoids a second nearly identical command.

Both open points are settled:

- `Validate` lives in each binding's single drop-in file. The one-file promise the README leads with outranks keeping the core lean; a schema user copying two files would quietly break the pitch.

- No string/array length bounds. They would reopen the byte-versus-code-point metric already settled for merge keys, for modest benefit; an `allowed` enum or the consuming program covers the need.

### Formatter

Structure-only canonicalizer: block form, tabs, insertion order, minimal quoting, redundancy collapsed, value text untouched (it cannot know types).

### Testing

The conformance corpus is the primary cross-language guarantee. Each case is an input, its canonical formatting, and the expected typed reads with status sentinels. Every parser runs it in CI.

Passing the corpus independently is necessary but not sufficient once there is more than one binding. Two implementations can each satisfy the expectations yet still disagree on details the corpus never pinned, like float rendering or diagnostic-free edge behavior. So the pipeline also runs a differential check:

- Every binding's CLI is replayed over the same inputs: the whole corpus plus a freshly generated fuzz set.

- All bindings must agree with the reference byte-for-byte on stdout and exit code. The reference is Rust.

- stderr is deliberately outside the contract. Diagnostic wording and OS error text are per-binding voice. stdout and exit codes are the contract.

Two portability rules bind every binding:

- Floats render as shortest round-trip decimal, never scientific notation. This matches the reference's native float formatting.

- Diagnostic order must be deterministic, in first-appearance order. A port can match a rule, but not a coin flip.

### CI/CD

The responsibility is split rather than duplicate the pipeline:

- The GitHub workflow (`.github/workflows/ci.yml`) is a correctness gate only - format check, build, lint, tests on push/PR. Minimal permissions, cancels superseded runs, times out.

- Everything else (cross-compile, packaging, publish) stays in the local pipeline, `cicd/cicd.bash`, config-driven via `cicd/config.bash`.

- Both share one definition of "passing": the workflow just runs `cicd.bash --ci`. Per-language toolchain setup lives in the workflow YAML; what passing means lives in the engine, so the two cannot drift.

- The formatter rewrites in place locally but is check-only (fail on diff) in CI.

- Branch flow: `dev` is the integration target (feature branches merge there, `--no-ff`); `main` is release-only. A dev -> main merge is normally a release cut.
	- The exception is a merge that changes no product code - documentation, the demo asset, the pipeline. Those go to `main` on their own so the front page and the install one-liners (which read from `main`) stay current, and the version stays where it is. Cutting a tag for them would publish a second set of binaries that behave identically to the last one, which tells a reader nothing.

- One canonical version source: `source/rust/Cargo.toml`. The pipeline reads it for artifact names and release tags. (An automatic bump-before-push guard was tried and dropped: dev is the integration branch, and versions there are cut deliberately at release time, not policed per push.)
	- Release cut checklist: bump the five version sites (Cargo.toml canonical, the Go/Python/C CLI mirrors, and `source/python/pyproject.toml` for PyPI), date the changelog heading, and pass the README status once - lifecycle badge, Status section, and Installing section must match the release being cut (they drifted to "no tagged release" after beta1). Sign the sums file (`cicd/utility/sign-release.bash --key ...`) and attach everything in `cicd/artifacts/release/` to the GitHub release: raw binaries, the .deb/.rpm and NSIS setup packages, the sha256sums file that covers them all, and its `.sig`.
	- Two tags per cut, not one: `vX.Y.Z` for the release, and `source/go/vX.Y.Z` for the Go module. Go resolves a module in a subdirectory only through a path-prefixed tag, so skipping the second one silently strands every Go consumer on a pseudo-version. Then publish the registry packages: `cargo publish` from `source/rust/`, and a built sdist/wheel from `source/python/`.
		- Publish through the rustup `cargo`, not whatever `cargo` the shell finds first. A distro-packaged Rust ahead of `~/.cargo/bin` ignores `rust-toolchain.toml` silently, so the crate gets built by an unpinned compiler. `cicd.bash` prepends the right directory; a hand-run `cargo publish` does not.
		- Let both tools prompt for the token rather than passing it on a command line or exporting it. An argv token is readable by other users via the process list while it runs, and both forms persist in shell history - including the searchable database that history-sync tools keep. Registry tokens should also be project-scoped, which each registry only offers once the package exists.
	- A registry publish cannot be taken back. Neither crates.io nor PyPI allows reuploading a version, and each bakes in the README it was given at that moment - so the per-binding READMEs (`source/rust/README.md`, `source/python/README.md`) get their example code compiled and run against the packaged artifact before the upload, not after. Those files exist separately from the front-page README because relative links and images break on a package page, and Cargo cannot reach above the crate root anyway.
	- Binding versions move in lockstep with the product version, deliberately. The bindings are byte-for-byte equivalent by design, so a single number across all of them means a consumer reading `1.4` in any language knows exactly what behavior they have. It also lets each ecosystem's ordinary compatible-version operator (`shcl = "1"`, `shcl~=1.0`, Go's major-version import rule) do the tracking, with no scheme of our own to explain or maintain.

- Installer packages ride the release stage, not a separate pipeline: `cicd/utility/package.bash` builds .deb/.rpm (nfpm, one sed-rendered template) per Linux binary and an NSIS setup per Windows binary, into the same versioned artifact family before the checksums are written. Package layout follows distro convention (/usr/bin + /usr/share/shcl) rather than the /opt layout the standalone install.bash uses - packages answer to distro policy, the script answers to the spec. Payload matches install.bash: binary + code/ drop-ins + scripts/ wrappers.

- Release trust root: the sha256sums file is signed offline with an RSA-4096 key, and both installers carry the public half inlined and check it before reading a checksum out of the file. Order matters more than the algorithm - a checksum taken from an unverified sums file proves nothing, so the signature is checked first or not at all. The threat this addresses is release-asset replacement (a leaked token with release scope, a compromised CI job), not full repo compromise: an attacker who can rewrite `install.bash` on `main` can also delete the check. Those are different access paths, and separating them is the point. Decisions behind it:
	- The key is offline and signing is manual. A key in CI secrets would be reachable by exactly the compromise being defended against, which would make the signature decorative.
	- The key is inlined, not fetched. A key downloaded over the same channel as the artifact authenticates nothing.
	- RSA, not the more fashionable Ed25519, purely on verifier availability: `openssl` covers Unix, and .NET's RSA covers Windows PowerShell 5.1, where Ed25519 is absent. Any scheme needing a tool the box does not already have (minisign, cosign, gpg) loses to the bootstrap problem - verifying the verifier.
	- `openssl` became a hard prerequisite of `install.bash` alongside curl/wget. Installing unverified is not offered as a fallback; the DIY path is there for a box that genuinely lacks it.
	- Rotation is expensive by construction, since old installers carry the old key. Treat a key change as a breaking change: new installer, and a release note.
	- The Go binding gets tamper-evidence free from `sum.golang.org`, so this covers the binaries and drop-in payload, which have no equivalent backstop.

- Toolchain pins: `rust-toolchain.toml` (rustc + clippy + cross targets) and warn-only pins for cargo-installed helpers, so a box update cannot silently change results.

- Fuzzing lives in the regression suite, not a separate rig. A deterministic mutator over the corpus asserts two invariants for any input: never panic at any strictness, and the formatter is a fixpoint. The same mutator generates the inputs for the differential check above.

- Profiling is a standing stage. Every full run samples an optimized build over a heavy parse-and-format workload and emits a flamegraph plus a hot-spot summary, so a performance regression shows up in the artifacts the run it happens in. Sampling is in-process, feature-gated, and never reaches a shipped binary.

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

- Memory is a per-document bump arena, so teardown is a single free with no per-object bookkeeping. A short-lived-tool trade that keeps the port readable.

- Two portability details the reference gets for free but C makes explicit: UTF-8 is iterated by codepoint (plain byte scanning would mishandle multibyte text), and float output reproduces the reference's shortest-decimal, never-scientific rule.

- C has no committed zero-dependency formatter, so its quality gate is a warning-clean compile rather than a separate format stage.

### Shell wrappers

- The shell binding wraps the `shcl` CLI, not a parser, so it inherits conformance for free.

- Bash 3.2 (`source/bash/shcl.bash`) was targeted rather than POSIX sh (mostly defined in 1979). The wrapper earns its keep by being dual-purpose: run it as a script, or source it and call functions. That dual mode and the typed helpers read far cleaner with Bash's arrays and `local` than with portable sh. A thin passthrough would give a sourcing caller nothing over the binary itself.

- One `shcl` function is the whole CLI. `shcl_get`, `shcl_int`, `shcl_bool`, and friends are one-line typed sugar. Both modes take the same arguments and return the binary's exit code unchanged, so a not-found or empty read stays a distinct nonzero.

- Two things a sourced tool must not do, and doesn't:

	- Leak shell options into the caller. Strict mode is armed only on the run path.

	- Let its own `shcl` function shadow the binary during lookup. The binary is resolved via `$SHCL_BIN`, a co-located `shcl`, PATH, then the repo build, so a dogfooded install and in-repo dev both work without configuration.
