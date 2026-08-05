# shcl

**S**imple **H**ierarchical **C**onfig **L**anguage. Forgiving to write, predictable to read.

Values are stored as plain text and coerced only when *your* code asks for a type, so a config file can never silently decide that `NO` means `false`. A bad line is skipped or repaired with a diagnostic instead of taking down the whole file.

Pure standard library, no dependencies, one module. Held byte-for-byte against the Rust reference by a shared conformance corpus.

## Install

```sh
pip install shcl
```

## Use

```python
from shcl import Document, Status

with open("server.shcl", encoding="utf-8") as f:
	doc = Document.parse(f.read())

# One call, a typed value, a visible fallback at the call site.
limit = doc.get_int("site[example.com].max-upload-mb", default=10)

# Or ask why a read failed: Good, Empty, NotFound, BadType, Multiple.
r = doc.read_int("site[example.com].max-upload-mb")
if r.status is not Status.Good:
	print(r.status, r.raw)

# Wildcards read across instances, with a status per slot.
roots = doc.read_string_array("site[*].root")
```

`Document.parse` never raises. When you want a hard error instead, use `Document.parse_with(text, Strictness.Strict)`, which raises `LoadError`. A `get_*` call with no `default=` raises `StatusError` rather than inventing a value.

Also here: `merge` for layered config (defaults, site, user), `validate` against a schema that is itself a SHCL file, a full writer, and a canonical formatter that preserves comments.

Python 3.9 or newer.

## Compatibility

Bindings are versioned in lockstep, so `1.x` is the same behavior in every language. `shcl~=1.0` picks up minor and patch releases on its own and never crosses a major version.

## The CLI

This package is the library only. The `shcl` command comes as a prebuilt binary and as `.deb`/`.rpm`/Windows packages - see <https://github.com/jim-collier/shcl>.

## Docs

Language spec, formal grammar, and the other bindings: <https://github.com/jim-collier/shcl>

## License

MIT. SHCL™ is a trademark of Jim Collier - see the [trademark policy](https://github.com/jim-collier/shcl/blob/main/trademark.md).
