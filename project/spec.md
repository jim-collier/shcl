<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
# SHCL specification

Simple Hierarchical Config Language. This is the canonical language spec: terminology, lexical rules, structure, the read-time type/coercion model, raw blocks, the accessor and writer API, the canonical formatter, and the conformance strategy. The formal line/value grammar is in `grammar.abnf`; the raw by-example origin is in `../../notes.txt`; the settled decision log is in project memory.

## Design goals (the north star)

SHCL aims to be forgiving to write, predictable to read, and expressive enough for any flat or hierarchical data - with the friendliest read API in the space. (Not "the simplest possible language": the grammar has real features, and pretending otherwise just invites the comparison. The simplicity claim lives where it is true - in what the two audiences below actually experience.) It is optimized for those two audiences and nobody else:

- **The person writing config by hand.** It must be forgiving and obvious enough that a non-programmer can hand-author a whole file from scratch - even something as rich as a declarative DDL - without memorizing rules. If a modern parser can figure out what was meant, it must; the user is never asked to satisfy the machine.

- **The programmer consuming values.** However SHCL is pulled in - as a **Command** (CLI), a **Drop-in** source file, a **Package**, or a linked library (**Shared** or **Bundled**) - it must get you to "read the value I need" in one obvious call - amateur-friendly - and let you write out defaults and comments just as easily.

Everything hard - typing, coercion, disambiguation, error recovery, formatting - is the parser's job. The corollary rule for every ambiguous design call: **push the burden onto this program, never onto the user or the consumer, and impose no constraint the parser could have resolved from context when the input is not legitimately ambiguous.**

## Terminology

The word "key" is deliberately avoided - it implies uniqueness, which SHCL does not require. The mental model is a database, not a map.

- **Field** (a.k.a. column): the name on the left of a colon. A field is not unique; it may recur.

- **Value**: the text on the right of a colon (or an array of such, or a raw block).

- **Instance**: one occurrence of a field carrying a particular value. Repeating a field with a *different* value creates another instance. (*Row* and *record* are synonyms, listed here only as a glossary pointer; the docs and API use "instance" throughout, and "record" is never used as a verb - the write-side verb is "write".)

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

- Comments are never discarded: the parser carries each one as trivia attached to the tree, and the canonical formatter re-emits them (see Canonical formatter). They play no part in merging, reads, or diagnostics.

### Whitespace, quoting, and reserved characters

- Whitespace around dots, colons, brackets, commas, and values is insignificant and trimmed. `a . b : "x"` == `a.b:"x"`.

- Quotes are optional. A value or name only *needs* quotes when it contains a **reserved character**: whitespace, `,` `:` `#` `"` `'` `[` `]`. (A `.` is reserved only in *field/path* position, not inside a value: `host: example.com` is fine bare.)

- Either single or double quotes may wrap a string. Programming-quote rules apply: an unescaped `'` is literal inside `"..."` and vice-versa.

- Outermost quotes are removed on read; whitespace *inside* the quotes is preserved.

### Escapes

- Inside any value (quoted or bare), `\` triggers escape processing: `\t` (tab), `\n` (newline), `\\` (backslash), `\"`, `\'`.

- Backslash-heavy content (Windows paths, regexes) should go in a raw block, which is fully verbatim - that is the escape hatch, so no anti-escape gimmick is needed in values. Doubling backslashes in a one-line value (`C:\\new`) is always available.

- A value is always a single physical line; a newline *in* a value is written `\n`. (Multi-line verbatim content is a raw block instead.)

## Structure and hierarchy

Hierarchy is expressed two interchangeable ways; both produce identical trees.

### Indentation (block form)

- A line indented deeper than the previous line is its child. Indentation is **relative and stack-based**: any increase opens a level; a decrease must return to the exact column of an ancestor.

- A dedent to a column that matches no open level is a (recoverable) error - the line is diagnosed and skipped, the rest of the file continues.

- Indentation is tabs *or* spaces, consistent within a subtree. (Detection resets at each top-level ancestor, so distinct top-level trees could technically differ, but authors should just keep it uniform per file.)

### Dot and bracket (inline form)

- `a.b.c: v` is exactly `a:` / (indent) `b:` / (indent) `c: v`. The `.` stands in for "newline + one deeper indent".

- `field[disc]` selects (or creates) the instance of `field` whose discriminator value is `disc`, then continues the path under it. `base[Boston].metrics.population: 700` is identical to writing `base: Boston` then nesting `metrics` then `population: 700`. The colon before a selector is optional sugar, so `field[disc]` and `field:[disc]` are the same; the colon-less form is also the Accessor's lookup syntax, so a path reads identically whether authored in a file or passed to `Get`. Matching is against the instance's **display form** (elements joined with `, `) in both places, so a selector also selects an array-valued instance (`base[Boston, MA]` finds `base: Boston, MA`); a new instance is created only when nothing matches.
	- A selector on the **last** path segment already supplies that instance's value (the discriminator), so a trailing value has nowhere to bind: `field[disc]: value` is grammar-legal but not a valid binding. The instance is still selected/created from the discriminator, and the trailing value is reported as an `error` diagnostic and dropped. Being an `error`, it fails a Strict load like any other. (A value is fine after a selector that is *not* last, e.g. `base[Boston].population: 700`, where it binds the deeper leaf.)

- Inline and block forms may be freely mixed; the parser normalizes both to the same tree.

### Merging and instances

- Occurrences with the same `(field-name, value)` merge; their children combine. This is how redundant paths collapse and how you add fields to an existing instance later in the file.

- Occurrences of a field with **different** values are distinct instances, kept in file order.

- A **repeated leaf line** (`tags: red` then `tags: blue`, no children) is two *instances* of `tags` - not the array `tags: red, blue`. Instances and arrays are separate mechanisms (see below).

- Field names are case-**insensitive**, folding **ASCII `A-Z`/`a-z` only** (`Metrics` == `metrics`). Non-ASCII characters in names never fold (`Straße` != `STRASSE`, `İ` != `i`) - full Unicode case folding is locale-trapped (Turkish dotless-I) and unportable across bindings, so it is deliberately excluded. Discriminator **values** are case-**sensitive** after trimming and quote-stripping (`base: Chicago` and `base: chicago` are two instances).

## Values and types

**Typing is accessor-driven.** The parser stores every value as raw text and never guesses a type. The consumer knows its own schema and requests a type on read; the library coerces intelligently but safely, and reports problems without ever refusing to keep working. The value forms below are therefore *coercion targets recognized at read time*, not parse-time tags.

### Strings

Any value can be read as a string. On read: trim surrounding whitespace, strip the outermost quotes (keeping inner whitespace), and apply escapes. A string containing a reserved character must be quoted in the file. A multi-element array read as a single string yields its **canonical inline form** - elements minimally quoted, escapes intact, joined with `, ` - so the string re-parses to the same array; per-element unquoting and escapes belong to the array-of-strings read.

### Integers

Recognized (case-insensitive) when a value is read as an integer:

- Optional sign `+`/`-`, then digits.

- Thousands separators are accepted only inside quotes, since `,` is reserved bare (`"1,000"` -> 1000).

- Hexadecimal integers `0x...` (`0xFF` -> 255).

- No digit-group underscores.

- No currency handling: `$1200` is a string, `BadType` as an integer. (The Loose strictness level re-admits a fixed leading-symbol list; see Strictness levels.)

### Floats

Recognized when a value is read as a float:

- Optional sign; digits with a leading or trailing dot allowed (`.5`, `5.`, `3.14`).

- Scientific exponent (`1e6`, `2.5E-3`).

- An integer is a valid float on read.

- No currency and no percent handling: `$3.14` and `50%` are strings, `BadType` as floats. (The Loose strictness level re-admits both; see Strictness levels.)

### Booleans

Recognized (case-insensitive) when a value is read as a boolean:

- True: `true`, `yes`, `on`, `1`.

- False: `false`, `no`, `off`, `0`.

- `1`/`0` are boolean only when a boolean is requested; otherwise they are integers.

- Anything else - including `t`, `y`, `enabled` - is `BadType`. (Strict narrows the set to `true`/`false` only; Loose widens it; see Strictness levels.)

### Dates and times

Recognized when a value is read as a date/time. The formats are a closed whitelist, each pinned by a conformance case; the admission rule is that a format cannot be misread - either the year comes first, or the month is a word. Anything else fails the read as a date with `BadType` (the value is still readable as a string). Matching a format is not enough: the value must then also be a real calendar date and clock time (`2026-02-30` or `25:00` is `BadType` despite matching a shape).

**Dates** - 4-digit year always. The internal delimiter is one of `-` `/` `.` and must be uniform within the date:

- `YYYY-MM-DD`: `2026-07-12`, `2026/07/12`, `2026.07.12`.

- `YYYYMMDD` - compact 8-digit.

- `DD Mon YYYY` - space or a delimiter between components: `12 Jul 2026`, `12-Jul-2026`, `12.July.2026`.

- `Mon DD YYYY` - likewise: `Jul 12 2026`, `Jul/12/2026`; in the space form a comma may follow the day (`Jul 12, 2026`).

Month names are the fixed English set only - 3-letter abbreviation or full name, case-insensitive (`jan`, `Jan`, `JANUARY`), no trailing dot, no other languages (a locale table is how bindings drift).

**Times:**

- 24-hour `HH:MM` and `HH:MM:SS`.

- Optional fractional seconds after seconds, `.` delimiter, 1-9 digits (`14:30:05.123`) - unambiguous because it can only follow `HH:MM:SS`.

- 12-hour with mandatory minutes and mandatory meridiem: `H:MM AM` / `h:mm:ss pm`, case-insensitive, space before AM/PM optional (`2:30PM`); dotted `a.m.` is rejected.

**Timezone suffix** (optional, after any time): `Z` or `+`/`-``HH:MM`. Named zones (`EST`, `Europe/Paris`) are rejected - they require a timezone database, the ultimate parity killer.

**Combined:** `<date><sep><time>[zone]`. The separator is `T`, a single space, `_`, or one of `-` `/` `.` where it does not create ambiguity - the time's `:` ends the date reading, so `2026-07-12-14:30` and `20260712.14:30` are fine, and the separator need not match the date's internal delimiter. Date-only and time-only values are each valid alone. Without a zone suffix the value is a *local* (floating) date/time - each binding returns its idiomatic local type; it is never silently assumed UTC.

**Rejected by decision, not omission:** `MM/DD/YYYY` and `DD/MM/YYYY` (the motivating ambiguity) and every other all-numeric date that is not year-first; 2-digit years; Unix epoch numbers (a consumer wanting epoch reads the integer and converts); fully-written-out prose dates ("July twelfth"). Because typing is accessor-driven, a bare 8-digit number is tried as `YYYYMMDD` only when a date is requested; otherwise `20260712` is the ordinary integer 20260712.

### Arrays

An array is multiple values in a **single cell**. It has two interchangeable spellings that produce the identical array; the canonical formatter emits the inline form.

**Inline (comma) form** - `tags: red, green, blue`. Splitting is on **unquoted** commas; each element is trimmed; quoted elements keep their internal commas and colons; a single value is a one-element array; an empty value is an empty array. **Empty elements are dropped** - a leading, doubled, or trailing comma contributes nothing (`red,,blue` is `[red, blue]`, `red,` is `[red]`), and a value that is only commas and whitespace (`,`, `, ,`, `,,,`) is the empty array. None of these is an error. To carry a deliberately empty element, quote it (`red, "", blue`).

**Stacked (`*`) form** - an empty-valued field whose child lines are all `*`-marked is the same array, one element per line:

```text
sizes:
	* small
	* medium
	* "extra, large"
```

is exactly `sizes: small, medium, "extra, large"`. The rules that keep it unambiguous:

- The marker is `*` then at least one space. A stacked element is always **colon-less**, and that (not the space) is what separates `* small` (an element) from `*x: y` (a field) - a `*` can never begin a field name.

- **Uniform or nothing:** at the child indent, every line is a `*` element or none is; a mix is not a block array (each line is parsed as whatever it is, with a diagnostic if malformed).

- **One element per line**, each a single scalar typed on read exactly like an inline element; quote to embed a space, comma, or colon (`* "Bond, James"`). A bare comma on an element line is an error, not a second element.

- The field opening the list has an **empty inline value**; a field may not carry both an inline value and a `*` list.

- **Scalars only** (first cut): a `*` element is a value, not a sub-node, so it has no `field: value` children. Arrays *of objects* are expressed with instances and discriminators, not `*` lists.

Element typing is accessor-driven, so `GetStringArray`/`GetIntArray` read either spelling identically.

An array (either spelling) is **one cell holding many values** - not the same as repeating a field on separate lines, which makes distinct **instances** of that field. The two look alike only for a bare repeated leaf; once an instance carries children the intent is plain. A parser **must** emit a hint diagnostic on a bare repeated leaf (`did you mean tags: red, blue?`) and never silently treats one form as the other. The hint is advisory (severity `hint`, not `error`) - repeated leaves are legal and are the instance mechanism working as designed; the diagnostic just makes the lookalike impossible to hit unknowingly.

### Coercion rules ("intelligent but safe")

- int -> float: always.

- float -> int: **`BadType`**. `3.5` read as an int is not silently rounded - silent lossy conversion is exactly what adopters distrust. It is still readable as a float or a string. (The Loose strictness level re-admits rounding; see Strictness levels.)

- any scalar -> string: always.

- to boolean: only from the boolean token set (never numeric-nonzero; `5` is not `true`).

- No other lossy, silent conversion. A value that cannot be coerced to the requested type yields the requested failure behavior (below), not a thrown error.

## Raw blocks

A raw block embeds verbatim multi-line content - a DDL, a code snippet, a template - exempt from all SHCL rules (indentation, escapes, `#` comments, reserved characters). A file may contain any number of them.

- **Fenced, Markdown-style.** A block opens with a run of at least three identical fence characters, `` ``` `` or `~~~`. The opening run's character and length define the block; it closes at the first later line whose trimmed text is a run of the *same* character with length **>=** the opener. (So content may itself contain shorter fences.) An optional info-string may follow the opening fence (e.g. `~~~sql`); it is a free-form advisory label - captured and exposed to the consumer (a raw-block accessor returns it), but never interpreted by the parser. No values are reserved; a consumer may treat it as a content-type hint if it wants.

- **Binding: a fence is a value line for its parent field.** A fence line at child indent binds the block as its parent field's value. If the parent's value is empty, the block fills that instance's value; if the parent already carries a value (or already received a block), the fence creates a **new instance** of the parent field with the block as its value - the same rule as a repeated leaf line. There is no separate "anonymous block" concept: blocks are instances, selected with the normal selectors (`notes[0]`, `notes[#2]`), and identical `(field-name, value)` blocks merge like any other instances. A block's identity is its content **plus its info-string** (a `sql` and a `python` block are distinct even with equal bodies); the fence character and length are spelling, not identity.

- **Same-line spelling:** the fence may also open on the field's own line (`path.name: ~~~sql` ... close fence). Identical tree; the child-indent spelling is canonical. The info-string rides the fence line in both spellings.

- **Reading:** `Get`/`GetRaw` at the field path. A path resolving to several block instances reports `Multiple` on a single-value read, like any other field.

- **Indentation:** the block is visually nested at a child indent for clarity. The parser strips the block's common leading indentation (the nesting) and preserves the relative internal indentation. Content `a` then `  b` under nesting yields the value `"a\n  b"`.

```text
config:
	ddl:
		~~~sql
		CREATE TABLE users (
			id   INTEGER PRIMARY KEY,
			name TEXT NOT NULL
		);
		~~~
	notes: ~~~
		free-form paragraph, kept verbatim,
		    including this deeper indent.
		~~~
```

Both blocks bind as values: `GetRaw("config.ddl")` returns the DDL, `GetRaw("config.notes")` the paragraph. No wrapper, no index needed.

## Consumer API

The library is uniform across languages; each binding realizes the same concepts idiomatically. Aim for cross-language consistency with reasonable compromises, not for maxing out each language. Bindings are tiered by delivery commitment:

- **Tier 1**: the Rust reference implementation, and the `shcl` CLI built from it. These define conformance.

- **Tier 2**: Go, C (+ C++ veneer), Python - independent parsers, shipped when corpus-green.

- **Tier 3**: everything else (C#, Java (+ Kotlin), JavaScript (+ TypeScript), ...) - after v1.0, corpus-gated, designed-for from the start.

- **CLI wrappers**: Bash and PowerShell are thin wrappers around the `shcl` CLI, not independent parsers - they inherit conformance from Tier 1 for free.

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
| Rust       | `let pop = doc.get_int(path).unwrap_or(0);`   | `let r = doc.get_int(path); // Result<i64, Status>`|
| C          | `int pop = shcl_get_int(doc, path, 0);`       | `shcl_get_int_ex(doc, path, &pop); // -> status`   |
| C++        | `int pop = doc.get_or<int>(path, 0);`         | `auto r = doc.get<int>(path); // .value / .status` |
| C#         | `int pop = doc.GetIntOr(path, 0);`            | `var r = doc.GetInt(path); // .Value / .Status`    |
| Java       | `int pop = doc.getIntOr(path, 0);`            | `var r = doc.getInt(path); // .value() .status()`  |
| Kotlin     | `val pop = doc.getIntOr(path, 0)`             | `val r = doc.getInt(path)`                         |
| Python     | `pop = doc.get_int(path, default=0)`          | `r = doc.read_int(path)  # r.value, r.status`      |
| JS / TS    | `const pop = doc.getIntOr(path, 0)`           | `const r = doc.getInt(path)  // {value, status}`   |
| PowerShell | `[int]$pop = $doc.GetIntOr($path, 0)`         | `$r = $doc.GetInt($path)  # .Value .Status`        |
| POSIX sh   | `pop=$(shcl get --int --default=0 f 'path')`  | `shcl get --int f 'path'; status=$?`               |

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
	- Every array read carries one status per slot alongside the values (a slot list on the result). Each slot reads like a scalar of the target type: `Good`, `Empty` (empty value), `NotFound` (missing sub-path), `BadType` (uncoercible, raw block, or array where one scalar is expected), or `Multiple` (sub-path ambiguous within that instance). The read's aggregate status is the worst slot, so a partial miss can never report `Good`.
	- `Instances` on a wildcard path keeps unresolved slots in the enumeration as empty strings, preserving index alignment with the read and with `Count` (which counts slots).
	- With `onBad=default`, the supplied default substitutes per bad slot; resolved slots keep their values.

- `Instances(field)` and `Count(field)` enumerate instances by value or index.

- An ambiguous single-value read (path resolves to many instances) reports `Multiple`; narrow it with a selector until exactly one remains.

### Diagnostics and writing

- Loading also yields a list of structured **diagnostics** (line number + reason + severity) for every skipped or repaired line, which the consumer may inspect or ignore. Severity is `error` (a line was skipped or repaired) or `hint` (legal input that looks like a common mistake, e.g. the repeated-leaf array hint). The split matters for Strict mode: only `error` diagnostics fail a strict load.

- The **Writer** handles the reverse of the Accessor: emit values, defaults, and comment sections, and canonicalize a file (see below). It mirrors the Accessor's typed-entry-point shape - a `Set<T>` per type (`SetInt`/`SetString`/.../`SetRaw`) and their array forms - so a programmatic value lands as canonical text with no consumer-side formatting. Each setter is the exact inverse of the matching read: `SetString` re-quotes and escapes so the value reads back verbatim; `SetFloat`/`SetInt` emit the same canonical number text the reader accepts; `SetDateTime` stores the canonical spelling; `SetRaw` picks a fence long enough that the content cannot close it early. A **set** creates the path (intermediate nodes as needed) and replaces the value at the leaf; a `[value]`/`[#index]` selector on the path targets a specific instance (a `[value]` selector creates the instance if absent). Companions round out the surface: `Set<T>Default` writes only when the path does not already resolve (the "emit defaults" half), `Exists` reports presence, `SetComment` attaches a leading comment line (creating an empty node so a section can be annotated), and `Remove` deletes the node(s) at a path. After any edits, the canonical formatter emits the result; a written document is a formatter fixpoint like any other canonical output.

## Canonical formatter

The formatter normalizes structure only - it cannot know value types, so it never rewrites value text (no `.5` -> `0.5`). It loads at the requested strictness like every other operation; a strict-failing document formats nothing (the load failure is the result).

- Block (indented) form, tabs for indentation.

- Preserve file/insertion order of instances and fields (so index-based access stays stable).

- Collapse and merge redundant sections and paths.

- Preserve comments as attached trivia. A whole-line comment attaches to the node bound by the next non-comment line and re-emits just above that node's line, at its indent; a trailing comment stays on its line, two spaces before the `#`. Comment text is never rewritten.

- When instances merge, their comments concatenate in encounter order; a second trailing comment moves to the lines above (a canonical line has room for one). Comments among stacked-list elements ride the list's field line. Comments after the last binding line re-emit at the end of the output, unindented.

- Quote a value only when a reserved character requires it (minimal quoting).

- Leave scalar text exactly as authored; raw blocks are re-emitted verbatim. A block value canonicalizes to the child-indent spelling - bare `name:`, fence (with its info-string) on the next line at child indent - one field line per block instance.

- Two narrow exceptions keep round-trips exact. If an *earlier* instance of the same field under the same parent is empty, the child-indent header line would merge into it on re-read and the fence would fill that instance - so the formatter emits that block in the same-line spelling instead. And an info-string that *starts with* the fence character gets one space after the fence, so it cannot lengthen the fence run on re-read. A block emitted in the same-line spelling also moves any trailing comment to the lines above - after the fence it could read as part of the info-string on re-read.

## Error handling philosophy

SHCL never bails on a whole file for one bad line (at Loose and Standard strictness; Strict turns any `error` diagnostic into a load failure by request - see Strictness levels). The parser skips or best-effort-repairs the offending line, emits a diagnostic, and continues. The Accessor never errors when it can unambiguously reach a value; malformed content before or after a clean section does not poison that section. Errors are reserved for genuine ambiguity (or surfaced on request via `onBad: Error`).

One repair is defined concretely, because it is the common "figure it out" case: a line that is a **well-formed field path with no colon and nothing after it** (`base[Boston].metrics.population`, no value) is repaired to that path carrying an **empty value** - the obvious intent - with a diagnostic emitted. This is deliberately narrow. A line whose colon is missing but which is *not* a clean path - a bareword then whitespace then another token (`square-miles 300`) - is genuinely ambiguous (is `300` a value, or part of a name that cannot legally contain a space?), so it is skipped with a diagnostic rather than guessed.

## Strictness levels

One knob, set once per document at load time, governs how forgiving the whole surface is: **`Loose` / `Standard` / `Strict`** (CLI shorthand `--strictness=loose|standard|strict` or `1|2|3`). The default is `Standard` - everything specified elsewhere in this document describes `Standard` behavior. The level is per-document, never per-call: it is a property of how an application treats its config, not of one read. It composes with, and is orthogonal to, the per-call `onBad` policy: strictness decides *whether* a value coerces; `onBad` decides what happens when it does not.

The bundles are normative - a binding implements exactly this table:

| Behavior | Loose (1) | Standard (2, default) | Strict (3)
| :-- | :-- | :-- | :--
| Malformed line at load | skip + `error` diagnostic | skip + `error` diagnostic | **load fails**
| Colon-less-path repair | applied + `error` diagnostic | applied + `error` diagnostic | **load fails**
| `hint` diagnostics (e.g. repeated-leaf) | emitted | emitted | emitted (never fail a load)
| float -> int | rounds (`3.5` -> 4) | `BadType` | `BadType`
| Leading currency symbol -> number | stripped | `BadType` | `BadType`
| `50%` -> float | 0.5 | `BadType` | `BadType`
| Boolean token set | Standard set plus `t`/`f`, `y`/`n`, `enable(d)`/`disable(d)` | `true`/`false`, `yes`/`no`, `on`/`off`, `1`/`0` | `true`/`false` only

Notes:

- **Loose** re-admits the forgiving conversions cut from Standard, as a closed list - nothing joins it without a spec change. The currency rule is: a single leading symbol from exactly these codepoints is stripped (`$ ¢ £ ¤ ¥ ₩ ₪ ₫ € ₭ ₮ ₱ ₲ ₴ ₹ ₺ ₼ ₽ ₾ ₿`); multi-letter codes (`USD`, `kr`) are not, and there is no trailing form. A `%` float is the fraction, so a Loose `GetInt` on `50%` rounds 0.5 -> 1 - never special-cased to the pre-`%` number, so `GetInt` and `GetFloat` cannot disagree.

- **Strict** is the "fail loudly" mode: any `error` diagnostic aborts the load (the never-bail philosophy above describes Loose and Standard). Reads are unchanged except the boolean set. `hint` diagnostics never fail a load at any level - repeated leaves are legal instances, and failing legal input would break the data model.

- Every level is corpus-pinned: conformance reads carry an optional level column, so a binding cannot drift on any bundle row.

## Cross-language parity and conformance

The guarantee is the corpus, not the binding count: **every shipped binding is corpus-green**. A binding that has not passed the full conformance corpus is not shipped, full stop. A companion surface (C++/Kotlin/TypeScript) inherits its core's conformance for free, and the CLI-wrapper bindings (Bash, PowerShell) inherit the Tier 1 CLI's. The safeguards:

- This spec plus `grammar.abnf` are the single source of truth; behavior is specified, not left to each implementation.

- A **conformance corpus** of golden cases (`conformance/`) pins every implementation to identical results: each case is an input `.shcl`, its expected canonical formatting, and a set of expected typed reads with their status sentinels. The date/coercion/quoting edge cases live here so no parser can silently drift.

- Every parser runs the corpus in CI before it is considered conformant.

## Resolved minor items

These were previously deferred and are now settled inline (above):

- **Currency set**: cut from Standard; Loose-only, a fixed 20-codepoint list, single leading symbol only (see Strictness levels).

- **`field[*]` with a missing sub-path**: keep the positional slot, per-element `NotFound`, no diagnostic (see Paths, selectors, and enumeration).

- **`onBad` surface**: canonical `Error`/`Default`/`Flag`, idiomatic enum per language, default `Flag` (see The core call).

- **`GetInt` on a `%` value**: cut from Standard (`BadType`); at Loose, rounds the fraction, not special-cased (see Strictness levels).

- **Fence info-string**: free-form advisory label, exposed but never interpreted (see Raw blocks).

- **Fehu anti-escape**: removed entirely - raw blocks are the verbatim escape hatch (see Escapes).

- **Field-name case folding**: ASCII `A-Z` only, never Unicode (see Merging and instances).
