<!-- markdownlint-disable MD010 MD033 MD041 -->
# Conformance corpus

Golden cases that pin every shipped SHCL binding to identical behavior. Each independent parser runs this corpus in CI; drift on any case means the binding is non-conformant and does not ship. (CLI-wrapper bindings and companion typed surfaces inherit conformance from their core.)

## Case layout

Each case is a directory `NNN-short-name/` containing:

- `input.shcl` - the source, usually deliberately messy.

- `expected.shcl` - the canonical formatter output for that input (block form, tabs, insertion order, minimal quoting, redundancy collapsed), at Standard strictness.

- `reads.tsv` - expected typed reads. Columns, tab-separated: `query` `type` `expected` `status` `[level]` `[slots]`. `type` uses `int|float|bool|datetime|string|raw|rawinfo` and `[]` for array forms (except `raw`/`rawinfo`, which have no array form), or the pseudo-calls `count`/`instances`/`load`. `rawinfo` reads a raw block's info-string (the fence tag) rather than its content. `expected` is the value (`-` when not applicable); `status` is one of `Good|Empty|NotFound|BadType|Multiple`. The optional fifth column is the strictness level (`loose|standard|strict`), default `standard`. The optional sixth column (requires the fifth) pins the per-slot statuses of an array read, `|`-joined in slot order; the row's `status` is then the worst slot. The `load` pseudo-call asserts whether the document loads at that level: query `-`, expected `ok` or `fail`, status `-`. In `expected`, a newline inside a raw-block value is written `\n` (a literal newline or tab would break the TSV).

- `expected-diags.txt` - the diagnostic golden (required): the exact `check` stdout at Standard strictness - one `line N: Severity: CODE` line per diagnostic in emission order, then the summary line (`ok (N diagnostic(s))`, or `failed: N diagnostic(s), M error(s)` when errors are present). Pins count, line, severity, and stable code per case, including the mandatory repeated-leaf hint (`H001`) and the zero-diagnostic cases.

- `write.ops` + `expected-write.shcl` (optional, as a pair) - the **Writer** dimension. `write.ops` is a script of write operations (one per line, tab-separated); each binding applies it to `input.shcl` via its library Writer and must produce `expected-write.shcl` byte-for-byte, and that output must be a formatter fixpoint. A blank line or one starting with `#` is skipped. Op grammar (`<T>` = `int|float|bool|string|datetime`):
	- `<T><TAB>PATH<TAB>VALUE` - set a scalar. `datetime` VALUE is any accepted spelling and is stored canonically.
	- `<T>-array<TAB>PATH<TAB>V1<TAB>V2...` - set an inline array (no elements = an empty value).
	- `<T>[-array]-default<TAB>...` - set only if the path does not already resolve.
	- `raw<TAB>PATH<TAB>INFO<TAB>CONTENT` (and `raw-default`) - set a raw block; `INFO` may be empty.
	- `empty<TAB>PATH`, `comment<TAB>PATH<TAB>TEXT`, `remove<TAB>PATH`.
	- `string` and `raw` `CONTENT` values decode `\n` `\t` `\\` (so a multi-line value fits on one op line); no other escapes are interpreted. The setters re-encode for storage, so a value read back equals the logical value it was set from.

- `schema.shcl` + `expected-validate.txt` (optional, as a pair) - the **schema validation** dimension (spec.md "Schema validation"). `schema.shcl` is a schema (itself plain SHCL); `expected-validate.txt` is the exact `check --schema schema.shcl input.shcl` stdout at Standard strictness - the document's parse diagnostics, then the validation diagnostics (`V###` codes), then the summary line, under the same format as `expected-diags.txt`. Each binding replays it via its library `Validate`; a schema that does not itself load cleanly must yield the single `line 0: Error: V099` diagnostic, mirroring the CLI.

## Notes

Case `001` corrects the autoformat shown in `../../../notes.txt`: instances are kept in **insertion order** (Chicago, Cleveland, Boston, Philly), not sorted alphabetically, and values are **minimally quoted** (city names stay bare unless a reserved char forces quotes). Those two points are the intentional divergence from the original by-example draft.

Case `002` pins the stacked (`*`) array form: an inline comma array and the `*`-per-line form read byte-identical and both canonicalize to the inline form, while repeated leaf lines stay separate **instances** (not an array).

Case `003` pins the strictness bundles on coercion: currency, `%`, float->int rounding, and the widened boolean set exist only at `loose`; `strict` narrows booleans to `true`/`false`.

Case `004` pins load behavior per level: a malformed line is skipped with a diagnostic at `loose`/`standard` (the rest of the file still reads), and fails the whole load at `strict`.

Case `005` pins raw-block binding: a fence is a value line for its parent field - both spellings (same-line and the canonical child-indent) bind as the field's value; a fence under an already-valued field creates a new instance, addressed with the normal `[0]`/`[#N]` selectors.

Case `006` pins forgiving inline arrays: stray commas never error. Leading, doubled, and trailing commas drop their empty slots (`red,,blue` -> `red, blue`), an all-comma value (`,,,`) is the empty array, and a `""`-quoted element is the one way to keep a deliberately empty slot.

Case `007` also pins that a multibyte char inside a time-shaped value's zone tail is a plain `BadType`, never a crash.

Case `008` pins 10-element typed arrays of every kind (int, float, bool, datetime) - large enough to force output-buffer growth in the CLIs, which is where per-element formatted output can go stale.

Case `009` pins wildcard slot alignment: a missing sub-path keeps its slot (per-slot `NotFound`, value zero/default), an uncoercible one reads `BadType`, the aggregate status is the worst slot, and `count`/`instances` stay index-aligned with the read (unresolved slots enumerate as "").

Case `010` pins uniform-or-nothing: mixing `*` elements and field children under one parent is not a block array - the first mixed field diagnoses an Error (and keeps the field), every `*` line after a field child is an Error and is dropped, and the document loads at `standard` but fails at `strict`.

Case `011` pins selector-vs-instance matching on the display form: `base[Boston, MA]` selects the existing array-valued instance `base: Boston, MA` instead of creating a second one. It also pins array-as-string: a multi-element value read as one string is the canonical inline form (minimal quoting, escapes intact), while the array-of-strings read unquotes and applies escapes per element.

Case `012` pins raw-block identity: the info-string is part of a block's value, so equal bodies with `sql` and `python` infos are two instances (never a silent merge that drops an info).

Cases `014`-`016` pin the **Writer**. `014` builds a document from an empty base (scalars, arrays, a comment above a later-set field, an empty section, and a `-default` that no-ops when the field already exists). `015` edits an existing document (overwrite the first instance of a leaf, `-default` that keeps the present value, `remove`, and a `[value]` selector that adds children under the matching instance). `016` pins the emit hazards: raw blocks (fence chosen so the content cannot close it early, info-string as identity), tricky strings (tab/quote/backslash and a fence-lookalike, minimally quoted so they read back verbatim), an explicit empty string (`""`, distinct from an empty value), and a bare 8-digit date stored canonically.

Case `013` pins comment preservation through `fmt`: a whole-line comment re-emits above the node bound by the next line (merged instances concatenate theirs), a trailing comment stays on its line (a second one from a merged instance moves above), comments among `*` elements ride the field line, a comment between a bare header and its fence attaches to that field, `#` inside a raw block stays content, and comments after the last binding line re-emit at the end. The older cases' expected files carry their inputs' comments too.

Case `017` pins merge-key injectivity: a single element holding a literal NUL (`x: "a<NUL>b"`) stays distinct from the two-element array `x: a, b` (`count = 2`), where a bare-NUL-joined key would merge them and drop the second. The input carries an actual NUL byte, so the cross-binding differential skips it (bash cannot hold a NUL) and the four native runners do the pinning.

Case `018` pins `field[disc]: value`: a value after a last-segment selector is an `error` (the instance is created from the discriminator, the value dropped), so the document loads at `standard` but fails at `strict`, and `city` ends up with the two discriminator instances.

Case `019` pins i64 bounds across hex and decimal spellings: the int read parses the magnitude as u64 and range-checks it against the sign, so `-0x8000000000000000` reads i64-min like its decimal spelling, `0x7fffffffffffffff` reads i64-max, and the positive `0x8000000000000000` overflows to `BadType`.

Case `020` pins the accessor surface that ports diverge on: wildcard reads across instances (`server[*].port`, and `server[*].region` where one instance lacks the sub-path, so a slot is `NotFound` and the aggregate is the worst slot), a `[value]` selector read, and a raw block read both ways (`raw` for content, `rawinfo` for the `sql` info-string).

Cases `021`-`024` pin the schema validation dimension. `021` is the all-pass sweep (every constraint kind satisfied, including a quoted wildcard path, constraints for one path split across two merged `field` instances, and an empty value passing `type: bool`). `022` produces every data-validation code `V001`-`V007` at least once - unknown fields with and without a "did you mean" suggestion, `required` missing at document scope (line 0) and per wildcard instance (that instance's line), `repeat` violated at both scopes, plus the `H001` hint riding along in the combined output. `023` produces the schema-fault codes (`V090`-`V093`) and pins that a broken schema suppresses data validation (the document's own violation must NOT be reported). `024` pins `V099`: a schema that does not parse cleanly yields exactly one line-0 diagnostic.

Beyond the fixed corpus, the differential harness (`cicd/utility/crosscheck.bash`) also derives accessor coverage over the fuzz set: the reference's fuzz dump writes a `<name>.reads.tsv` beside each dumped input (paths it knows exist, cycling type and strictness), which the `--extra` replay runs through the same row machinery. Every scalar read row - corpus and fuzz-derived - is additionally replayed under `--on-bad=error` (an exit-code differential) and `--default=<x>` (a stdout differential), so the on-bad/default policy surface is pinned cross-binding too.

Not yet modeled natively (as golden files): the on-bad/default outputs (covered cross-binding via the harness above, not by per-row `expected`). Diagnostic expectations are modeled natively via `expected-diags.txt` (above) and cross-binding via the `load` rows.
