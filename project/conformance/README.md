<!-- markdownlint-disable MD010 MD033 MD041 -->
# Conformance corpus

Golden cases that pin every shipped SHCL binding to identical behavior. Each independent parser runs this corpus in CI; drift on any case means the binding is non-conformant and does not ship. (CLI-wrapper bindings and companion typed surfaces inherit conformance from their core.)

## Case layout

Each case is a directory `NNN-short-name/` containing:

- `input.shcl` - the source, usually deliberately messy.
- `expected.shcl` - the canonical formatter output for that input (block form, tabs, insertion order, minimal quoting, redundancy collapsed), at Standard strictness.
- `reads.tsv` - expected typed reads. Columns, tab-separated: `query` `type` `expected` `status` `[level]`. `type` uses `int|float|bool|datetime|string|raw` and `[]` for array forms, or the pseudo-calls `count`/`instances`/`load`. `expected` is the value (`-` when not applicable); `status` is one of `Good|Empty|NotFound|BadType|Multiple`. The optional fifth column is the strictness level (`loose|standard|strict`), default `standard`. The `load` pseudo-call asserts whether the document loads at that level: query `-`, expected `ok` or `fail`, status `-`.

## Notes

Case `001` corrects the autoformat shown in `../../../notes.txt`: instances are kept in **insertion order** (Chicago, Cleveland, Boston, Philly), not sorted alphabetically, and values are **minimally quoted** (city names stay bare unless a reserved char forces quotes). Those two points are the intentional divergence from the original by-example draft.

Case `002` pins the stacked (`*`) array form: an inline comma array and the `*`-per-line form read byte-identical and both canonicalize to the inline form, while repeated leaf lines stay separate **instances** (not an array).

Case `003` pins the strictness bundles on coercion: currency, `%`, float->int rounding, and the widened boolean set exist only at `loose`; `strict` narrows booleans to `true`/`false`.

Case `004` pins load behavior per level: a malformed line is skipped with a diagnostic at `loose`/`standard` (the rest of the file still reads), and fails the whole load at `strict`.

Not yet modeled: diagnostic expectations (count, severity, the mandatory repeated-leaf hint). Needs a `diags.tsv` or similar once the reference parser defines the diagnostic shape.
