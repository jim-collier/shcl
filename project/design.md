<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
# Design

Design, requirements, and direction. The pre-v1.0.0 task list is in `backlog.md`. The full language definition is in `spec.md` (with `grammar.abnf`); this file stays high-level - the *why*, not the letter of the rules.

## Assumptions

- The language began by example in `../../notes.txt`. A decision pass rationalized it into a coherent model. Where the two disagree, `spec.md` wins.

- The language ships as tiered bindings, plus single-file drop-in source and compiled binaries per platform.

	- The guarantee is the corpus, not the binding count. Every shipped binding is corpus-green, and nothing ships before it is.

	- Tier 1: the Rust reference and the `shcl` CLI built from it. Rust wins on the stated priorities: small binary, instant CLI startup, a clean shared-library build, and compile-time strictness that forces spec precision.

	- Tier 2: Go, C (with a C++ veneer), Python.

	- Tier 3: the rest (C#, Java with Kotlin, JavaScript with TypeScript), after v1.0, corpus-gated, designed for from the start.

	- POSIX sh and PowerShell are thin wrappers around the CLI, not independent parsers. They inherit conformance for free. The companion typed surfaces (C++, Kotlin, TypeScript) are one core plus a veneer, not separate parsers.

## Direction decisions

The guiding tension is acknowledging "simplest possible" versus "expressive enough for anything". The resolution is identifying the two audiences and moving all difficulty onto the parser:

- Optimize for the hand-authoring user and the value-consuming programmer; burden neither. Any ambiguity a modern parser can resolve from context, it must - the user is never made to satisfy the machine.

- The data model is relational, not a map. The left-of-colon token is a *field* (column), not a unique key; repeating it with different values yields *instances*. One rule covers wrappers, leaves, and valued instances: nodes are `(name, value, children)` and merge on matching `(name, value)`.

- Typing is accessor-driven: the parser stores raw text and never guesses; the consumer requests a type on read and the library coerces intelligently but safely, reporting problems without ever refusing to keep working.

- Forgiveness is a feature: never bail on a whole file for one bad line; skip/repair and diagnose; never error when a value is legitimately reachable.

- Forgiveness is also a knob, not dogma. There are three strictness levels - Loose/Standard/Strict, per-document, default Standard - instead of a binary strict flag. Standard keeps the defaults clean (no currency stripping, no `%` fractions, no float->int rounding, trimmed boolean set); Loose re-admits those conversions as a closed list for those who want maximum forgiving; Strict fails the load on any error diagnostic, the StrictYAML-style answer. Defaults are what adopters judge; the party tricks survive as opt-in. The normative bundle table is in `spec.md`.

- Coercion earns trust by refusing to surprise: silent lossy conversion (rounding a float on an int read, `$1200` as a number) was cut from the default level for exactly that reason. Same logic killed the fehu anti-escape rune (raw blocks are the verbatim escape hatch) and restricted field-name case folding to ASCII (full Unicode folding is a locale trap and a cross-binding parity risk).

- Raw (fenced) blocks give verbatim escape hatches (DDL, code, templates) without contorting the config syntax.

- A fence is just a value line for its parent field. The same-line spelling (`name: ~~~sql`) read badly, so the canonical spelling puts the fence on the next line at child indent - and rather than special-case that, the rule is uniform. A fence fills the parent's empty value, or creates a new instance if the parent already has one (the repeated-leaf rule). Blocks are then ordinary instances: no index-addressing syntax to invent, and the existing `[0]`/`[#N]` selectors just work.

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

- **Schema-as-SHCL validation.** A schema is itself a plain SHCL file describing expected paths (type, required, allowed values, ranges). One library call - `Validate(doc, schemaDoc)` - returns the same structured diagnostics loading already produces. The schema vocabulary (`int`, `required`, ...) is interpreted by the validator, not the parser, so the core language stays free of reserved words (same pattern as the fence info-string). This also closes the forgiving-parser typo hazard: the schema knows the legal field names, so unknown/misspelled fields get caught ("unknown field `enabeld`, did you mean `enabled`?").

- **Layered loading.** `Load(defaults, site, user, ...)` merges later files over earlier ones using the merge rule the language already defines (nodes merge on matching `(field-name, value)`); layering is the existing rule applied across files. CLI/env overrides (`--set a.b=v`, env-var mapping) sit on top as the final layer. Covers the defaults-plus-overrides composition story without in-language imports.

- **Schema-driven generation.** The Writer plus a schema yields a fully commented, correctly typed starter config (`shcl init --schema ...`). Composition of two things already specified - the Writer's "emit defaults and comments" and the schema above.

Explicitly out of scope, with finality unless something big changes: in-language expressions, functions, inheritance, interpolation, imports, anchors/references. The moment config files can compute, they need debugging - that is the complexity cliff to avoid.

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

- Branch flow: `dev` is the integration target (feature branches merge there, `--no-ff`); `main` is release-only. A dev -> main merge is a release cut.

- One canonical version source: `source/rust/Cargo.toml`. The pipeline reads it for artifact names and release tags. (An automatic bump-before-push guard was tried and dropped: dev is the integration branch, and versions there are cut deliberately at release time, not policed per push.)
	- Release cut checklist: bump the four CLI version sites (Cargo.toml canonical, Go/Python/C mirrors), date the changelog heading, and pass the README status once - lifecycle badge, Status section, and Installing section must match the release being cut (they drifted to "no tagged release" after beta1).

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

### Shell wrapper

- The shell binding wraps the `shcl` CLI, not a parser, so it inherits conformance for free.

- Bash 3.2 (`source/bash/shcl.bash`) was targeted rather than POSIX sh (mostly defined in 1979). The wrapper earns its keep by being dual-purpose: run it as a script, or source it and call functions. That dual mode and the typed helpers read far cleaner with Bash's arrays and `local` than with portable sh. A thin passthrough would give a sourcing caller nothing over the binary itself.

- One `shcl` function is the whole CLI. `shcl_get`, `shcl_int`, `shcl_bool`, and friends are one-line typed sugar. Both modes take the same arguments and return the binary's exit code unchanged, so a not-found or empty read stays a distinct nonzero.

- Two things a sourced tool must not do, and doesn't:

	- Leak shell options into the caller. Strict mode is armed only on the run path.

	- Let its own `shcl` function shadow the binary during lookup. The binary is resolved via `$SHCL_BIN`, a co-located `shcl`, PATH, then the repo build, so a dogfooded install and in-repo dev both work without configuration.

## Code Review 20260716

Technical detail behind the backlog's "Code Review 20260716" items. Item numbers match the backlog; items whose backlog bullet says everything needed are not repeated here.

- **Item 1 - C CLI use-after-free** (`source/c/cmd/shcl/main.c:127`)
	- `PUSHLINE_FMT`/`PUSHLINE_BUF` store `lines[n].p = lines[n].own` - a pointer into the `lines` array itself. When the array grows (realloc at 8 entries, then each doubling), every stored `.p` still points into the freed block; the print loop reads it.
	- Only formatted entries are affected, so the trigger is exactly int/float/datetime array output with 9+ elements. String/bool/raw entries borrow arena or static memory and are safe.
	- Fix: stop storing a self-pointer - set `.p = NULL` for owned entries and pick `own` vs `p` at print time via the existing `owned` flag; or format into arena buffers and drop `own[]` entirely. Add a 9+-element typed-array corpus row so all bindings pin it.

- **Item 2 - datetime timezone panic** (`source/rust/src/lib.rs:1485`)
	- `parse_time_part` checks `is_char_boundary` only at the start of the 6-byte zone tail, then slices `tail[1..3]` and `tail[4..6]`. A `+`/`-` followed by a multibyte char (e.g. U+20AC) lands the slice mid-character: panic, exit 134.
	- Fix: validate the tail as bytes (`is_ascii_digit` on positions 1,2,4,5 and `b':'` at 3) before any str slicing; byte indexing cannot panic. Add a fuzz/corpus case with a multibyte char in the last 6 bytes of a time-shaped value, mirrored to all ports.

- **Item 3 - wildcard per-slot status** (`source/rust/src/lib.rs:1731`)
	- `Read<Vec<T>>` carries one status for the whole array; a missing sub-path pushes `T::default()` and leaves status Good (spec line 361 says the slot is NotFound). Uncoercible scalar sets BadType, but an array/raw-valued slot hits the `Err(_)` arm and stays Good - internally inconsistent.
	- `count()` counts None slots, `instances()` flattens them away, so the two disagree on the same path and index pairing breaks.
	- Direction: per-slot statuses alongside the values (Vec in Rust, parallel array in C, multi-return in Go, list on the result in Python), aggregate = worst slot; keep empty slots in `instances()` (or spec count/instances as slot-count/slot-values); define the CLI surface for it; corpus rows with a missing sub-path.

- **Item 4 - comments destroyed by fmt** (`source/rust/src/lib.rs:966`)
	- Comments are dropped at parse (whole-line skipped, trailing stripped); `NodeData` has no comment storage, so `to_canonical()` cannot emit them. `fmt --write` therefore erases every comment from a hand-authored file silently. The spec's formatter section claims structure-only normalization; the Writer is spec'd to emit comment sections.
	- Options: (a) attach comments as trivia (leading list + trailing string per node, orphan list for blank regions) and re-emit - model rework in five codebases, cheapest now; (b) spec the loss explicitly and have `fmt --write` warn on stderr when comments were present.
	- Resolved with (a): each node carries a leading-comment list plus one trailing comment; on a merge the leading lists concatenate in encounter order and a second trailing comment demotes to leading. Comments after the last binding line are kept document-level and re-emit at the end. Stacked-list element comments ride the field node (block arrays canonicalize inline). A block in the same-line spelling moves its trailing comment above the line, since after the fence it could read as info-string text on re-read. Trivia never touches merge keys, reads, or diagnostics, so the formatter stays a fixpoint. Pinned by corpus case 013 and the comments now present in the older cases' expected files.

- **Item 5 - Writer missing everywhere** (`spec.md:371`)
	- No set/write/emit API existed in any binding beyond `to_canonical`/`fmt`. The Writer forces model decisions (comment representation, insertion position, quoting programmatic values, raw-block round-trip) that ripple through every binding, so cost grows with every month of port hardening.
	- Decided (among descope vs. implement): implement in the reference before 1.0, ported to all four bindings with a write corpus, while the port muscle is warm. Full CRUD surface: typed `set_<T>`/`set_<T>_array` (+ `set_raw`, `set_empty`), `set_<T>_default`/`_array_default` (only-if-absent = the "emit defaults" half), `exists`, `set_comment`, `remove`. Each setter is the exact inverse of the matching read: a string encoder (backslash/newline/tab, quotes left to `emit_element`) that `apply_escapes` reverses, the reader's canonical number/datetime text, and a fence chosen long enough that raw content cannot close it early. The empty array maps to an empty value.
	- Model note: the Writer mutates the arena's children vecs directly - the parser's child-index map is gone by load time and reads/`to_canonical` walk children, so no map upkeep. A set walks/creates intermediates, then at the leaf replaces the first same-named instance (or creates one); a `[value]` selector locates-or-creates a specific instance, `[#index]` must already exist, a wildcard is rejected.
	- Cross-binding exercise: a new `shcl set` CLI subcommand applies a tab-delimited write-ops script from stdin (`FILE` is the base, `-` = empty) and prints canonical. Corpus cases 014-016 pair `write.ops` with a golden `expected-write.shcl`; every binding's conformance runner applies the ops via its library Writer and matches the golden (and asserts the output is a fmt fixpoint), and the cross-binding differential replays `set` through all four CLIs byte-for-byte. A 50k-iteration reference fuzz pins the string encode/read round-trip over the reserved/escape/fence hazard set. Resolved.

- **Item 7 - fmt ignores strictness** (`source/rust/src/main.rs:242`)
	- `do_fmt` calls `Document::parse` (hardwired Standard) while every other subcommand uses `parse_with(text, o.strictness)`. Route fmt through the same load, exit 6 with diagnostics on strict failure; same one-line change in the Go/Python/C mains; pin with a corpus row at strict level.
	- Related looseness: every CLI accepts and silently ignores inapplicable flags (`check --int`, `get --write`); a later warn-on-unused pass would catch flag-placement typos.

- **Item 8 - mixed star/field children** (`source/rust/src/lib.rs:858`)
	- `add_star_element` has no uniformity tracking; each `* x` appends independently, so `sizes:` with `* small` / `color: red` / `* medium` yields an array plus a child, zero diagnostics - spec line 224 and grammar line 88 forbid the mix.
	- Fix: per-parent tri-state (none/star/mixed) set by both the star path and the field path; on first mix, emit an Error and treat subsequent `* ` lines under that parent as the malformed lines they are.

- **Item 9 - selector matching split-brain** (`source/rust/src/lib.rs:614`)
	- `attach_path` (parse) matches `field[disc]` via `Value::key()` on a synthetic one-element cell; `resolve_from` (query) matches via `display()`. They disagree for array-valued instances: `base: Boston, MA` + `base[Boston, MA].pop: 700` makes a second instance; count=2, duplicate `instances`, fmt emits both spellings.
	- Fix: make `attach_path`'s ByValue arm use the same display() predicate resolve_from uses, creating only when no display match exists; corpus case for selector-selects-array-valued-instance.

- **Item 10 - raw-block identity vs info-string** (`source/rust/src/lib.rs:177`)
	- `Value::key()` keys Raw on content only; two fences under one header with infos `sql` and `python` merge to one instance, the second info silently gone. The identical pair spelled as two headers stays two instances, so the accidental rule is not even applied consistently across spellings the spec calls equivalent.
	- Spec decision needed: include info-string (and fence style) in identity, or keep content-only and emit a hint when a merge drops a differing info. Corpus case either way - currently unpinned behavior all bindings merely share.
	- Resolved: we decided identity = content + info-string, fence style excluded. It is the data-preserving choice, and it makes the one-header-two-fences spelling behave like the two-header spelling the spec already calls equivalent. Pinned in spec.md (Raw blocks) and corpus case 012.

- **Item 11 - array-as-string read** (`source/rust/src/lib.rs:1689`)
	- Single element: escapes applied. Multi-element cell: `display()` - bare `, `-join, no re-quoting, no escapes; contradicts the doc comment, spec line 135, and `read_string_array` (which does escape per element). `Read.raw` shares display(), so "original raw text" is not the original text either.
	- Fix: define array-as-string as the canonical inline form (join `emit_element()` output so it re-parses to the same array) and one uniform escape rule; corpus rows with quoted/comma-bearing elements.

- **Item 12 - quadratic parse** (`source/rust/src/lib.rs:559`, `source/python/shcl.py:548`)
	- `select_or_create` linearly scans siblings per line, recomputing `value.key()` (a String build) per same-name candidate; `emit_repeated_leaf_hints` does a linear group-find per child. Both O(children^2) per parent. Measured (release): 12.5k lines 0.8s, 25k 2.9s, 50k 16s, 100k does not finish in 2 min. Python: 5k 0.7s, 20k 9.5s, 50k 72s.
	- Fix in the reference and each port: per-parent map (name, value-key) -> child index for select_or_create (keep the Vec for order), cache the key at node creation, and group hints via map + first-appearance list (hint order stays deterministic).
	- Resolved: per-parent map landed in all four parsers (C got a small arena-backed chained hash table). Values that mutate in place (an empty field filled by a raw block, star-list appends) move their map entry with first-wins semantics on both remove and insert, so lookups keep matching the earliest sibling exactly as the scan did. Hint grouping now indexes by name. Measured after: 100k flat lines rust 0.2s / go 1.0s / python 1.5s / c 0.2s.

- **Item 13 - Python recursion** (`source/python/shcl.py:863`)
	- `_emit_node` recurses per level. The CLI's `setrecursionlimit(20000)` caps around depth 19992; the reference handles ~5x that. Library users at the default limit crash near depth 1000 even though parse (iterative) succeeded.
	- Fix: iterative emit with an explicit (node, depth) stack, children pushed in reverse; then delete the recursion-limit bump in the CLI. Output already appends to a list, so the conversion is mechanical and byte-identical.
	- Resolved: `to_canonical` drives the stack, `_emit_node` emits one node; the CLI bump is deleted. Depth 25000 formats byte-identical to the reference from the CLI and at the default limit as a library.

- **Item 14 - ps1 resolution accepts non-executables** (`source/powershell/shcl.ps1:59-85,125`)
	- All resolution sites test `Test-Path -PathType Leaf` only (bash checks `-x` everywhere). On pwsh/Linux, `&` on a non-executable file produces no output, no error record, `$LASTEXITCODE` stays null; `exit $LASTEXITCODE` then exits 0. Worse, the OS may hand the file to the desktop opener, launching a GUI editor.
	- Fix: on Unix require the executable bit (`UnixFileMode -band UserExecute`) at every site, keep bare Leaf on Windows; and harden the passthrough - `exit ($LASTEXITCODE ?? 1)` and the same null-guard for the sourced function.

- **Item 15 - crosscheck trailing newlines** (`cicd/utility/crosscheck.bash:57`)
	- `out="$(...)"` strips all trailing newlines pre-compare; "42\n" vs "42" vs "42\n\n" all pass. Fix with the sentinel idiom (`printf x` after the command, strip it after capture) or write both sides to temp files and `cmp -s`.

- **Item 16 - crosscheck count floor** (`cicd/utility/crosscheck.bash:119`)
	- Exits 0 on "agree on 0 comparison(s)"; nothing asserts the fuzz-dump dir was populated after cicd evals `XCHECK_GEN`. Fix: exit 2 when nCompared==0 and when `--extra` matches no `*.shcl`; optionally a minimum-count knob in config.bash so 1764 -> 588 fails loudly.

- **Item 17 - gif generator exit codes** (`cicd/utility/gen-demo-gif.py:181`)
	- `fRunStep` never checks `res.returncode`; error text renders into the gif and stage 8 reports OK, stage 9 publishes. Fix: nonzero step fails the render (with an optional per-step `expect_exit` key in the scenario TOML for future deliberate-failure demos).

- **Item 18 - query-side pinning** (`cicd/utility/crosscheck.bash:95,111`)
	- Recurring differential coverage of the accessor surface is ~88 hand-written reads.tsv rows; the ~500-file fuzz set is compared via `fmt` only; the big port-landing batteries were never committed. `read_raw_info` has no CLI type at all, so it is unpinnable today.
	- Direction: have the fuzz dump also emit a derived `<name>.reads.tsv` per input (paths it knows exist, cycling types/strictness/on-bad), replayed through the existing row machinery; add a `--rawinfo` CLI type; commit corpus rows for wildcards and on-bad/defaults.

- **Item 19 - diagnostic wording contract** (`source/rust/src/main.rs:272`)
	- `check` prints prose diagnostics to stdout and crosscheck compares stdout byte-for-byte, so every format string in lib.rs is frozen five-way - the opposite of design.md's "wording is per-binding voice" rule (see Testing section above).
	- Direction: stable machine codes per diagnostic (E001, H001, ...); compared stdout becomes `line N: severity: code`; prose moves to stderr or behind a stripped delimiter. Load behavior (count, lines, severities, kinds) stays pinned; wording becomes free again.
	- Resolved: `Diagnostic` gained a `code` field (`E001..`/`H001..`) in all four bindings, set by a single `diag_code` map that keys off the message kind (the one place prose couples to a code; the reference threads it through `err`, the ports match by message prefix, and the sole hint kind is `H001`). `check` now prints `line N: severity: CODE` to stdout and the prose to stderr, so the differential check (which drops stderr) compares codes + the summary line + exit, freeing the wording. The C library exposes it via `shcl_diag_code`. Folds in item 36 (below). Crosscheck stays at 585 (the `load` rows now agree on codes instead of prose).

- **Item 36 - check exits 0 on errors** (folded into item 19)
	- `check` reported `ok` and exited 0 even when diagnostics included errors, so a CI gate on `check` passed configs whose lines were dropped.
	- Resolved (owner-pinned: nonzero exit): `check` exits 6 whenever any `error` diagnostic is present - a strict load failure prints `strict load failed: N diagnostic(s)`, and a standard/loose load that dropped lines prints `failed: N diagnostic(s), M error(s)`; a clean load still prints `ok (N diagnostic(s))` and exits 0. Same summary strings and exit in all four bindings (compared by the differential check).

- **Item 23 - `field[sel]: value`** (`source/rust/src/lib.rs:624`)
	- Grammar allows it, spec never defines it, implementation drops the value with an Error diagnostic - so strict loads fail on a grammar-legal line. Align the three: forbid in grammar, or spec the drop-with-Error as the defined meaning. Corpus case with a strict-level load row.
	- Resolved by spec'ing the drop-with-Error as the defined meaning (the code already did it, and a selector can legitimately appear on a non-last segment, so tightening the grammar would over-restrict). A selector on the last path segment already supplies the instance's value; a trailing value is an `error` and dropped, failing Strict. spec.md "Selectors" gained the rule, grammar.abnf a semantic note by `field-line`, and corpus case 018-selector-value pins standard-ok / strict-fail plus the discriminator instance.

- **Item 24 - argv encoding** (`source/rust/src/main.rs`)
	- `std::env::args()` panics (with panic=abort: exit 134) on non-UTF-8 argv; Go/Python/C exit 3. Fix the reference: `args_os` + a graceful "invalid argument encoding" error, exit 1 - and mirror the check in C, or document non-UTF-8 argv as out of contract.
	- Resolved: uniform rather than reference-only. All four validate every argv entry as UTF-8 before dispatch and exit 1 with the same message; a garbled arg is a usage error, and up-front validation removes the position-dependent exit-3 (bad PATH read as NotFound vs bad FILE as I/O). Rust uses `args_os().into_string()`, C reuses `utf8_valid`, Go uses `utf8.ValidString`, Python catches the surrogate-escape `encode` failure. Not corpus-pinnable (the crosscheck harness can't carry non-UTF-8 argv), so it lives in code + this note.

- **Item 25 - broken-pipe exits** (`source/go/cmd/shcl/main.go:227`, `source/python/cmd/shcl/main.py:299`)
	- Rust panics on EPIPE (134), Go dies by SIGPIPE (141), Python catches BrokenPipeError and returns 0. Decide the contract (conventional Unix = die by SIGPIPE, or a common clean exit) and implement it in all four; note in parity docs since crosscheck cannot exercise pipes.
	- Resolved: conventional Unix - die by SIGPIPE, exit 141. Rust and Python both install SIG_IGN by default, so both restore SIG_DFL at startup (Rust via a self-contained `signal` extern to stay zero-dep; Python via `signal.signal`, and its BrokenPipeError-to-0 catch is removed). Go and C already carry the default disposition. Not crosscheck-exercisable (pipes), so pinned here.

- **Item 30 - merge-key injectivity** (`source/rust/src/lib.rs:172`)
	- Cell elements join with bare NUL, so `[a, b]` collides with the single element `"a<NUL>b"` (NUL is grammar-legal in quoted strings); the later value silently merges away. Fix: length-prefix each element in the key (or include element count). Reference first, then ports, plus a NUL corpus case.
	- Resolved: each cell element is length-prefixed (`c:<len>:<text>...`) and the raw key length-prefixes the info-string (`r:<len>:<info><content>`), making both injective. The length metric is per-binding-native (bytes in Rust/Go/C, code points in Python) - merge decisions only depend on injectivity, which holds in each, so cross-binding behavior is identical. Corpus case 017-nul-merge-key pins `count = 2`; the crosscheck skips NUL-bearing inputs (bash cannot hold a NUL byte), so the four native conformance runners are what pin the cross-binding agreement.

- **Item 31 - bare-name charset and hex i64 min** (`spec.md:85`, `grammar.abnf:52`, `source/rust/src/lib.rs:225,1245`)
	- Spec prose: quotes only needed for reserved chars (and shows a non-ASCII field name); grammar/parser: bare names are ALPHA/DIGIT/-/_ only, so the prose's own example is dropped as a malformed line. Pick one truth and align prose, grammar, `is_bare_name_char`, and the emit quoting predicate.
	- Hex parse negates after an i64 magnitude parse, so `-0x8000000000000000` is BadType while its decimal spelling works: parse magnitude as u64, range-check against sign, mirror in ports.
	- Resolved: prose was the only outlier (grammar, `is_bare_name_char`, and the emit predicate already agreed), so the prose now states the bare-name charset explicitly and shows the `Straße` examples quoted - a name outside ASCII letters/digits/`-`/`_` must be quoted. Hex fixed in all four parsers by parsing the magnitude as u64 and range-checking against the sign (`i64::MIN` magnitude reads negative, overflows positive), pinned by corpus case 019-hex-int-bounds.
