<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
# SHCL specification

Simple Hierarchical Config Language. This is the canonical language spec: terminology, lexical rules, structure, the read-time type/coercion model, raw blocks, the accessor and writer API, the canonical formatter, and the conformance strategy. The formal line/value grammar is in `grammar.abnf`; the raw by-example origin is in `../../notes.txt`; the settled decision log is in project memory.

## Design goals (the north star)

SHCL exists to be simultaneously the simplest and the most expressive config language, optimized for two audiences and nobody else:

- **The person writing config by hand.** It must be forgiving and obvious enough that a non-programmer can hand-author a whole file from scratch - even something as rich as a declarative DDL - without memorizing rules. If a modern parser can figure out what was meant, it must; the user is never asked to satisfy the machine.
- **The programmer consuming values.** However SHCL is pulled in - as a **Command** (CLI), a **Drop-in** source file, a **Package**, or a linked library (**Shared** or **Bundled**) - it must get you to "read the value I need" in one obvious call - amateur-friendly - and let you write out defaults and comments just as easily.

Everything hard - typing, coercion, disambiguation, error recovery, formatting - is the parser's job. The corollary rule for every ambiguous design call: **push the burden onto this program, never onto the user or the consumer, and impose no constraint the parser could have resolved from context when the input is not legitimately ambiguous.**

## Terminology

The word "key" is deliberately avoided - it implies uniqueness, which SHCL does not require. The mental model is a database, not a map.

- **Field** (a.k.a. column): the name on the left of a colon. A field is not unique; it may recur.
- **Value**: the text on the right of a colon (or an array of such, or a raw block).
- **Instance** (a.k.a. row/record): one occurrence of a field carrying a particular value. Repeating a field with a *different* value creates another instance, exactly like rows in a table.
- **Discriminator**: an instance's value, when used to tell instances of the same field apart (`base: Chicago` vs `base: Boston`).
- **Wrapper**: a field whose occurrences all have an empty value; they collapse into a single organizational node. A wrapper is just the degenerate (empty-discriminator) case of an instance.
- **Leaf**: a field occurrence with a value and no children.
- **Field path** (a.k.a. key path): the dotted chain of field names identifying a position in the tree, e.g. `base.metrics.population`. A path may resolve to many instances.

One rule unifies all of the above: **a node is `(field-name, value, children)`; nodes merge when their `(field-name, value)` pair matches; an empty value merges with other empty values under the same parent (that is a wrapper).**

The software side has its own nouns:

- **Parser**: turns SHCL text into the in-memory model. It carries all the hard work - deferred typing, disambiguation, error recovery.
- **Accessor**: the typed read layer a consumer calls to pull values back out (`GetInt`, wildcards, status sentinels). Typing is *accessor-driven* - the target type is fixed by which entry point you call, never stored in the file.
- **Writer**: the emit side - write values, defaults, and comment sections, and canonicalize a file.
- **Consumer**: the programmer using a binding, as opposed to the person hand-authoring the file.
- **Binding**: one language's implementation of the whole surface (parser + accessor + writer) - "the Go binding", "the Rust binding".

The Accessor reads in two modes:

- **Lookup** (a.k.a. query): fetch a single value at a field path - the TOML/YAML-style point read. Plain English: *get a value*.
- **Traversal** (a.k.a. scan): consume the document as a whole and iterate it - the mode for treating an SHCL file as a DDL or dumping every setting. Plain English: *walk the document*.
- **Materialize**: the step a traversal runs first - merge duplicate instances and order everything deterministically, so the walk is stable and repeatable.

## Lexical structure

### Encoding and lines

- Files are UTF-8. A leading UTF-8 BOM is stripped if present.
- Line endings may be LF or CRLF (both accepted); the canonical formatter emits LF.
- Trailing whitespace on a line is trimmed before parsing.
- The recommended extension is `.shcl`.

### Comments

- `#` begins a comment that runs to end of line, **only when outside quotes**.
- Comments may be a whole line or trail a value (`pop: 700  # note`).
- Blank lines are ignored.
- A `#` inside quotes is literal (`url: "http://h/#frag"`), and a `#` inside a raw block is literal.

### Whitespace, quoting, and reserved characters

- Whitespace around dots, colons, brackets, commas, and values is insignificant and trimmed. `a . b : "x"` == `a.b:"x"`.
- Quotes are optional. A value or name only *needs* quotes when it contains a **reserved character**: whitespace, `,` `:` `#` `"` `'` `[` `]`. (A `.` is reserved only in *field/path* position, not inside a value: `host: example.com` is fine bare.)
- Either single or double quotes may wrap a string. Programming-quote rules apply: an unescaped `'` is literal inside `"..."` and vice-versa.
- Outermost quotes are removed on read; whitespace *inside* the quotes is preserved.

### Escapes

- Inside any value (quoted or bare), `\` triggers escape processing: `\t` (tab), `\n` (newline), `\\` (backslash), `\"`, `\'`.
- The runic character `ᚠ` (U+16A0, "fehu") is the complement: it inserts literal backslash run(s) with **no** escape processing, sparing the user backslash-escaping madness. So `ᚠn` is the two literal characters `\n`, and `C:ᚠnew` is `C:\new`. A literal fehu is written `\ᚠ`.
- A value is always a single physical line; a newline *in* a value is written `\n`. (Multi-line verbatim content is a raw block instead.)

## Structure and hierarchy

Hierarchy is expressed two interchangeable ways; both produce identical trees.

### Indentation (block form)

- A line indented deeper than the previous line is its child. Indentation is **relative and stack-based**: any increase opens a level; a decrease must return to the exact column of an ancestor.
- A dedent to a column that matches no open level is a (recoverable) error - the line is diagnosed and skipped, the rest of the file continues.
- Indentation is tabs *or* spaces, consistent within a subtree. (Detection resets at each top-level ancestor, so distinct top-level trees could technically differ, but authors should just keep it uniform per file.)

### Dot and bracket (inline form)

- `a.b.c: v` is exactly `a:` / (indent) `b:` / (indent) `c: v`. The `.` stands in for "newline + one deeper indent".
- `field[disc]` selects (or creates) the instance of `field` whose discriminator value is `disc`, then continues the path under it. `base[Boston].metrics.population: 700` is identical to writing `base: Boston` then nesting `metrics` then `population: 700`. The colon before a selector is optional sugar, so `field[disc]` and `field:[disc]` are the same; the colon-less form is also the Accessor's lookup syntax, so a path reads identically whether authored in a file or passed to `Get`.
- Inline and block forms may be freely mixed; the parser normalizes both to the same tree.

### Merging and instances

- Occurrences with the same `(field-name, value)` merge; their children combine. This is how redundant paths collapse and how you add fields to an existing instance later in the file.
- Occurrences of a field with **different** values are distinct instances, kept in file order.
- A **repeated leaf line** (`tags: red` then `tags: blue`, no children) is two *instances* of `tags` - not the array `tags: red, blue`. Instances and arrays are separate mechanisms (see below).
- Field names are case-**insensitive** (`Metrics` == `metrics`). Discriminator **values** are case-**sensitive** after trimming and quote-stripping (`base: Chicago` and `base: chicago` are two instances).

## Values and types

**Typing is accessor-driven.** The parser stores every value as raw text and never guesses a type. The consumer knows its own schema and requests a type on read; the library coerces intelligently but safely, and reports problems without ever refusing to keep working. The value forms below are therefore *coercion targets recognized at read time*, not parse-time tags.

### Strings

Any value can be read as a string. On read: trim surrounding whitespace, strip the outermost quotes (keeping inner whitespace), and apply escapes. A string containing a reserved character must be quoted in the file.

### Integers

Recognized (case-insensitive) when a value is read as an integer:

- Optional sign `+`/`-`, then digits.
- A single leading currency symbol is ignored (`$1200` -> 1200). The accepted set is exactly these codepoints: `$ ¢ £ ¤ ¥ ₩ ₪ ₫ € ₭ ₮ ₱ ₲ ₴ ₹ ₺ ₼ ₽ ₾ ₿`. Multi-letter codes (`USD`, `kr`, `Fr`) are not stripped - they collide with barewords. Only one leading symbol is stripped; there is no trailing-currency form. Every binding hardcodes this exact list so results match byte-for-byte.
- Thousands separators are accepted only inside quotes, since `,` is reserved bare (`"1,000"` -> 1000).
- Hexadecimal integers `0x...` (`0xFF` -> 255).
- No digit-group underscores.

### Floats

Recognized when a value is read as a float:

- Optional sign; digits with a leading or trailing dot allowed (`.5`, `5.`, `3.14`).
- A leading currency symbol is stripped by the same rule as integers.
- Scientific exponent (`1e6`, `2.5E-3`).
- A trailing `%` yields the fraction (`50%` -> 0.5).
- An integer is a valid float on read.

### Booleans

Recognized (case-insensitive) when a value is read as a boolean:

- True: `true`, `t`, `yes`, `y`, `on`, `enable`, `enabled`, `1`.
- False: `false`, `f`, `no`, `n`, `off`, `disable`, `disabled`, `0`.
- `1`/`0` are boolean only when a boolean is requested; otherwise they are integers.

### Dates and times

Recognized when a value is read as a date/time. The parser accepts common formats with any delimiters (or a delimiter only between the date and time parts): numeric dates, short named months (`Jan`, `Jul`), 12- or 24-hour times with or without AM/PM and with or without milliseconds. A bare 8-digit number is `YYYYMMDD` and must be a valid calendar date. Fully-written-out English prose dates are **not** recognized. A genuinely ambiguous numeric date (e.g. `01/02/2003`, both fields <= 12) or an invalid date is an error on read (it can still be read as a string). Because typing is accessor-driven, `20260709` is only tried as a date when a date is requested; otherwise it is the integer 20260709.

### Arrays

A comma-separated value is an array held in a single cell. Splitting is on **unquoted** commas; each element is trimmed; quoted elements keep their internal commas and colons; a trailing comma is ignored; a single value is a one-element array; an empty value is an empty array. Element typing is accessor-driven like any value.

### Coercion rules ("intelligent but safe")

- int -> float: always.
- float -> int: **rounds** (`3.5` -> 4, `3.4` -> 3) rather than erroring. A `%` value is a fraction like any other float, so `GetInt` rounds it: `50%` -> 0.5 -> 1, `40%` -> 0.4 -> 0. It is deliberately not special-cased to the pre-`%` number, so `GetInt` and `GetFloat` never disagree.
- any scalar -> string: always.
- to boolean: only from the boolean token set (never numeric-nonzero; `5` is not `true`).
- No other lossy, silent conversion. A value that cannot be coerced to the requested type yields the requested failure behavior (below), not a thrown error.

## Raw blocks

A raw block embeds verbatim multi-line content - a DDL, a code snippet, a template - exempt from all SHCL rules (indentation, escapes, `#` comments, reserved characters). A file may contain any number of them.

- **Fenced, Markdown-style.** A block opens with a run of at least three identical fence characters, `` ``` `` or `~~~`. The opening run's character and length define the block; it closes at the first later line whose trimmed text is a run of the *same* character with length **>=** the opener. (So content may itself contain shorter fences.) An optional info-string may follow the opening fence (e.g. `~~~sql`); it is a free-form advisory label - captured and exposed to the consumer (a raw-block accessor returns it), but never interpreted by the parser. No values are reserved; a consumer may treat it as a content-type hint if it wants.
- **Named form:** `path.name: ~~~` ... `~~~`. The block is that field's value, read via `Get`/`GetRaw`.
- **Anonymous form:** a bare fence line at child indentation, content, closing fence. It attaches to the parent path positionally and is addressed by index (`notes[0]`).
- **Indentation:** the block is visually nested at a child indent for clarity. The parser strips the block's common leading indentation (the nesting) and preserves the relative internal indentation. Content `a` then `  b` under nesting yields the value `"a\n  b"`.

```
config:
	ddl: ~~~sql
		CREATE TABLE users (
			id   INTEGER PRIMARY KEY,
			name TEXT NOT NULL
		);
	~~~
	notes:
		~~~
		free-form paragraph, kept verbatim,
		    including this deeper indent.
		~~~
```

## Consumer API

The library is uniform across languages; each binding realizes the same concepts idiomatically. Planned bindings: Go, Rust, C (+ C++), C#, Java (+ Kotlin), Python, JavaScript (+ TypeScript), PowerShell, POSIX sh. Aim for cross-language consistency with reasonable compromises, not for maxing out each language.

The consumer-facing surface has two halves: the **Accessor** reads values (by lookup or traversal), and the **Writer** emits them.

Three of these are not separate implementations but a base core plus a thin **companion typed surface** - one parser, two call surfaces:

- **C++ over the C core**: the C source, its public header wrapped in `extern "C"`, plus a header-only C++ template veneer (`Get<T>()` over the typed C functions). C is not a strict subset of C++, so the shared header is kept C++-clean; only the `.c` need compile as C.
- **Kotlin over the Java core**: Kotlin calls the Java classes directly via JVM interop with no runtime work; the companion is a small extensions file giving `reified`-generic `get<T>()` instead of Java's `Class<T>` token form.
- **TypeScript over the JavaScript core**: one `.js` implementation plus a hand-authored `.d.ts` whose overloads/generics realize the typed entry points. TS support means the declaration file, not a second port - and the JS API must be shaped so those declarations can be precise (not a single `Get` returning `any`).

### The core call

The conceptual operation is **"get the value at `path`, coerced to a target type, with a default and an on-bad policy."** The critical portability rule: **the target type is expressed by the entry point (a typed variant or a compile-time generic), never by a runtime field in an options object.** This is the only shape that lands directly in a strongly-typed variable with no consumer-side cast in *every* target language. A runtime `type` value cannot drive a static language's return type (Go, Rust, C, C++, C# all forbid it; Java can only via a `Class<T>` token, TypeScript only via overload typing), so we do not rely on it.

- **type**: chosen by which method/generic you call - `GetInt` / `GetFloat` / `GetBool` / `GetDateTime` / `GetString` / `GetRaw` and their array forms, or a generic `Get<T>` where idiomatic (Rust always; Go/C++/C# optional). Realizations: Go typed methods or generics; Rust trait + turbofish/inference; C typed functions with out-param + status; C++ templates (`Get<T>`) over those C functions; C# explicit generics; Java `get(path, Integer.class, ...)`; Kotlin `reified`-generic `get<T>()` extensions over the Java methods; Python `get_int(...)` (or `get(..., type=int)` since it is dynamic); JS typed methods with `.d.ts` overloads/generics typing them for TS; PowerShell typed variable coercion on assign; POSIX sh a single command returning text (type flag only *validates*).
- **default** and **onBad** ride along as options/parameters on each of those.
- **onBad**: how to react to a bad/empty/missing/ambiguous value - `Error` (surface it), `Default` (substitute the default), or `Flag` (return the zero/empty value plus a soft indicator, never erroring). Regardless of choice, the Accessor can retrieve the original raw text of the offending value. When unspecified, the mode is **`Flag`** - the most forgiving, so a caller who ignores the status still never gets a thrown error.

The three modes are the canonical surface everywhere; each binding names them idiomatically: an enum parameter where the language has enums (Go/Rust/C/C++/C#/Java/PowerShell), a small string/keyword arg where it does not (Python `on_bad=`, JS `{onBad}`, sh `--on-bad=`). `Flag` mode returns the status alongside the value by each language's natural multi-value idiom (Go multi-return, Rust `Result`/tuple, C out-param + code, others a small result struct).

This accessor-controlled error handling applies to **all** value access, not just arrays.

### Ergonomic tiers

The consumer is assumed to be a junior programmer in **every** binding, so each typed entry point comes in two tiers:

- **Convenience tier (the one the docs lead with).** One value, one baked-in fallback, one return, no status to inspect. Supplying the fallback *is* choosing `Default` on-bad mode, so an empty/missing/bad/ambiguous read yields the fallback and nothing throws. This is the call a beginner writes 90% of the time.
- **Full tier.** The same read exposing the status sentinel (and the raw text) for callers who must tell `Empty` from `NotFound` from `BadType`, or who want `onBad: Error`.

The convenience tier has the same shape everywhere - a mandatory, call-site-visible fallback - which is precisely what defuses the silent-zero trap: a junior cannot accidentally read a `0`/`""` that was really empty or missing, because there is no convenience call without a stated fallback. Each binding names the tiers by its own convention; in languages that already have a native "value-or-default" idiom, that idiom *is* the convenience tier rather than a new method.

| Language   | Convenience tier                              | Full tier                                          |
|------------|-----------------------------------------------|----------------------------------------------------|
| Go         | `pop := doc.GetIntOr(path, 0)`                | `pop, st := doc.GetInt(path)`                      |
| Rust       | `let pop = doc.get_int(path).unwrap_or(0);`  | `let r = doc.get_int(path); // Result<i64, Status>`|
| C          | `int pop = shcl_get_int(doc, path, 0);`      | `shcl_get_int_ex(doc, path, &pop); // -> status`   |
| C++        | `int pop = doc.get_or<int>(path, 0);`        | `auto r = doc.get<int>(path); // .value / .status` |
| C#         | `int pop = doc.GetIntOr(path, 0);`           | `var r = doc.GetInt(path); // .Value / .Status`    |
| Java       | `int pop = doc.getIntOr(path, 0);`           | `var r = doc.getInt(path); // .value() .status()`  |
| Kotlin     | `val pop = doc.getIntOr(path, 0)`            | `val r = doc.getInt(path)`                         |
| Python     | `pop = doc.get_int(path, default=0)`         | `r = doc.read_int(path)  # r.value, r.status`      |
| JS / TS    | `const pop = doc.getIntOr(path, 0)`          | `const r = doc.getInt(path)  // {value, status}`   |
| PowerShell | `[int]$pop = $doc.GetIntOr($path, 0)`        | `$r = $doc.GetInt($path)  # .Value .Status`        |
| POSIX sh   | `pop=$(shcl get --int --default=0 f 'path')` | `shcl get --int f 'path'; status=$?`               |

The array, bool, float, datetime, string, and raw forms follow the same two-tier pattern (`GetIntArrayOr`, `GetBoolOr`, ...); only the coercion target changes. The full tier is one representation of the `Flag`-mode status described above; the convenience tier is `Default` mode with the fallback the caller passed.

### Status sentinels

Reads report one of: **Good**, **Empty** (present but no value), **NotFound** (path does not resolve), **BadType** (present but not coercible to the requested type), **Multiple** (path resolves to more than one instance and the call wanted one). `Empty` is informational, not a failure - the empty value is still returned. The parser **never** refuses a legitimately reachable value because some *other* part of the file was malformed.

### Lookup and traversal

Two ways to read a document:

- **Lookup** (query) - get a single value at a path, the point read most config code wants (`doc.GetIntOr("base[Boston].metrics.population", 0)`). The core call and tiers above are all lookups.
- **Traversal** (scan) - consume the file as a whole: **materialize** it first (merge duplicate instances, order deterministically), then walk it. This is the mode for treating an SHCL file as a DDL or dumping every setting. The wildcards, `Instances`, and `Count` below are its building blocks.

Materialization is idempotent and order-stable, so two traversals of the same document walk it identically.

### Paths, selectors, and traversal

- A lookup path uses the same syntax and tolerances as the file: case-insensitive field names, dots to descend, `[sel]` to select an instance, whitespace ignored (`base [ Boston ] . population`).
- Selector forms: `[Boston]` (bare non-numeric = value), `[0]` (bare numeric = index), `["2020"]` (quoted = value even if numeric), `[#2]` (explicit index).
- `field[*]` is a wildcard returning every instance's value as an array: `GetIntArray("base[*].metrics.population")`. The result is positionally aligned to the instances - one slot per instance, in file order. If an instance lacks the sub-path, its slot is kept (status `NotFound`, taking the zero/default per `onBad`); slots are never silently dropped, so indices stay aligned with `Instances(field)`/`Count(field)`. A legitimately absent sub-path is not malformed, so it produces no diagnostic.
- `Instances(field)` and `Count(field)` enumerate instances by value or index.
- An ambiguous single-value read (path resolves to many instances) reports `Multiple`; narrow it with a selector until exactly one remains.

### Diagnostics and writing

- Loading also yields a list of structured **diagnostics** (line number + reason) for every skipped or repaired line, which the consumer may inspect or ignore.
- The **Writer** handles the reverse of the Accessor: emit values, defaults, and comment sections, and canonicalize a file (see below).

## Canonical formatter

The formatter normalizes structure only - it cannot know value types, so it never rewrites value text (no `.5` -> `0.5`, no `50%` change).

- Block (indented) form, tabs for indentation.
- Preserve file/insertion order of instances and fields (so index-based access stays stable).
- Collapse and merge redundant sections and paths.
- Quote a value only when a reserved character requires it (minimal quoting).
- Leave scalar text exactly as authored; raw blocks are re-emitted verbatim.

## Error handling philosophy

SHCL never bails on a whole file for one bad line. The parser skips or best-effort-repairs the offending line, records a diagnostic, and continues. The Accessor never errors when it can unambiguously reach a value; malformed content before or after a clean section does not poison that section. Errors are reserved for genuine ambiguity (or surfaced on request via `onBad: Error`).

One repair is defined concretely, because it is the common "figure it out" case: a line that is a **well-formed field path with no colon and nothing after it** (`base[Boston].metrics.population`, no value) is repaired to that path carrying an **empty value** - the obvious intent - with a diagnostic recorded. This is deliberately narrow. A line whose colon is missing but which is *not* a clean path - a bareword then whitespace then another token (`square-miles 300`) - is genuinely ambiguous (is `300` a value, or part of a name that cannot legally contain a space?), so it is skipped with a diagnostic rather than guessed.

## Cross-language parity and conformance

All independent bindings (Go, Rust, C (+ C++), C#, Java (+ Kotlin), Python, JavaScript (+ TypeScript), PowerShell, POSIX sh) must agree byte-for-byte; a companion surface inherits its core's conformance for free. The safeguards:

- This spec plus `grammar.abnf` are the single source of truth; behavior is specified, not left to each implementation.
- A **conformance corpus** of golden cases (`conformance/`) pins every implementation to identical results: each case is an input `.shcl`, its expected canonical formatting, and a set of expected typed reads with their status sentinels. The date/coercion/quoting edge cases live here so no parser can silently drift.
- Every parser runs the corpus in CI before it is considered conformant.

## Resolved minor items

These were previously deferred and are now settled inline (above):

- **Currency set**: a fixed 20-codepoint list, single leading symbol only (see Integers).
- **`field[*]` with a missing sub-path**: keep the positional slot, per-element `NotFound`, no diagnostic (see Paths, selectors, and enumeration).
- **`onBad` surface**: canonical `Error`/`Default`/`Flag`, idiomatic enum per language, default `Flag` (see The core call).
- **`GetInt` on a `%` value**: rounds the fraction, not special-cased (see Coercion rules).
- **Fence info-string**: free-form advisory label, exposed but never interpreted (see Raw blocks).
