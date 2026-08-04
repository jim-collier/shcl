# Changelog

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## v1.1.0 - 2026-08-04

### Added

- Name-position wildcard `*` in lookup paths: `Get("indicators.*.period")` slots across children of any name the way `[*]` slots across instances (per-slot statuses, `Count`/`Instances` aligned), and a schema path can declare an open section - "any child name here, each shaped like this" - without enumerating names. The setters refuse a wildcard path, while `Remove` on one removes every matching slot; a field literally named `*` stays addressable quoted (`"*"`).

- Schema fragments: `fragment: <name>` declares a reusable shape (ordinary `field:` entries with relative paths), `inherits: <name>` mounts it at a field's path. Recursive and mutually-referencing shapes are legal with no depth limit - validation follows a mount only where the document has nodes - so arbitrarily-nesting structures (layout trees and the like) validate at any depth, and one shape can be mounted at several paths instead of duplicating its fields. `shcl init` expands mounts into the starter config, cutting where a fragment would re-enter itself. New schema-fault codes `V094` (bad fragment declaration) and `V095` (`inherits` names no declared fragment).

- `QuoteSegment` in every binding and the C++ veneer: quotes one path segment so a user-typed name can be spliced into a lookup path safely - without it, a dotted name silently reads as nesting.

- `Children(path)`: child field names in file order, duplicates included, so an open (map-shaped) section can finally be read without prefix-scanning `Paths()`. The empty path enumerates the top level.

- `Line(path)`, plus `line` and `quoted` on the read result (Rust/Go/Python; C keeps its value+status structs and gains the `shcl_line`/`shcl_children` accessors): consumer-side checks can cite the source line, and a quoted value is distinguishable from a bare one - `a: @null` vs `a: "@null"`.

- `LoadAndValidate(text, schema, strictness)`: parse and validate in one call, returning the document with one combined diagnostics list - no more hand-merging two lists and losing half the errors - plus `ErrorCount()` as the "did this file have errors?" predicate.

- A new `H002` hint when a binding merges with a non-adjacent earlier one (two separately-written `table: t` sections silently becoming one combined table is now visible; the prose names the earlier line). Adjacent re-mentions and selector/path-intermediate merges - the deliberate redundant-path idiom - stay silent.

- `check --schema` now drops `H001` (repeated bare leaf) for fields whose schema declares a repeat upper bound above 1: there, repetition is the instance mechanism by declaration, and the hint was training users to ignore hints.

- `SuppressDeclaredRepeats(schema, diags)` in every binding: drops the repeated-field hint for any field whose schema allows more than one instance - the same filter `check --schema` applies, callable wherever document diagnostics and a schema meet.

- `WriteReason(path)`: the reason a write at a path would fail - `BadPath`, `ValueInPath`, `Wildcard`, `NoSuchIndex`, or `TooDeep` - behind the setters' bare pass/fail. A probe only; it never creates.

- `V096`: generating a starter config from a schema whose fragments mount each other along several paths now reports a schema fault instead of expanding until it runs out of memory.

- `shcl set --write FILE --set PATH=VALUE` writes edits given as options straight back to the file. Previously the only way to persist an edit was a tab-separated op script piped in on stdin, which is awkward to write by hand in any shell and impossible to read once tabs are invisible; edits now need neither a pipe nor a tab. The options are repeatable and apply in the order given.

- `--set-literal=PATH=TEXT` beside it, for the values `--set` cannot spell. A `--set` value is data, so `ports=80, 443` stores one quoted string; the same text through `--set-literal` stores a two-element array, because it goes in as value syntax the way a file spells it. Both share one ordered list, so the last option to touch a path wins. Raw blocks, set-only-if-absent and removal still go in as an op script.

- `SetLiteral` and `SetLiteralDefault` in every binding (`set_literal` / `shcl_set_literal`), the library half of the above: they read their argument the way the parser reads the value half of a line, so a consumer holding value text can write it without first working out its shape. Text carrying a line break, or a quote that never closes, is rejected rather than written; an unquoted `#` ends the value as it would in a file. The op script gained a matching `literal` op.

- Generated starter configs (`shcl init --schema`) end with a short footer naming the format and linking its home page and spec, so whoever opens the file next can find out what it is and how to edit it. `--no-banner` leaves it out, and `generate` takes the same flag in every binding.

### Changed

- `generate` gained a `no_banner` argument in every binding (Python and the C++ veneer default it; Rust, Go and C callers must pass it). The flag is negative on purpose: passing false, zero, or nothing writes the footer, so the opt-out is the thing a caller has to ask for.

- The Go module's `go` directive dropped from 1.24 to 1.20, the tested floor (`strings.CutSuffix` is the newest stdlib dependency) - so pinned-toolchain projects can use the module instead of vendoring the file.

- `shcl set` no longer reads a write-ops script from stdin when any `--set` is given. This only affects a command line that passed both, which previously applied the `--set` values and then the piped ops; pass the whole edit one way or the other. The wording of the two `--write` refusals also split, naming just the option that clashed rather than "--layer or --set".

- Writer-created top-level nodes now carry the blank-line grouping a hand-written file would have: one blank line between top-level sections. Written and generated output changes shape accordingly.

- Parsing is faster: the reference no longer copies every line and its indent, and three bindings scan paths without building a character list per line. Formatting a large file is roughly 10 percent quicker in the reference, Go and C, and about 12 percent in Python.

### Fixed

- The canonical formatter no longer loses hand-authored comment layout two ways: a blank line between comment-only regions survives the round-trip, and a comment written deeper than the next binding stays with the block it sits in (re-emitted after that block's last child, at the block's indent) instead of re-attaching dedented to the next field.

- A failed strict load is no longer a dead end: the failure carries the parsed document (Go's `ParseWith` also returns it non-nil alongside the error), and the message names the first few diagnostics instead of a bare count.

- `Paths()` no longer hides a node whose name needs quoting, nor its subtree. Non-bare segments come back quoted and escaped, so every returned path is a valid lookup path and the enumeration matches the document.

- A read's raw text is now the value span exactly as authored on its source line, as documented - not the canonical display form, which silently rewrote `^\d{2,3}$` to `^\d{2, 3}$`. Values with no one-line source spelling (writer-built, stacked lists, raw blocks) keep the display-form fallback.

- By-value selectors now apply escape sequences to both sides before comparing, so `["q\"uote"]` finds an instance written `'q"uote'`. Previously the comparison was spelling against spelling and the mismatch was a silent `NotFound`; the writer's placement walk had the same blind spot and could create a spurious second instance.

- Formatting a file could change what it meant. A value that only became final after its siblings were keyed - a stacked list closing onto an earlier instance's value, or a fenced block filling an empty field that matched an earlier one - was left beside its match instead of merging, so the file formatted to two entries, reformatted to one, and a read that correctly reported several instances started returning a value.

- Rewriting a file in place created its temporary under a predictable name without demanding exclusive creation, so anything already sitting there - including a symlink someone else had planted - was written through, and the rename then turned the config itself into a link. The temporary is now created exclusively and given the target's permissions before any data goes into it, so a private config is never briefly readable to anyone else either.

- Validating against a recursive schema could stop finishing, because a shape reached from two paths was re-checked per path per level. Each shape is now checked once per node. The C binding also held every level's working data until the end; that is released as it goes now.

- Python raised on numerals beyond a few thousand digits where the other bindings report a bad type. Length is checked before conversion now.

- The C date formatter could write past the buffer size it documents when handed a value the caller built rather than one that came from parsing.

- The C++ wrapper's structured date read handed back a pointer into the document's storage, so the value dangled once the document was gone. It owns its fractional digits now, which changes what `read_datetime_raw` returns.

- Comments were dropped when documents merged: a section present in both layers lost the higher layer's comments, and a write that merged duplicate fields lost any comment hanging below the losing one. A footer several layers share is now carried over once rather than repeated per layer.

- The one-shot load-and-validate ignored a schema that failed to load, so constraints on its broken lines silently vanished. It reports the schema failure now, like `check --schema` does.

- A value or a path that read like `-h` or `--version` printed help or the version and exited successfully. Only a flag in option position counts now, and `--` ends the options so a path may begin with a dash.

- Rewriting a file in place while merging layers folded those layers permanently into it. The combination is refused.

- Generated starter configs did not always load: a wildcard written with spaces or with the alternate colon spelling produced a line the parser rejects, and a deep chain of fragments produced paths past the nesting limit.

- Three C entry points left working data in documents they do not own, growing a long-running program that holds one parsed schema.

- The Go repeat-hint filter rewrote the caller's list while returning a new one.

- Quoting a name that ends in a backslash produced a path that could not be read back.

- Repeat-hint suppression could silence an unrelated field when a schema path ended in a quoted name containing a dot.

- A field name containing a null byte could satisfy a two-segment schema path, slipping past the unknown-field check.

- A system install linked the program into a directory that is not on a normal user's path.

## v1.0.0 - 2026-07-28

First stable release. The language, the read and write APIs, and the diagnostic codes are now covered by semantic versioning: a breaking change to any of them means 2.0.0.

### Added

- Published packages. Rust on crates.io (`cargo add shcl`), Python on PyPI (`pip install shcl`), and the Go module by tag (`github.com/jim-collier/shcl/source/go`). Binding versions move in lockstep with the product version, so each ecosystem's ordinary compatible-version operator tracks minor and patch releases without crossing a major.

- Signed releases. The `sha256sums` file now comes with a detached signature made offline with the project signing key, and both install scripts verify it before trusting any checksum out of that file. The public key is at `shcl-signing.pub` for manual verification.

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
