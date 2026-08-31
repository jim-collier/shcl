<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
# Design

Design, requirements, and direction. The task list is in `backlog.md`. The full language definition is in `spec.md` (with `grammar.abnf`); this file stays high-level - the *why*, not the letter of the rules.

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
	- [Power layer (library-level, grammar untouched)](#power-layer-library-level-grammar-untouched)
	- [Schema validation](#schema-validation)
	- [Formatter](#formatter)
	- [Saving a file](#saving-a-file)
	- [Testing](#testing)
	- [Format comparison](#format-comparison)
	- [CI/CD](#cicd)
	- [Reference implementation](#reference-implementation)
	- [Go binding (Tier 2)](#go-binding-tier-2)
	- [C binding (Tier 2)](#c-binding-tier-2)
	- [Shell wrappers](#shell-wrappers)
	- [Man page and completions](#man-page-and-completions)

<!-- /TOC -->

## Assumptions

- The language began by worked example, before the spec existed. Where the early examples and the spec disagree (and they do), `spec.md` wins.

- The language is delivered as tiered bindings, plus single-file drop-in source and compiled binaries per platform.

	- The guarantee is the corpus, not the binding count. Every released binding is corpus-green, and nothing is released before it is.

	- Tier 1: the Rust reference and the `shcl` CLI built from it. Rust wins on the stated priorities: small binary, instant CLI startup, a clean shared-library build, and compile-time strictness that forces spec precision.

	- Tier 2: Go, C (with a C++ veneer), Python.

	- Tier 3: the rest (C#, Java with Kotlin, JavaScript with TypeScript), after v1.0, corpus-gated, designed for from the start.

	- Bash and PowerShell are thin wrappers around the CLI, not independent parsers. They inherit conformance for free. The companion typed surfaces (C++, Kotlin, TypeScript) are one core plus a veneer, not separate parsers.

## Guiding principles and decisions

The inescapable core tradeoff (for any config language) is to acknowledge and optimally balance "simplest possible" versus "expressive enough for anything" - including a simple DDL language. Ultimately it comes down to two factors:

1. Ideologically-driven opinion on where the balance should be - informed by technical experience, related PTSD gained along the way, childhood trauma - all of it.

1. The observation that processing power is now dirt-cheap even on the smallest embedded systems, validation and "correctness" tools are now incredibly powerful - and the subsequent decision to move as much of the heavy-lifting of the language on to this code, and away from end users and programmers.

Other points

- Optimize for the hand-authoring user and the value-consuming programmer; burden neither. Any ambiguity a modern parser can resolve from context, it must - the user is never made to satisfy the machine.

- The data model is relational, not a map. The left-of-colon token is a *field* (column), not a unique key; repeating it with different values yields *instances*. One rule covers wrappers, leaves, and valued instances: nodes are `(name, value, children)` and merge on matching `(name, value)`.

- Typing is accessor-driven: the parser stores raw text and never guesses; the consumer requests a type on read and the library coerces intelligently but safely, reporting problems without ever refusing to keep working.

- Forgiveness is a feature: never bail on a whole file for one bad line; skip/repair and diagnose; never error when a value is legitimately reachable.

- Forgiveness is also a knob, not dogma. There are three strictness levels (Loose/Standard/Strict, per-document, default Standard) instead of a binary strict flag. Standard keeps the defaults clean (no currency stripping, no `%` fractions, no float->int rounding, trimmed boolean set); Loose re-admits those conversions as a closed list for those who want maximum forgiving; Strict fails the load on any error diagnostic, the StrictYAML-style answer. Defaults are what adopters judge; the party tricks survive as opt-in. The normative bundle table is in `spec.md`.

- The knob belongs to the consuming program, not to the end user. Strictness and bad-read handling are part of the contract an application makes about its own config handling, so they are set at the call site (or the CLI flag) and nowhere else. A per-user defaults file was considered and rejected: it would let a user silently weaken guarantees an app relies on, and would make an identical `shcl` invocation mean different things on different machines - the `GREP_OPTIONS` mistake. Nothing else the CLI exposes is presentation-only, so SHCL has no user config file at all.

- Coercion earns trust by refusing to surprise: silent lossy conversion (rounding a float on an int read, `$1200` as a number) was cut from the default level for exactly that reason. Same logic killed the fehu anti-escape rune (raw blocks are the verbatim escape hatch) and restricted field-name case folding to ASCII (full Unicode folding is a locale trap and a cross-binding parity risk).

- Name identity normalizes by escape resolution, as of 2.0.
	- A name used to be compared and emitted in its escaped spelling, so `"a\"b"` and `'a"b'` were two fields while those same two spellings as values were one string - the two halves of the language disagreed on what a quoted string means, and a consumer splicing user text into a path had to know which half it was in.
	- Escapes now resolve on names too: they already normalized once (ASCII case folds), so this finishes a rule the language started rather than inventing one.
	- It waited for the 2.0 major because it moves canonical output for a spelling already published, and because two fields that were distinct merge - the same class of merge case folding already performs.
	- The as-authored accessor is deliberately the one call that still hands back the source spelling; that is what it is for.
	- Emitting needed a real escaper rather than the value one, which picks a quote style to *avoid* escaping and never escapes a backslash - correct for a value, which is stored in its escaped spelling, and wrong for a name, which is stored resolved.
	- One consequence fell out for free: a line break in a name is now writable, because the name escaper spells it `\n`, where before it had to be refused. A line break in a `[value]` selector is still refused, since the value emitter has no such spelling.

- Raw (fenced) blocks give verbatim escape hatches (DDL, code, templates) without contorting the config syntax.

- A fence is just a value line for its parent field. The same-line spelling (`name: ~~~sql`) read badly, so the canonical spelling puts the fence on the next line at child indent - and rather than special-case that, the rule is uniform. A fence fills the parent's empty value, or creates a new instance if the parent already has one (the repeated-leaf rule). Blocks are then ordinary instances: no index-addressing syntax to invent, and the existing `[0]`/`[#N]` selectors just work.

- Parity over idiom in the bindings: each port deliberately mirrors the reference's structure - same function inventory, same call flow, same contract strings - even where the host language would idiomatically do it differently. Byte-for-byte agreement is the product's core guarantee, and structural parallelism is what makes it maintainable: a fix ports by mechanical diff instead of re-derivation. Per-language idiom keeps the surface (formatters, naming case, doc comments); it yields on structure. The full rules and each accepted deviation live in `../style-guide.md`.

- Positioning: the pitch is "forgiving to write, predictable to read", with a read API built junior-first - not "simplest possible", which overpromises and invites the takedown. Versus schema-bearing languages (Pkl, CUE): the file stays dumb, the library is powerful (see Power layer).

- The CLI's informational commands (`help`, `version`, `about`, `donate`) each take both spellings - the bare word and the dashed flag. Among the options it was decided that one rule for the whole class beats deciding per command whether it reads better as a command or as a modifier. The flag spellings are recognized anywhere in option position, after FILE included, since where on the line a person types `-h` says nothing about what they want; only a value slot and anything after `--` are data.

- It was decided that `-` (stdin) may be named only once across FILE, `--layer` and `--schema`. There is one stream, so two names for it each read part of a document, and which name got the real content depended on read order; the second name is a usage error instead.

- Those outputs print with a blank line above and below, so the block does not butt up against the shell prompts either side of it. Two neighbors stay unpadded on purpose: bare `shcl`, which prints the same help text but as a usage error, and `version`, which stays a single bare line so a script can still capture it cleanly. The rule is "padded when a person asked for it", not "padded when it is long".

The itemized decisions are recorded in this file as they are made; `spec.md` is their normative form.

## Architecture

### Software stack

Many bindings with a shared conformance corpus (`conformance/`) as the contract between them. One portability constraint shapes the Accessor: the requested value type is expressed by a typed entry point or a compile-time generic, never a runtime `type` field, because static languages (Go, Rust, C, C++, C#) cannot let a runtime value drive a return type. Prebuilt binaries cover Linux and Windows, on x86_64 and ARM64. macOS and the BSDs are buildable from source but have no published binary yet, since the pipeline runs on Linux.

### Configuration model

See `spec.md` - fields/instances, accessor-driven types, arrays vs instances, raw blocks.

### Consumer API

The consumer-facing surface is the **Accessor** (read) and the **Writer** (emit).

- The Accessor's one conceptual operation is a **lookup** (query) - get a value at a path, coerced to a type, with a default and an on-bad policy - realized idiomatically per language.

- The type is chosen by a typed entry point or compile-time generic (not a runtime field), so results go into a strongly-typed variable with no consumer cast everywhere.

- For consuming a file as a whole there is **traversal** (scan): materialize the document - merge duplicates, order deterministically - then walk it (wildcards, instance enumeration).

- The Writer is the reverse: write out defaults, values, and comments. Structured diagnostics ride alongside.

The consuming programmer is assumed to be a junior in *every* binding, not just the dynamic ones, so ergonomics are a design constraint, not an afterthought. Decided:

- Two tiers, junior-first. A convenience tier is the documented default: one value, one baked-in fallback, one return, no status to inspect. The full tier, the status-returning form, is there when the caller needs to know *why* a read failed. Every binding spells the convenience tier `_or`, so a routine ported between two of them cannot keep the call name while changing tier; each language's native idiom for the same thing still works where it has one (Rust `unwrap_or`, Python's default argument, the CLI's `--default=`).

- A supplied default implies default-behavior. The caller never writes a fallback *and* an explicit on-bad - which is why the libraries carry no on-bad parameter at all: the tier you call *is* the mode. `Error` mode has no library form, because a read that cannot reach a value is a normal outcome rather than a fault, and a caller who wants a throw already has the status to raise on. The CLI keeps the explicit `--on-bad`, having no tiers to choose between.

- The convenience tier defuses the one real hazard of a forgiving accessor - a junior discarding the status and trusting a `0`/`""` that was actually empty or missing. Making the fallback mandatory and visible at the call site means an unwanted zero can't slip in unseen.

- Portability is unaffected: the type is still fixed by the entry point/generic, and the fallback is an ordinary parameter.

- **Reading a whole config into one structure is declined, not overlooked.** Every binding is path-at-a-time, so a forty-key config is forty call sites - and users arriving from libraries that decode a document into a typed struct in one call will notice.
	- It was considered and turned down, because the reference cannot implement it: a derive-based decoder needs a proc-macro, which is a second crate, and one file per binding with no dependencies is what the product *is*.
	- Hand-writing the reflection instead would give each binding its own machinery with nothing in the reference to mirror - the parity rule inverted, and the thing that keeps a fix portable by mechanical diff.
	- A binding-local decoder in the two languages that could do it cheaply would be worse than none, since the same config would then load two different ways depending on the language.
	- The plain answer is that the forty call sites are one loader function a consumer writes once, and `Children`/`Paths` plus the typed reads are the material for it.

### Integration modes

One question - "how do you pull SHCL into your project?" - with two kinds of answer: *run it* or *embed it*. Named plainly and ordered easiest-first, so a beginner starts at the top:

- **Command**. Run the `shcl` CLI from a shell or script. Nothing to compile, nothing to link.

- **Drop-in**. Copy one source file into your project. No dependency and no build step. You own the copy.

- **Package**. Add it as an ordinary dependency and let the package manager fetch it (`go get`, `pip install`, `npm i`, and so on).

- **Shared library**. Compile a drop-in source file into a `.so`, `.dll`, or `.dylib` and link it at runtime. The library stays a separate file.

- **Bundled**. Compile the same source into your binary, so you distribute one self-contained file.

The last two are the same code compiled two ways - "shared" stays a separate file loaded at runtime, "bundled" is baked into your binary. Neither is published as a prebuilt artifact, and that is a decision rather than a gap. A release carries the CLI binaries, the packages and the drop-in sources. Shipping a shared object means an ABI to keep: a soname, symbol versioning, a separate headers package, and a promise that a caller built against an old one keeps working. All of that pulls against the one-file zero-dependency premise, and the C header exports plain externs with no export macro, so a Windows DLL would need one anyway. Every mode reaches the same Accessor/Writer surface; the choice is packaging, not capability.

### Power layer (library-level, grammar untouched)

Compared to schema-bearing config languages (Pkl, CUE), SHCL is deliberately weaker in-language - that is the simplicity trade. To close most of the practical gap - the power lives in the library, never in the grammar: Pkl makes the config file powerful; SHCL keeps the file dumb and makes the library powerful. Everything below is optional - a consumer doing a bare `GetIntOr` never sees any of it - and none of it adds a rule a file author must learn.

- **Schema-as-SHCL validation.** A schema is itself a plain SHCL file describing expected paths (type, required, allowed values, ranges). One library call - `Validate(doc, schemaDoc)` - returns the same structured diagnostics loading already produces.
	- The schema vocabulary (`int`, `required`, ...) is interpreted by the validator, not the parser, so the core language stays free of reserved words (same pattern as the fence info-string).
	- This also closes the forgiving-parser typo hazard: the schema knows the legal field names, so unknown/misspelled fields get caught ("unknown field `enabeld`, did you mean `enabled`?"). Design below.

- **Layered loading.** (Implemented; see spec.md "Layered loading".) `Load(defaults, site, user, ...)` is a left fold of `merge(base, over)` across files.
	- Container instances merge on matching `(field-name, value)` - the existing in-file rule - but a leaf name in a higher layer *overrides* (replaces) the lower layer's same-named leaves, so defaults-plus-override works for scalars, arrays, and raw blocks, not just structure.
	- CLI `--set PATH=VALUE` sits on top as the final layer, written as literal text through the Writer.
	- Env-var mapping was deliberately dropped: the env namespace and its convention belong to the consuming program, which can map env onto `--set` itself (the same reasoning that canceled the per-user config file). `check` takes no layers - its diagnostics are single-file.

	- On `set`, a `--set` is an edit rather than a layer, so `set --write` persists it. That closed the only route to a shell-driven write being a tab-separated script on stdin, which no shell writes comfortably and no editor preserves. A family of typed options (`--int`, `--string`, ...) was measured first and rejected: `--set` writes the value as literal config text, so the type already follows the text and the typed forms produced identical output. One gate moved, rather than twenty options added.

	- `--set-literal PATH=TEXT` is the same option reading its argument as value syntax instead of data, which is what lets an array be written as an option at all. It parses the text rather than splicing it in, so the outcome is a value or a rejection - output stays canonical, a written document is still a formatter fixpoint, and there is no way to inject syntax through it.

- **Schema-driven generation.** (Implemented; see spec.md "Schema-driven generation".) `generate(schema, no_banner)` + `shcl init --schema` emit a commented, typed starter config.
	- `desc` becomes a comment, a generated annotation line summarizes type/constraints, required fields are live (their `default` or an empty value), optional fields are the same line commented out, and wildcard paths go in a trailing comment block.
	- Output is flat dotted form (mirrors the schema shape) and always loads clean, and ends with a footer naming the format and linking its spec.
	- The annotation line and the footer are both byte-for-byte cross-binding contracts, so their format is fixed and the annotation's numbers use the canonical formatters.
	- Among the options for the footer it was decided to write it by default and spell the knob negatively (`no_banner`, `--no-banner`): a generated file is usually the first SHCL a person ever sees, so the pointer to the spec earns its place, and a negative flag means the useful behavior is what a caller gets by saying nothing. It goes at the bottom so the settings, not the boilerplate, are what the file opens with.
	- Its `Legal` line leads with SHCL as the subject rather than with the copyright, so a reader cannot take it as a claim over the config it sits in.

Explicitly out of scope, with finality unless something big changes: in-language expressions, functions, inheritance, interpolation, imports, anchors/references. The moment config files can compute, they need debugging - that is the complexity cliff to avoid.

### Schema validation

The schema is a plain SHCL file, read with the ordinary parser and the ordinary Accessor. No grammar change, no reserved words, no new parser feature - the whole design was prototyped against the released binary before being written down.

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

The alternative (a schema that mirrors the document's tree, with constraints as children of each leaf) was rejected: it cannot tell a constraint named `type` from a real document field named `type`, so the schema vocabulary would collide with the user's namespace. The flat form has no such ambiguity, because the document's paths appear as *values*, never as field names. It also falls straight out of the relational model the language is already built on - `field` is the column, each path is a row - and reads as an ordinary SHCL file to someone who has never seen a schema.

Consequences of the flat form, all verified against the current binary:

- The validator needs no new lookup machinery: `Instances("field")` enumerates the described paths, and `field[<path>].type` reads a constraint.

- A path containing a bracket selector must be quoted (`field: "server[*].host"`), because a bare selector's scan ends at the first `]`. The canonical form writes the path as a value rather than a selector, so the formatter applies that quoting itself.

- A schema file is a formatter fixpoint like any other SHCL document, so `shcl fmt` works on schemas for free.

**Wildcards carry the repeated-instance story.** `server[*].port` constrains every instance of `server`, which is how a schema says "each server needs a port" in a language whose core idea is repeated instances. `repeat` (on the parent path) bounds the instance count itself - the one constraint with no equivalent in tree-shaped schema languages.

**The vocabulary stays small and closed**, in the same spirit as the Loose coercion list: `type`, `required`, `allowed` (an enum, written as an ordinary array), `min`/`max` (numeric ranges, inclusive), `repeat`, plus `default` and `desc` which only the generator reads.

- Fragments later added `inherits` as a constraint key, alongside the top-level `fragment:` declaration. Nothing joins that list without a spec change.

- Datetime ranges were considered and dropped for the same reason as regex below: comparing datetimes across zone suffixes needs calendar arithmetic (an offset can roll the date), no parser has any, and four hand-written implementations of it is a parity minefield.

**Regular-expression constraints are rejected outright**, and this is the one real capability given up. No two of the target languages share a regex engine - character classes, Unicode properties, and anchoring all differ - so a `pattern` key could not hold byte-for-byte agreement across bindings, which is the product's core guarantee. An enum covers the common case; anything past that belongs in the consuming program.

**Validation reuses the reads, so strictness composes automatically.** `type: int` against `3.5` passes at Loose (which rounds) and fails at Standard. That falls out of the validator calling the same typed reads a consumer would, and it is the correct behavior: the schema says what the program needs, and strictness says how forgiving that program is about getting it.

**Diagnostics reuse the existing structure** (line, severity, stable code, prose message), so a consumer inspects validation results exactly as it inspects load results. Two additions:

- A `V###` code range, disjoint from the parser's `E###`/`H###`, so a validation failure is never confused with a parse failure. Unknown field, missing required, wrong type, value outside the allowed set, out of range, and wrong instance count each get one.

- Line 0 means "no line" - the document-scope report a missing-required-field diagnostic needs, since the whole point is that nothing was written.

The "did you mean `enabled`?" suggestion rides in the prose message, not the code. Edit-distance implementations would otherwise have to agree byte-for-byte across four bindings for a string that is explicitly per-binding voice.

**A broken schema is reported against the schema.** Codes `V090+` cover schema faults (unknown constraint key, unusable type name), and their line numbers refer to the schema file.

- Originally any fault suppressed data validation entirely; after consumer feedback we decided a fault must not mask real violations - the schema builder already drops a broken key or field individually, so the surviving constraints now check the document too, with the faults listed first.

- The unknown-field sweep needs the complete declared vocabulary of names, but a key-level fault keeps its entry's path. So after a second round of the same feedback we decided the sweep runs through key-level faults. It skips only when a fault cost a path spelling outright (an unreadable `field:` path, or a mount naming no declared fragment), the two classes that would make declared fields read as unknown.

- Generation keeps the all-or-nothing rule - a partial starter file would be worse than an error.

**CLI surface: `shcl check --schema SCHEMA FILE`**, rather than a new subcommand. Loading and validating are the same question ("is this file good?"), the output shape and exit codes are already defined by `check`, and folding it in avoids a second nearly identical command.

Both open points are settled:

- `Validate` lives in each binding's single drop-in file. The one-file promise the README leads with outranks keeping the core lean; a schema user copying two files would quietly break the pitch.

- No string/array length bounds. They would reopen the byte-versus-code-point metric already settled for merge keys, for modest benefit; an `allowed` enum or the consuming program covers the need.

**Fragments close the recursion gap with one construct: `fragment:` declares, `inherits:` mounts.** The data language nests freely, but a flat path list cannot describe a tree whose nodes contain their own kind. Consumers were generating schema entries to a fixed depth, past which correct keys reported as unknown: validation returning false errors on valid files, in the tool CI gates on.

- Every mature schema language grew a named-shape reference for this reason; declining it would mean telling tree-shaped configs to flatten into instances with parent pointers, which makes the file serve the schema tool instead of the reader.

- Naming: `use` was rejected as overloaded English (verb-directive or "purpose"); `parent` collides with the parent/child vocabulary of a nesting language; `inherits` reads as a single plain word beside `type`/`required`/`allowed` and is accurate - the mount inherits the fragment's fields and can add its own.

- Design properties worth keeping true: expansion is demand-driven (a mount is followed only where the document has nodes), so recursion has no depth limit, needs no cycle detection, and costs document-proportional time and memory; the generator is the one place expansion could run away, and it cuts exactly where a fragment would re-enter itself.

- Suggestions do not descend mounts, same rationale as below stars.

**Open sections use a name-position wildcard, and it lives in the lookup grammar, not just the schema.** A map-shaped section ("any child name under `indicators`, each shaped like this") had no schema spelling at all - wildcards selected instances, never names.

- Among the candidates (a `*` name segment, an `open:` constraint key, glob patterns), the bare `*` segment won: it reads exactly like the `[*]` story one level up, needs no new vocabulary word, and composes with deeper paths (`indicators.*.period`) without a second construct.

- We decided it also belongs in reads, not only schemas, so the query language stays one language: `Get("*.port")` slots across children of any name the way `[*]` slots across instances. The Writer refuses it like any wildcard (paths must name their target to be writable). A field literally named `*` stays addressable quoted (`"*"`), which is never a wildcard.

- Suggestions ("did you mean") do not reach below a `*`, since there is no fixed sibling list to suggest from, and a `repeat` on a `*` leaf disavows no `H001`.

**A quoted by-value selector is scalar-only.** By-value matching is against the display form, and an inline array's display joins elements with `, ` - so the scalar `"a, b"` and the list `a, b` met the same selector and a read could only answer Multiple.

- Among the candidates (a scalar-only quoted spelling, an IndexOf companion, leaving it documented) we decided the quoted spelling wins: quoting already forces a value match over an index and already escapes sentinels and formats elsewhere this round, so "quoted selects the scalar spelling only" extends one rule instead of adding a second construct.

- Bare selectors keep the whole-display match, so existing paths behave identically; the parser's accelerator keeps the first same-display child, with a rare fallback scan when a quoted selector needs the scalar sibling instead.

**Hand-edited configs are structurally safe across a round trip: retain what can be retained, gate the save on the rest.** A malformed line used to be diagnosed and dropped, so a stray typo plus one settings change equaled a silently vanished hand-written line.

- The full fix splits by what is provably safe. A content-malformed line (unreadable at any position) is retained as inert trivia and re-emitted in place; it re-diagnoses identically and can never read as a live binding. A line the parser could read but not apply (bad indent, unusable selector, depth cap) cannot be made inert: re-emitted, it might parse as live content and invent data, which the fuzzer confirmed for BOM-led lines.

- Those instead count into `LostCount`, and the file tier's save refuses while it is nonzero, with an explicit lossy variant as the override - so deleting a user's line is always a stated choice. We considered retaining everything verbatim and rejected it on the invented-content risk.

- The CLI's in-place write goes through the same gate: it was first left alone on the grounds that a person sees the diagnostics on stderr, which turned out to be false - at the default strictness the load recovers and prints nothing, so `--write` deleted the line at exit 0 in silence. It now prints the load's diagnostics and refuses while `LostCount` is nonzero, with `--lossy` as the stated override.

- The CLIs call `SaveFile` rather than carrying their own copy of the rule, so the command line and a consumer program cannot disagree about which rewrites are safe.

- The refusal is a value, not prose: every binding reports it distinguishably from a failed write, because only one of the two is the caller's to reverse - and Python raises rather than returning a message, since a message a caller may ignore turns the safest spelling of the call into a silent no-op that reports success.

**The library carries an optional file tier; file lifecycle is where consumer bugs live.** Consumer feedback showed every program that persists a config re-implementing the same load/save dance and making the same mistakes independently - confusing absent with unreadable, fumbling buffer lengths, tearing a config with a plain overwrite.

- We decided on a small companion tier. The load never fails and returns a four-way status (clean / had-errors / not-found / unreadable) beside an always-usable document. The save writes canonical text through the same atomic temp-and-rename the CLI's `--write` already used, moved into the library so the CLIs call it and the two cannot drift.

- It stays a companion, not core: C guards it behind `SHCL_NO_FILE_IO` so embedded consumers keep a file-I/O-free build.

**H002 reports every merged level, and a schema can disavow it per section with `reopen:`.** The first cut hinted only the outermost re-open: inner merges looked adjacent at their own scope, because the newest-child test cannot see that the whole region arrived by re-opening. That made consumer-side filtering unsound - allowing one section's hint silently waved through everything nested under it.

- We decided merges under a hinted container hint too, each naming its own earlier line (the parser carries the re-open line down, so text the re-opened region itself wrote stays silent).

- The disavowal is a dedicated `reopen:` key rather than an overload of `repeat` - "many instances" and "one section written in parts" are different declarations.

- Suppression mechanics mirror the H001 one exactly: single wording site, leaf-name match, dropped where diagnostics and a schema meet.

### Formatter

Structure-only canonicalizer: block form, tabs, insertion order, minimal quoting, redundancy collapsed, value text untouched (it cannot know types).

**Author quoting on plain strings survives canonical output; quoted data-format values still normalize to bare.** Quoting had been pure spelling, normalized away entirely - which silently un-escaped values a downstream language treats as special (`"@null"`, a quoted function ref), the very case the `quoted` read flag was added for.

- We decided the rule splits by what the text reads as: int, float, bool, and datetime spellings normalize (`ver: "8"` -> `ver: 8` - readers type the value either way, so the quotes say nothing), while a quoted plain string keeps its quotes through `fmt` and `init`.

- The gate uses standard strictness, fixed, so canonical form cannot vary with load strictness, and the rule only ever adds quoting over the reserved-character minimum, so no bare emit can become unsafe.

**A raw block's nesting is the closing fence's own indent, and the rule is symmetric.** The nesting used to be the common indent of the body's non-blank lines, which made a shared body indent unrepresentable and needed an emit exception for a body with no non-blank line at all - the shape that once grew by a level per pass.

- It was decided that the fence, not the body, defines the nesting: each body line loses only what it shares with the closing fence's indent (the opening line's when the block never closes), and emit pads every non-empty body line by that same indent.

- Strip and pad mirror each other exactly, so no special case is left: a body's shared extra indent survives as content, and a whitespace-only body keeps its spacing for the same reason as any other line - a raw block promising verbatim content should not be the place that quietly rewrites it.

**`SetRaw` refuses an info-string an emitted fence line cannot carry.** A line break, or an unquoted `#` - the fence line would read the `#` as opening a trailing comment, so the block came back with a different info-string than the one written. The info-string is also trimmed the way a fence line reads it back, and the op script's `raw` op shares the gate.

**A raw block in a higher layer fills a same-named empty binding below.** Merge matched instances by `(name, value)` only, so a bare `blk:` in the base and a `blk:` carrying a block in the overlay both survived a merge, where parsing the two run together folds them.

- That made merged output not a formatter fixpoint, and dragged in a second defect, since the emitter's workaround for the resulting pair spells the fence on the name's line and loses an info string containing `#`.

- It was decided that merge adopts the parser's own empty-fill rule, so a merge and a parse of the concatenation agree. The fill is limited to raw blocks because that is the limit of the parser's rule: a valued instance still appends.

### Saving a file

- **A save publishes a new file in the old one's place** - write a temp file beside the target, then move it over. That is what makes an interrupted save unable to truncate a config, and it is also the source of every limitation below: the bytes are new, so anything the old file carried outside its contents has to be deliberately carried across or it is gone.

- **What is carried, and what is not.** The permission bits are copied deliberately, and on POSIX that is the whole of what gets copied: ACLs, extended attributes, the SELinux label and any other xattr are lost, as are other hard links to the old file.
	- None of that is fixable at this layer, since a rename cannot preserve what a rename replaces, so it is documented in the spec rather than papered over.
	- A relabeled config on an SELinux host is the case worth knowing about: the new file takes the label its parent directory and the writing process imply, which is the same label in the ordinary case and not the same one after a `chcon`.

- **Windows goes through `ReplaceFile` instead**, because it does not have the same constraint. `ReplaceFile` exists for exactly this publish step and carries the destination's ACLs, attributes and named streams onto the replacement, which is the gap a plain move leaves. It needs a destination to replace, and it fails outright rather than skip a merge it cannot perform, so a create and any failure fall back to the replacing move - the behavior that was there before, never worse.

- **The C file tier reaches Windows through the wide API, and splits a path on either slash.** Both came out of the first consumer to embed the C header on Windows. The narrow file calls read a path in the process's active code page, so a path with a character outside it either failed to open or, worse, was written under a mojibake name that the same narrow read found again. Every call is the wide one now, and a path that is not valid UTF-8 is refused rather than folded to a different name. The temp name was derived from the last `/`, and a path built with the platform separator has none, so every save through one failed. A drive-relative `C:x` splits after the colon, where the reference's `Path::parent` splits it. The other three bindings' runtimes already did all of this.

- **The Go binding reaches it through a hook rather than a build-tagged pair.** Go has no way to name a windows-only symbol from a file that also compiles elsewhere, and `shcl.go` promises to work when copied into a tree on its own.
	- So the publish step is a package-level variable holding the plain rename, and a small windows-only file swaps in the `ReplaceFile` version. Dropping the one file still works everywhere; taking the whole module gets the better windows publish.
	- The consequence to remember is that the lint stage only ever sees the host's `GOOS`, so the windows file is compiled and vetted in the cross stage instead - it is the only thing that looks at it at all.

- **The directory is synced after the move, not just the file before it.** The file's own `fsync` only promises the contents reached the disk; the move is a change to the directory, and without a sync there a power cut can lose the publish and leave the old content. It is best effort in every binding (a filesystem that refuses an `fsync` on a directory is not a reason to fail a save that already succeeded) and it is POSIX-only, since Windows has no equivalent and `ReplaceFile` is asked to write through instead.

- **A file the save had to create takes the mode an ordinary create would**, `0666` narrowed by the umask, rather than a private `0600`. Among the options it was decided that a library should not quietly make a decision the user cannot see: a config that must be private needs that from the umask or a `chmod`, which are the mechanisms a person already reaches for. An existing file still keeps its own mode, and that case is why the temp file is born private - the copy is never briefly readable to anyone the original was not.

- **`set --write` creates a FILE that is not there yet.** `--write` names the file the command produces, so reporting it missing was an obstacle rather than a safeguard - the workaround was to `touch` it first, which is the same act with an extra step. `fmt --write` deliberately does not, having nothing to format. A file that exists but cannot be read stays an error in both, since the alternative is writing over something unread.

- **A refused in-place write exits 7, its own code.** It shared 1 with usage and I/O errors, so a script could not tell "pass `--lossy` or fix the file" apart from "the command line is wrong" - and the refusal is the one failure whose remedy is a decision rather than a correction.

- **A file or stream that cannot be read or written exits 8, and 1 is now the usage code alone.** The same reasoning as exit 7, applied to what was left in the catch-all. The two remedies have nothing in common: one is fixing the command line, the other is fixing a path, a permission or a disk. A path a write option refuses (a wildcard, an index naming no instance) stays at 1, because what has to change there is the option's value.

- **Every subcommand that loads a document prints the load's diagnostics to stderr, once per run.** It was decided that neither where the canonical text goes nor which subcommand asked for the load should decide whether a recovered-from typo is mentioned. This started as `fmt` and `set` in both modes, with the read subcommands left quiet; that half was reversed, because a read below strict returns the value and says nothing at all, which is the case where silence costs most. stdout carries the same bytes either way.

- **The mode is applied to the temp file again after its data is written.** The kernel clears setuid and setgid on a write by a process without the right capability, so giving the temp file the target's mode before filling it silently dropped those bits - the copied mode has to land last, after write and fsync, before the publish.

- **A symlink cycle at the write target is an error.** Resolving the target is what makes a linked-in config written through rather than replaced, and a cycle used to fall out of the resolver as "no target", which quietly replaced the link with a regular file. A loop is reported as what it is - too many levels of symbolic links - and nothing is written.

### Testing

The conformance corpus is the primary cross-language guarantee. Each case is an input, its canonical formatting, and the expected typed reads with status sentinels. Every parser runs it in CI.

Passing the corpus independently is necessary but not sufficient once there is more than one binding. Two implementations can each satisfy the expectations yet still disagree on details the corpus never pinned, like float rendering or diagnostic-free edge behavior. So the pipeline also runs a differential check:

- Every binding's CLI is replayed over the same inputs: the whole corpus plus a freshly generated fuzz set.

- All bindings must agree with the reference byte-for-byte on stdout and exit code. The reference is Rust.

- stderr is deliberately outside the contract. Diagnostic wording and OS error text are per-binding voice. stdout and exit codes are the contract.

Both of those run on small inputs, which leaves a whole class of defect unwatched: anything that only appears at scale. A parser that is accidentally quadratic, a buffer that grows wrong past a few megabytes, or a memory profile that puts a real config out of reach all pass a corpus of few-hundred-byte cases. So the pipeline also formats one large generated document - 100 MiB by default - through every binding:

- The bindings must agree on the result byte for byte, exactly as on the corpus. The document is generated rather than stored: a fixture that size has no business in a repo, and the shape matters more than the bytes.

- Each binding is held to a wall-clock and a peak-memory ceiling, expressed per input MiB so they follow the configured size. The ceilings are set to catch a change in growth rate, not to time a machine - the time ones carry wide headroom, because the pipeline runs the reference unoptimized and a hosted runner is slower again. Memory is held closer, since peak usage barely moves between machines.

- At that size the reference also has to prove formatting is a fixpoint and that a long array reads back whole, both of which are cheap to state and impossible for a small case to check.

The size is deliberately a floor rather than a target. It was chosen because it is roughly where memory stops being free: every binding holds tens of times the input size in memory while parsing, so a document this big is the first one whose cost is worth a decision.

All of that runs on Linux, which was enough while every binding did the same thing everywhere. The file tier ended that: publishing a written file is a different code path on Windows in all four bindings, so the platform-specific half was covered only where it never executes. A second CI job runs the runners and the veneer smoke on Windows.

Its scope is deliberately narrow, and it is not a second definition of passing. The corpus goldens, the differential check, the large document and every lint gate stay on the Linux job, where they are defined. The Windows job is there for the code that only exists there.

Two supporting decisions came with it. The C runner walks the corpus with `dirent.h`, which MSVC does not have, so the compiler is gcc. And end-of-line conversion is turned off repo-wide: Git for Windows defaults it on, the goldens are compared byte for byte, and some of them are about line endings specifically.

Two portability rules bind every binding:

- Floats render as shortest round-trip decimal, never scientific notation. This matches the reference's native float formatting.

- Diagnostic order must be deterministic, in first-appearance order. A port can match a rule, but not a coin flip.

### Format comparison

The feature comparison on the front page says how SHCL differs from JSON, YAML, TOML and XML. Nothing said what any of it costs, so `cicd/utility/comparison/` measures that, and the numbers README quotes come from there.

Method, and why each part of it is the way it is:

- **The language is held constant within a tier.** Every format in a tier is read by a library from that ecosystem, built the same way. Measuring SHCL's C binding against a JSON parser in Python would compare implementations and call the answer a property of the formats.

- **Two tiers, never ranked against each other.** Rust is the headline, because there every format has a mature native parser and the comparison is as close to formats-only as it gets.
	- Python then repeats the exercise over the same documents, to answer the question one tier cannot: how much of a format's cost is the format and how much is one implementation of it.
	- Most of what Python reaches for is a C extension wearing a Python name - `json`, `ElementTree`, PyYAML's `CSafeLoader` - while this project's Python binding is pure Python, so the row that carries the weight there is `tomllib`, which is also pure Python.
	- That pair is the tier's only like-for-like comparison, and every entry records which side of the line it is on. Python libraries that are not installed are skipped and named rather than failing the run.

- **One abstract model per document, five encoders.** Each shape is built once as a small tree and then encoded five ways, so the files hold the same data by construction rather than by five hand-written generators happening to agree. Each encoding is the spelling a person would really use - SHCL raw blocks against YAML block scalars against XML CDATA - because a number taken from an unidiomatic encoding is not measuring the format.

- **A pre-flight equivalence check.** At a small scale, every library has to parse its own file and find the same number of scalar values in it. One model does not rule out an escaping mistake in one encoder, and a size or speed number taken from documents that are not the same data is worth nothing.

- **One process per measurement.** Peak resident memory is only attributable that way: a process that parsed six documents says nothing about what any one of them cost.

- **Six shapes**, because one document shape hides most of what separates these formats. Four of them scale to whatever size the run asks for: long and flat, wide and deep, an array of records, and multi-line text blocks. The other two carry their own realistic size instead - a hand-edited application config of a couple of kilobytes, and a schema definition file of a few hundred. A config file measured at 64 MiB is not a config file anybody has, and the scaling shapes measured at two kilobytes would be measuring process startup.

- **The run count scales with the document.** Best of three says nothing when the parse takes microseconds, so a small document gets proportionally more timed runs, up to 200 times the count the run asked for. The count actually used is recorded beside each shape.

Several choices deliberately cut against SHCL, so that a favorable result stays one:

- **No comments in any document.** Four of the five formats take them and JSON does not, so any comment at all would compare different documents - and comment retention is exactly the difference SHCL would most like to show off.

- **JSON is pretty-printed and XML is indented**, since the premise is a file a person edits. JSON also gets `preserve_order`, because a config loader that silently sorts the file's keys is not one anybody would ship.

- **TOML and XML each get two libraries.** `toml_edit` is the only mainstream non-SHCL parser here that keeps the file as written, which makes it the one genuinely like-for-like comparison. XML is measured both by the fastest tree in Rust and by the tree you can write back.

- **The generated SHCL is canonical**, a `fmt` fixpoint, so the round-trip column reports what the format does rather than how the generator chose to space things.

Rows are ordered by the geometric mean of each library's parse time across every shape, fastest first - one order used for every printed table and for the results file, so a row keeps its place from shape to shape. The geometric mean rather than the arithmetic one, so a single large shape cannot decide the whole order; the number it sorts on is recorded beside each library, so the ordering can be re-derived rather than trusted. SHCL sorts last in both tiers; that is the measured result, and the reason the order is stated rather than chosen.

Reading a value by path is deliberately not measured. The five lookup APIs differ enough that the harness would have to write the walk itself for most of them, and the result would be a measurement of harness code.

It is not a pipeline gate. Benchmarks are noisy and slow, and a red build caused by a busy machine teaches nobody anything. It runs on demand, and its results accumulate in `results.shcl` - which is the tooling's storage format as well as its subject, written, pruned and read back through this repo's own library.

One thing the tool found about a library rather than a format, recorded so nobody re-derives it: `lxml` goes quadratic on the long-and-flat shape, dropping from 59 MiB/s at 3 MiB to 5.7 at 26 - measured with and without `huge_tree`, which makes no difference, so it is not the flag that lifts libxml2's size ceilings. The likely cause is the shape's millions of *distinct* element names against libxml2's name dictionary; the shapes whose names repeat show nothing of the sort, and no other library in either tier does this. It is a good argument for measuring more than one document shape.

What it found, at 64 MiB per shape (rerun after the 2.0 memory work):

- SHCL writes the smallest file of the five in three shapes of four, and the gap widens with nesting - half the size of JSON and of XML on deep structure. Gzipped, the five sit within a fifth of each other, so the win is a plain-text one.

- SHCL is still the slowest to load in both tiers by aggregate, six to ten times behind `serde_json`, but now sits near the light end on memory: below TOML and YAML on every shape, below JSON on two of four, and a quarter of `toml_edit`, the one other parser that keeps the file.

- The Python tier says the load cost is the format's rather than one implementation's - against `tomllib`, the tier's one other pure-Python parser, SHCL is 3.5x behind, against 2.1x behind `toml` in Rust. That trade is the design working as intended rather than a defect.

- At the two realistic sizes the size result holds and the speed result stops mattering: SHCL writes the smallest file of the five for both the config and the schema definition, and reads them in 0.07 ms and 13 ms.

### CI/CD

The responsibility is split rather than duplicate the pipeline:

- The GitHub workflow (`.github/workflows/ci.yml`) is a correctness gate only - format check, build, lint, tests on push/PR. Minimal permissions, cancels superseded runs, times out.

- Everything else (cross-compile, packaging, publish) stays in the local pipeline, `cicd/cicd.bash`, config-driven via `cicd/config.bash`.

- Both share one definition of "passing": the workflow just runs `cicd.bash --ci`. Per-language toolchain setup lives in the workflow YAML; what passing means lives in the engine, so the two cannot drift.

- The formatter rewrites in place locally but is check-only (fail on diff) in CI.

- Branch flow: `dev` is the integration target (feature branches merge there, `--no-ff`); `main` is release-only. A dev -> main merge is normally a release cut.
	- The exception is a merge that changes no product code - documentation, the demo asset, the pipeline. Those go to `main` on their own so the front page and the install one-liners (which read from `main`) stay current, and the version stays where it is. Cutting a tag for them would publish a second set of binaries that behave identically to the last one, which tells a reader nothing.

- One canonical version source: `source/rust/Cargo.toml`. The pipeline reads it for artifact names and release tags. (An automatic bump-before-push guard was tried and dropped: dev is the integration branch, and versions there are cut deliberately at release time, not policed per push.)
	- The Go/Python/C CLI version strings and `source/python/pyproject.toml` are hand-kept mirrors of it, moved together at a cut along with the changelog heading and the front-page README's literal version strings.
	- Two tags per cut, not one: `vX.Y.Z` for the release, and `source/go/vX.Y.Z` for the Go module. Go resolves a module in a subdirectory only through a path-prefixed tag, so skipping the second one silently strands every Go consumer on a pseudo-version.
	- Release assets are signed before the release is published: both installers refuse a release whose sums file has no valid signature, so publishing first would offer an install nothing can verify.
	- A registry publish cannot be taken back. Neither crates.io nor PyPI allows reuploading a version, and each bakes in the README it was given at that moment - so the per-binding READMEs (`source/rust/README.md`, `source/python/README.md`) get their example code compiled and run against the packaged artifact before the upload, not after. Those files exist separately from the front-page README because relative links and images break on a package page, and Cargo cannot reach above the crate root anyway.
	- Binding versions move in lockstep with the product version, deliberately. The bindings are byte-for-byte equivalent by design, so a single number across all of them means a consumer reading `2.1` in any language knows exactly what behavior they have. It also lets each ecosystem's ordinary compatible-version operator (`shcl = "2"`, `shcl~=2.0`, Go's major-version import rule) do the tracking, with no scheme of our own to explain or maintain.

- Installer packages ride the release stage, not a separate pipeline: `cicd/utility/package.bash` builds .deb/.rpm (nfpm, one sed-rendered template) per Linux binary and an NSIS setup per Windows binary, into the same versioned artifact family before the checksums are written. Package layout follows distro convention (/usr/bin + /usr/share/shcl) rather than the /opt layout the standalone install.bash uses - packages answer to distro policy, the script answers to the spec. Payload matches install.bash: binary + code/ drop-ins + scripts/ wrappers.

- Release trust root: the sha256sums file is signed offline with an RSA-4096 key, and both installers carry the public half inlined and check it before reading a checksum out of the file. Decisions behind it:
	- Order matters more than the algorithm - a checksum taken from an unverified sums file proves nothing, so the signature is checked first or not at all.
	- The threat addressed is release-asset replacement (a leaked token with release scope, a compromised CI job), not full repo compromise: an attacker who can rewrite `install.bash` on `main` can also delete the check. Those are different access paths, and separating them is the point.
	- The key is offline and signing is manual. A key in CI secrets would be reachable by exactly the compromise being defended against, which would make the signature decorative.
	- The key is inlined, not fetched. A key downloaded over the same channel as the artifact authenticates nothing.
	- RSA, not the more fashionable Ed25519, purely on verifier availability: `openssl` covers Unix, and .NET's RSA covers Windows PowerShell 5.1, where Ed25519 is absent. Any scheme needing a tool the box does not already have (minisign, cosign, gpg) loses to the bootstrap problem - verifying the verifier.
	- `openssl` became a hard prerequisite of `install.bash` alongside curl/wget. Installing unverified is not offered as a fallback; the DIY path is there for a box that genuinely lacks it.
	- Rotation is expensive by construction, since old installers carry the old key. Treat a key change as a breaking change: new installer, and a release note.
	- The Go binding gets tamper-evidence free from `sum.golang.org`, so this covers the binaries and drop-in payload, which have no equivalent backstop.

- Toolchain pins: `rust-toolchain.toml` (rustc + clippy + cross targets) and warn-only pins for cargo-installed helpers, so a box update cannot silently change results.

- Fuzzing lives in the regression suite, not a separate rig. A deterministic mutator over the corpus asserts two invariants for any input: never panic at any strictness, and the formatter is a fixpoint. The same mutator generates the inputs for the differential check above.

- Profiling is a standing stage. Every full run samples an optimized build over a heavy parse-and-format workload and emits a flamegraph plus a hot-spot summary, so a performance regression shows up in the artifacts the run it happens in. Sampling is in-process, feature-gated, and never reaches a release binary.

### Reference implementation

- Rust crate at `source/rust/`, zero dependencies, single-file library (`src/lib.rs`) so the drop-in integration mode stays real; the `shcl` CLI builds from the same crate. Later bindings get sibling folders (`source/go/`, ...).

- The conformance runner and fuzz smoke are plain `cargo test` targets, so "the corpus passes" and "the build passes" are the same command everywhere.

### Go binding (Tier 2)

- Module at `source/go/`: single-file library (`shcl.go`, zero dependencies, generics for the typed reads). The CLI under `cmd/shcl/` is its own module, so it stays out of the published one - same flags, output, and exit codes as the reference, but it exists to be driven by the differential check rather than installed.

- Conformance runs natively as `go test` (a port of the Rust runner over the same corpus), so the Go binding is corpus-green on its own, and the cicd crosscheck holds it byte-for-byte to the reference besides.

- The pipeline stays engine-generic: the Go fmt/build/vet/test commands ride the config's per-stage extras, and the binding registers in `BINDING_CLIS` - a pattern each further binding (C, Python) repeats without engine changes.

### C binding (Tier 2)

- Sources at `source/c/`: a single-header drop-in library (`shcl.h`, C11, zero dependencies) plus the CLI under `cmd/shcl/` - same flags, output, and exit codes as the reference. The single-header story is the C analog of the other bindings' single-file libraries: copy `shcl.h` into a tree and, in one translation unit, `#define SHCL_IMPLEMENTATION` before including it.

- The C++ typed surface (`shcl.hpp`) is a veneer, not a second parser: a header of `Read<T>` / `get<T>()` templates over the same C functions, so it inherits the core's conformance and only needs a compile-plus-behavior smoke to keep it correct.

- Conformance runs natively (a C port of the runner over the same corpus), so the C binding is corpus-green on its own, and the cicd crosscheck holds it byte-for-byte to the reference besides.

- Memory is a per-document bump arena, so teardown is a single free with no per-object bookkeeping. A short-lived-tool trade that keeps the port readable.

- Allocation failure is a hook, `SHCL_OOM()`, and the default is still the CLI's print-and-exit-70. The reference aborts on allocation failure too, since that is the language's contract, so making a C parse fail instead would invert parity for a case none of the other three can report. What a library must not do is end a process that is not its own: an embedder defines the hook before the implementation and takes it from there (longjmp out, log, abort). Nothing is unwound first, so a hook that returns leaks the document being built.

- Two portability details the reference gets for free but C makes explicit: UTF-8 is iterated by codepoint (plain byte scanning would mishandle multibyte text), and float output reproduces the reference's shortest-decimal, never-scientific rule.

- C has no committed zero-dependency formatter, so its quality gate is a warning-clean compile rather than a separate format stage.

### Shell wrappers

- The shell binding wraps the `shcl` CLI, not a parser, so it inherits conformance for free.

- Bash 3.2 (`source/bash/shcl.bash`) was targeted rather than POSIX sh (mostly defined in 1979). The wrapper earns its keep by being dual-purpose: run it as a script, or source it and call functions. That dual mode and the typed helpers read far cleaner with Bash's arrays and `local` than with portable sh. A thin passthrough would give a sourcing caller nothing over the binary itself.

- PowerShell (`source/powershell/shcl.ps1`) is the second wrapper, built to the same design: dual-mode (run, or dot-source for the identical `shcl`/`shcl_*` helper names), the same binary-resolution order, and exit codes passed through into `$LASTEXITCODE`. It deliberately has no script-level param block - one would try to bind subcommand words - so every argument arrives in `$args` verbatim. Like the Bash wrapper it forwards rather than parses, so it is not in the cross-binding differential.

- One `shcl` function is the whole CLI. `shcl_get`, `shcl_int`, `shcl_bool`, and friends are one-line typed sugar. Both modes take the same arguments and return the binary's exit code unchanged, so a not-found or empty read stays a distinct nonzero.

- Two things a sourced tool must not do, and doesn't:

	- Leak shell options into the caller. Strict mode is armed only on the run path.

	- Let its own `shcl` function shadow the binary during lookup. The binary is resolved via `$SHCL_BIN`, a co-located `shcl`, PATH, then the repo build, so a dogfooded install and in-repo dev both work without configuration.

### Man page and completions

- `source/man/shcl.1` is roff by hand rather than generated from the help text. The help is a byte-for-byte contract across the four CLIs and is shaped by an 80-column terminal; a man page has different obligations - sections a reader can jump to, and room to say why a refusal exists. Generating one from the other would either bloat the help or flatten the page.

- The man page carries no version string, so the release bump stays the same eight files it has always been. What it does carry is a revision date.

- The completions mirror the CLI's own per-subcommand option table rather than inventing one. Offering an option the subcommand rejects is worse than offering none, because the CLI treats an unusable option as a usage error rather than ignoring it. `cicd/utility/check-completions.bash` diffs the CLI's table against both completion files at lint time, so the three cannot drift apart silently.

- Bash and zsh completion files are kept structurally parallel - the same table, the same helper names, the same positional-counting loop - for the same reason the bindings are: a fix ports across by mechanical diff.

- Neither completion offers anything for a `PATH` argument. Nothing can enumerate the paths in a document without reading it, and filenames - the obvious fallback - would be wrong every single time.

- **Where each channel installs them.** Among the options it was decided that the packages and the installer should behave differently, because they are different kinds of thing:

	- The `.deb` and `.rpm` put the man page and both completions in the distribution's own directories, where each shell already looks. That is what a package is for, and it is the only channel that can do it without stepping on the package manager.

	- The installer symlinks the man page into the target's `man1` directory, mirroring the binary symlink exactly - the same trick, and it makes `man shcl` work as soon as the install directory is on `PATH`, since man derives its search path from the `bin` directories there.

	- The installer leaves the completions under the install directory and prints the line to paste for each shell. There is no one directory that works: bash's autoload directory varies with the bash-completion version, zsh needs a directory already on `$fpath`, and writing into the distribution's own would collide with the packaged copy. Printing the line is the same bargain the `PATH` note already strikes - plain about what was not done, with the fix one paste away.

	- Uninstall removes only what a matching install laid down, and the man symlink only when it points back into the install directory. One that does not came from a package, and removing it would break a working install.
