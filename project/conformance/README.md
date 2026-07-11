<!-- markdownlint-disable MD010 MD033 MD041 -->
# Conformance corpus

Golden cases that pin every SHCL parser (Go, Rust, C/C++, Python, POSIX sh) to identical behavior. Each parser runs this corpus in CI; drift on any case means the parser is non-conformant.

## Case layout

Each case is a directory `NNN-short-name/` containing:

- `input.shcl` - the source, usually deliberately messy.
- `expected.shcl` - the canonical formatter output for that input (block form, tabs, insertion order, minimal quoting, redundancy collapsed).
- `reads.tsv` - expected typed reads. Columns, tab-separated: `query` `type` `expected` `status`. `type` uses `int|float|bool|datetime|string|raw` and `[]` for array forms, or the pseudo-calls `count`/`instances`. `expected` is the value (`-` when not applicable); `status` is one of `Good|Empty|NotFound|BadType|Multiple`.

## Notes

Case `001` corrects the autoformat shown in `../../../notes.txt`: instances are kept in **insertion order** (Chicago, Cleveland, Boston, Philly), not sorted alphabetically, and values are **minimally quoted** (city names stay bare unless a reserved char forces quotes). Those two points are the intentional divergence from the original by-example draft.
