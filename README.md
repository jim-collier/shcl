<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- hard tabs -->
<!-- markdownlint-disable MD033 -- inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
<!-- markdownlint-disable MD026 -- Trailing punctuation in heading; the "compares to..." ellipsis is deliberate -->
<div align="center">

[![made-with-rust](https://img.shields.io/badge/Made%20with-Rust-1f425f.svg)](https://www.rust-lang.org/)
![Go](https://img.shields.io/badge/Go-00ADD8?logo=go&logoColor=white)
[![made-with-python](https://img.shields.io/badge/Made%20with-Python-1f425f.svg)](https://www.python.org/)
![Made with](https://img.shields.io/badge/Made%20with-C%2B%2B-brightgreen?style=plastic)
[![!#/bin/bash](https://img.shields.io/badge/-%23!%2Fbin%2Fbash-1f425f.svg?logo=gnu-bash)](https://www.gnu.org/software/bash/)
![Lifecycle: Stable](https://img.shields.io/badge/Lifecycle-Stable-brightgreen)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
![Support](https://img.shields.io/badge/Support-Maintained-brightgreen)
[![CI](https://github.com/jim-collier/shcl/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/jim-collier/shcl/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/jim-collier/shcl?sort=semver)](https://github.com/jim-collier/shcl/releases)
[![crates.io](https://img.shields.io/crates/v/shcl?logo=rust&label=crates.io)](https://crates.io/crates/shcl)
[![PyPI](https://img.shields.io/pypi/v/shcl?logo=python&logoColor=white&label=PyPI)](https://pypi.org/project/shcl/)
[![Go module](https://pkg.go.dev/badge/github.com/jim-collier/shcl/source/go.svg)](https://pkg.go.dev/github.com/jim-collier/shcl/source/go)

<!-- TOC ignore:true -->
# SHCL

**S**imple **H**ierarchical **C**onfig **L**anguage

*Forgiving to write. Predictable to read. The friendliest config API around.*

<!-- width = the gif's own pixel width, so it never renders larger than 1:1; no height attr, so a narrow column can still shrink it without squashing -->
<img src="assets/demo.gif" alt="SHCL demo" width="960"/>

<!-- [Watch the walkthrough](https://www.youtube.com/watch?v=VIDEO_ID) -->

<br />

</div>

> *You get: a complete specification and grammar; one drop-in source file per language; Rust (reference), Go, Python, and C/C++ bindings plus a CLI. Bindings are byte-for-byte identical. Thin Bash and PowerShell wrappers sit over the CLI. MIT License.*

<!-- TOC ignore:true -->
## Table of contents

<!-- TOC -->

- [The problem](#the-problem)
- [What SHCL does about it](#what-shcl-does-about-it)
- [How it compares to...](#how-it-compares-to)
	- [Config languages - JSON, YAML, TOML, XML](#config-languages---json-yaml-toml-xml)
	- [DDL languages - Pkl, CUE, Dhall](#ddl-languages---pkl-cue-dhall)
	- [When SHCL is the wrong choice](#when-shcl-is-the-wrong-choice)
- [Features](#features)
- [What a .shcl file looks like](#what-a-shcl-file-looks-like)
- [Example use-cases in your code](#example-use-cases-in-your-code)
	- [Rust](#rust)
	- [Go](#go)
	- [Python](#python)
	- [Zig](#zig)
	- [C, C++](#c-c)
	- [Bash](#bash)
	- [PowerShell](#powershell)
- [Installation](#installation)
	- [Language packages](#language-packages)
		- [Cargo](#cargo)
		- [Go module](#go-module)
		- [PyPi](#pypi)
		- [C and C++](#c-and-c)
	- [Other installation options](#other-installation-options)
		- [OS-level packages and installers](#os-level-packages-and-installers)
			- [Debian](#debian)
			- [Fedora, RHEL, openSUSE](#fedora-rhel-opensuse)
			- [Windows](#windows)
		- [Scripted installation direct from web - dev or stable](#scripted-installation-direct-from-web---dev-or-stable)
			- [Linux and WSL](#linux-and-wsl)
			- [Windows PowerShell](#windows-powershell)
		- [DIY install](#diy-install)
- [Set up a development environment](#set-up-a-development-environment)
- [Docs](#docs)
- [Contributing and support](#contributing-and-support)
- [Legal stuff](#legal-stuff)

<!-- /TOC -->

## The problem

You have probably lived some version of this:

- A whole service refused to start because one line of config had a typo.

- YAML turned `country: NO` into `false`. (Norway. It really does that.)

- JSON needed a comment, and JSON does not do comments.
	- Or a trailing comma killed the parse.

- TOML was pleasant right up until the data nested three levels deep.

- You wanted an integer. You got a string, or an exception, or a silent zero you did not notice until production.

- Remember that one project where a complex nested config was stored in XML - that you have PTSD over to this day?

Every mainstream format makes the human do the careful work, and punishes the whole file for one mistake.

## What SHCL does about it

SHCL (canonically pronounced "SHiCkLe") flips the burden around. Modern CPU cycles are cheap, even on embedded systems. Brainpower isn't.

So the parser does the hard work - not the person writing the file, and not the programmer consuming the configuration.

SHCL makes a contract:

- Types live in your code, not in the file. The file stores text. You ask for a type when you read a value. Nothing is guessed at parse time, so there is no "Norway problem".

- One broken line never takes down the file. It is skipped with a note. Everything else that can still be loaded safely, is.

- Every convenience read states a fallback at the call site. A missing value cannot sneak in as a silent zero.

- If a person can tell what a line means, the parser can too.

When you do want zero-tolerance rigor: schema validation, plus a strict mode that fails loudly.

## How it compares to...

### Config languages - JSON, YAML, TOML, XML

| | SHCL | JSON | YAML | TOML | XML
| :-- | :-- | :-- | :-- | :-- | :--
| Comments | ✅ | 🚫 | ✅ | ✅ | ✅
| Unquoted strings | ✅ | 🚫 | ✅ But the parser may silently change type | 🚫 | 🚫
| Bad lines don't break the whole thing | ✅ | 🚫 | 🚫 | 🚫 | 🚫
| Who decides a value's type | Your code, at read time | The file | The parser guesses | The file | Your code
| Deep nesting | Indent or dot paths, mixed | Brace pyramids | Indent, whitespace-fragile | `[a.b.c]` headers get old fast | Tag soup
| Multi-line verbatim blocks | Fenced, like Markdown | Escaped strings | Block scalars, with rules to memorize | Multi-line strings | CDATA
| Hand-editable by a non-programmer | ✅ | Risky | Risky | ✅ Mostly | 🚫
| Tells you what it fixed | ✅ Structured diagnostics | 🚫 | 🚫 | 🚫 | 🚫

> *A note on types, because it is the big design difference: JSON and TOML store types in the file, so the author has to get them right. YAML infers types from the text, which is where `NO` becomes `false`. SHCL stores plain text and coerces when **you** ask for a type; the only code that decides a value is an int (for example), is the code that needed an int.*

### DDL languages - Pkl, CUE, Dhall

These are a different species. They overlap a little with SHCL's power layer, but from the opposite direction: they make the *config file itself* powerful, which is exactly what SHCL avoids.

- **Pkl** (from Apple) is a real language: classes, inheritance, built-in validation. Great when your config genuinely is a program. (And very arguably the winner among these three, depending on your use-case.)

- **CUE** unifies types and values into one thing. Extremely strong validation, and a mental model that takes real time to absorb.

- **Dhall** is functional programming for config: imports, functions, guaranteed termination. Closer to writing Haskell than editing a file.

They are all good at what they do. The shared cost is that once a config file can compute, it can be wrong in ways you have to debug.

SHCL deliberately stays off that cliff. The file stays dumb, and the power moves into the library instead:

- **Schema validation.** A schema is just another SHCL file. `doc.validate(schema)` catches unknown fields, wrong types, and out-of-range values, including the "did you mean `enabled`?" typo case.

- **Layered loading.** `merge(base, over)` folds files in order - defaults, then site, then user, last wins - and the CLI stacks the same way with repeatable `--layer=FILE` plus `--set=PATH=VALUE` overrides on top. That covers most of what people actually use imports for. (Environment-variable mapping is deliberately your program's job: the env namespace and its naming convention belong to the app, which can map env vars onto `--set` itself.)

- **Generated starter configs.** The schema plus the writer can emit a fully commented, correctly typed starting file.

Your config never needs a debugger, and a non-programmer can still edit it.

### When SHCL is the wrong choice

- You need expressions, functions, or imports inside the file itself. Use Pkl (arguably best), CUE, or Dhall.

- You are serializing machine-to-machine data at high volume. Use JSON or something binary. SHCL is for files that people edit.

## Features

- Hierarchy by indentation or dot-notation (`site[blog.example.com].tls.hsts: off`), freely mixed. Both spell the same tree.

- Values are typed on *read*, not on parse. The file stores text; your code asks for an int.

- Never bails on a whole file over one bad line. Bad lines are skipped or repaired with diagnostics, and the rest still loads.

- Every convenience read takes a call-site fallback (`GetIntOr(path, 0)`), so a missing value can't masquerade as a real zero.

- Three strictness levels. Loose, standard, strict: one knob from maximum-forgiving to fail-on-anything.

- Schema validation, layered loading (defaults, site, user), and commented starter-config generation, all as library features.

- Raw fenced blocks embed anything verbatim: SQL, code, templates, Markdown-style.

- One conformance corpus pins every binding to identical behavior. The Rust reference plus independent Go, C, and Python parsers agree byte-for-byte. A binding is not released until it does.

## What a .shcl file looks like

A small web server - the kind of thing nginx makes you learn a bespoke brace language for. All of this is one valid file: indentation and dotted paths are interchangeable, quoting is only needed when a value contains a reserved character, and messy spacing is fine.

```text
# Flat, TOML-style settings
listen: "0.0.0.0:443"          # a colon in a value just needs quotes
workers: 4
log-level: warn

# Hierarchy when you need it: one instance per site
site: example.com
	root: /srv/www/example
	Max-Upload-MB : 50         # names are case-insensitive, spacing is loose
	methods: GET, POST, HEAD   # an array is just commas
	tls:
		cert: /etc/ssl/example.pem
		hsts: on

# Repeating the field adds another site - arrays of objects, no syntax to invent
site: blog.example.com
	root: /srv/www/blog

# Dotted paths spell the same tree; add to any instance from anywhere
site[blog.example.com].tls.hsts: off

# Multi-line content goes in a fenced block, kept verbatim
maintenance-page:
	~~~html
	<h1>Down for maintenance - back in five.</h1>
	~~~
```

Field names are case-insensitive. Repeated paths merge. `site` here is not one key but a set of instances (example.com, blog.example.com), each with its own children - arrays of objects without inventing syntax for them.

<!--
## Reading it from code

One call. A typed value. A visible fallback. This is the call you write 90% of the time:

```go
// Go
limit := doc.GetIntOr("site[example.com].max-upload-mb", 10)
```

```python
# Python
limit = doc.get_int("site[example.com].max-upload-mb", default=10)
```

```sh
# Bash (sh/ash/zsh/etc.)
limit=$(shcl get --int --default=10 server.shcl 'site[example.com].max-upload-mb')
```

When you need to know *why* a read failed, the full form returns a status instead: `Good`, `Empty`, `NotFound`, `BadType`, or `Multiple`.

Wildcards read across instances: an array read of `site[*].root` gives you every site's document root, in file order, with a status per slot.
-->

## Example use-cases in your code

Bindings are versioned in lockstep, so `1.x` means the same behavior and the same conformance corpus in every language. Each ecosystem's usual compatible-version operator is all you need: it picks up minor and patch releases on its own, and never crosses a major version without you editing the line yourself.

Every binding is one file with no dependencies. You can optionally just copy it out of `source/` and commit it - see [DIY install](#diy-install) above.

### Rust

- Install: `cargo add shcl`
- Dependency line: `shcl = "1"`
- Note: The Rust crate ships the library and the CLI together - see [Language packages](#language-packages) if the binary is what you are after.

```rust
use shcl::{Document, Status};

let text = std::fs::read_to_string("server.shcl")?;
let mut doc = Document::parse(&text);

// Typed read, with a fallback if the path is missing
let workers = doc.get_int("workers").unwrap_or(4);
let root = doc.get_string("site[example.com].root").unwrap_or_default();

// Or ask why a read failed, when the difference matters
match doc.get_int("site[example.com].max-upload-mb") {
	Ok(mb) => println!("{mb} MB"),
	Err(Status::NotFound) => println!("not configured"),
	Err(other) => println!("unusable: {other:?}"),
}

// Writes create what they need to: this adds a site and a nested block
doc.set_int("workers", workers * 2);
doc.set_bool("site[example.com].tls.hsts", true);
doc.set_string("site[blog.example.com].root", "/srv/www/blog");

std::fs::write("server.shcl", doc.to_canonical())?;
```

### Go

- Install the module: `go get github.com/jim-collier/shcl/source/go`
- Dependency line: `require github.com/jim-collier/shcl/source/go v1.0.0`
- Notes: Go keeps its module in a subdirectory, so the import path ends in `/source/go` and the module's own tags carry a matching `source/go/` prefix. `go get -u` tracks `1.x`. A future 2.0 would import as `.../source/go/v2`, so a major version cannot arrive by surprise.

```go
import shcl "github.com/jim-collier/shcl/source/go"

text, err := os.ReadFile("server.shcl")
doc := shcl.Parse(string(text))

workers := doc.GetIntOr("workers", 4)
root := doc.GetStringOr("site[example.com].root", "")

if mb, st := doc.GetInt("site[example.com].max-upload-mb"); st == shcl.Good {
	fmt.Println(mb, "MB")
} else {
	fmt.Println("unusable:", st)
}

doc.SetInt("workers", workers*2)
doc.SetBool("site[example.com].tls.hsts", true)
doc.SetString("site[blog.example.com].root", "/srv/www/blog")

os.WriteFile("server.shcl", []byte(doc.ToCanonical()), 0o644)
```

### Python

- Install: `pip install shcl`
- Dependency line: `shcl~=1.0`
- Note: The PyPI distribution is the library module by itself. Installing it does not put a `shcl` command on your `PATH`.
	- If you want the CLI too, get if from a package, or an installer.

```python
import shcl

with open("server.shcl") as f:
	doc = shcl.Document.parse(f.read())

workers = doc.get_int("workers", default=4)
root = doc.get_string("site[example.com].root", default="")

read = doc.read_int("site[example.com].max-upload-mb")
if read.status is not shcl.Status.Good:
	print("unusable:", read.status)

doc.set_int("workers", workers * 2)
doc.set_bool("site[example.com].tls.hsts", True)
doc.set_string("site[blog.example.com].root", "/srv/www/blog")

with open("server.shcl", "w") as f:
	f.write(doc.to_canonical())
```

### Zig

Zig needs no binding of its own - it consumes the C header directly. `@cImport` takes the declarations, one C file carries the implementation, and thin wrappers let Zig slices supply the pointer-and-length pairs the C API wants:

```zig
const std = @import("std");
const c = @cImport(@cInclude("shcl.h"));

// The C API takes (pointer, length) paths; a Zig slice already carries both.
fn getInt(doc: ?*c.shcl_doc, path: []const u8, def: i64) i64 {
	return c.shcl_get_int(doc, path.ptr, path.len, def);
}
fn setInt(doc: ?*c.shcl_doc, path: []const u8, v: i64) bool {
	return c.shcl_set_int(doc, path.ptr, path.len, v) != 0;
}
fn setBool(doc: ?*c.shcl_doc, path: []const u8, v: bool) bool {
	return c.shcl_set_bool(doc, path.ptr, path.len, @intFromBool(v)) != 0;
}
fn setString(doc: ?*c.shcl_doc, path: []const u8, v: []const u8) bool {
	return c.shcl_set_string(doc, path.ptr, path.len, v.ptr, v.len) != 0;
}
fn readString(doc: ?*c.shcl_doc, path: []const u8) c.shcl_read_str {
	return c.shcl_read_string(doc, path.ptr, path.len);
}

// text is the config file, already read into memory
const doc = c.shcl_parse(text.ptr, text.len);
defer c.shcl_free(doc);

const workers = getInt(doc, "workers", 4);

const root = readString(doc, "site[example.com].root");
if (root.status == c.SHCL_GOOD)
	std.debug.print("{s}\n", .{root.value.p[0..root.value.n]});

_ = setInt(doc, "workers", workers * 2);
_ = setBool(doc, "site[example.com].tls.hsts", true);
_ = setString(doc, "site[blog.example.com].root", "/srv/www/blog");

const out = c.shcl_to_canonical(doc);
```

Alongside it, an `impl.c` of two lines - `#define SHCL_IMPLEMENTATION`, then `#include "shcl.h"` - and build with `zig build-exe main.zig impl.c -lc -lm -I.`. The interop above is stable, but Zig's own standard library is not, so the surrounding file IO moves between Zig releases; this was checked on 0.16.

### C, C++

C and C++ have no registry worth targeting. `shcl.h` is a single dependency-free header: copy it into your tree from a release tag and pin that tag, or take it from an installed package under `/usr/share/shcl/code/`. Define `SHCL_IMPLEMENTATION` in exactly one translation unit, and link `-lm`. C++ callers can add `shcl.hpp` alongside it for the typed veneer.

- Install: vendor `shcl.h`
- Dependency line: pin the release tag

```c
#define SHCL_IMPLEMENTATION   // in exactly one translation unit
#include "shcl.h"

#define P(s) s, strlen(s)     // paths take a pointer and a length, not a NUL terminator

// text/len is the config file, already read into memory
shcl_doc *doc = shcl_parse(text, len);

int64_t workers = shcl_get_int(doc, P("workers"), 4);

// Strings keep the status tier, so missing and empty stay distinguishable
shcl_read_str root = shcl_read_string(doc, P("site[example.com].root"));
if (root.status == SHCL_GOOD)
	printf("%.*s\n", (int)root.value.n, root.value.p);

shcl_set_int(doc, P("workers"), workers * 2);
shcl_set_bool(doc, P("site[example.com].tls.hsts"), 1);
shcl_set_string(doc, P("site[blog.example.com].root"), P("/srv/www/blog"));

shcl_str out = shcl_to_canonical(doc);   // lives in the document's arena
fwrite(out.p, 1, out.n, f);

shcl_free(doc);   // frees the document and everything handed out from it
```

The C binding uses `round()`, so link the math library - `cc -std=c11 -O2 ex.c -o ex -lm`. There are no per-object frees: reads hand back pointers into the document's arena, and the single `shcl_free` releases all of it, so anything you need afterwards must be copied out first.

Two things worth knowing about the write half. Setters build any missing structure along the path, so the `tls.hsts` and `blog.example.com` lines above appear as a nested block and a new site instance without you assembling either. And saving rewrites the file in canonical form, which normalizes spacing and lowercases field names, but **keeps your comments** attached to what they documented:

```text
# Flat, TOML-style settings
listen: "0.0.0.0:443"  # a colon in a value just needs quotes
workers: 8
log-level: warn

# Hierarchy when you need it: one instance per site
site: example.com
	root: /srv/www/example
	max-upload-mb: 50  # names are case-insensitive, spacing is loose
	methods: GET, POST, HEAD  # an array is just commas
	tls:
		hsts: true

site: blog.example.com
	root: /srv/www/blog
```

A setter reports failure - `false`, or `0` in C - when a path cannot be written at all, a wildcard being the common case, since those are query-only. `write_reason(path)` names which of the five reasons applies.

### Bash

The shell wrappers are not parsers; they wrap the CLI, which is why they inherit its conformance for free. Source one and you get typed sugar over the same commands:

- Install: install the CLI, source the wrapper
- Dependency line: n/a - it wraps the CLI

```bash
source shcl.bash

workers=$(shcl_int --default=4 server.shcl workers)
root=$(shcl_get --default='' server.shcl 'site[example.com].root')

# --set is repeatable and applies in order; --write rewrites the file in place.
shcl set --write server.shcl \
    --set "workers=$((workers * 2))" \
    --set 'site[example.com].tls.hsts=true' \
    --set 'site[blog.example.com].root=/srv/www/blog'
```

A `--set` value goes in as literal config text, so its type follows the text: `workers=8` writes an integer, `name=hello` a string. That covers scalars, but not an array (the comma would make it a quoted string) or a value you need forced to a string. Those, along with raw blocks, set-only-if-absent and removal, go in as a write-ops script on stdin - one op per line, fields separated by a literal tab:

```bash
shcl set --write server.shcl <<OPS
string-array	cluster.hosts	a.example.com	b.example.com
remove	site[old.example.com]
OPS
```

### PowerShell

Dot-source it for the same helper names:

```powershell
. ./shcl.ps1

$workers = [int](shcl_int --default=4 server.shcl workers)
$root    = shcl_get --default='' server.shcl 'site[example.com].root'

shcl set --write server.shcl `
         --set "workers=$($workers * 2)" `
         --set 'site[example.com].tls.hsts=true' `
         --set 'site[blog.example.com].root=/srv/www/blog'
```

The op-script form works here too - the sourced `shcl` forwards pipeline input to the binary:

```powershell
$ops = "string-array`tcluster.hosts`ta.example.com`tb.example.com",
       "remove`tsite[old.example.com]"
$ops | shcl set --write server.shcl
```

## Installation

The latest release, `v1.0.0`, has packages, prebuilt CLI binaries, and a checksums file on the [releases page](https://github.com/jim-collier/shcl/releases).

### Language packages

Each binding is published where its own ecosystem looks for it, all under the name `shcl`: [crates.io](https://crates.io/crates/shcl) for Rust, [PyPI](https://pypi.org/project/shcl/) for Python, and the [Go module](https://pkg.go.dev/github.com/jim-collier/shcl/source/go) for Go.

Two of them carry the CLI as well as the library, which is the easiest way to get the binary on a platform with no prebuilt one - macOS and the BSDs included:

#### Cargo

Library as well as CLI

```sh
cargo install shcl
```

#### Go module

```sh
go install github.com/jim-collier/shcl/source/go/cmd/shcl@latest  # Go: CLI
```

#### PyPi

The PyPI distribution is the library module by itself and installs no command, so `pip install shcl` is a dependency, not an installation:

```sh
pip install shcl        # Python: library only
```

#### C and C++

C and C++ have no registry worth targeting, and need none: `shcl.h` is a single dependency-free header you vendor. Copy it out of a release tag, or take it from an installed package under `/usr/share/shcl/code/`.

For version pinning and the dependency line per ecosystem, see [Example use-cases in your code](#example-use-cases-in-your-code).

### Other installation options

#### OS-level packages and installers

The simplest route, if your system has a package manager. Download the `.deb`, `.rpm`, or Windows setup for your architecture (`x86_64` or `arm64`) from the releases page.

Packages put the binary at `/usr/bin/shcl`, and the drop-in sources and shell wrappers under `/usr/share/shcl/`.

##### Debian

```sh
sudo dpkg -i shcl-1.0.0-linux-x86_64.deb
```

##### Fedora, RHEL, openSUSE

```sh
sudo rpm -i  shcl-1.0.0-linux-x86_64.rpm
```

##### Windows

Run `shcl-1.0.0-windows-x86_64-setup.exe`. It installs to `C:\Program Files\Shcl`, adds that to `PATH`, and can uninstall itself later.

#### Scripted installation direct from web - dev or stable

Downloads a release, checks its signature, and installs the binary plus the drop-in files and wrappers. Idempotent. It states its plan and asks before touching anything. The default channel is `dev`, which means the newest release including pre-releases. Pass `stable` to take the newest full release only.

Each release ships a `sha256sums.txt` and a detached `.sig` over it. Both installers carry the release public key and verify that signature *before* reading any checksum out of the file, so replacing a release asset is not enough to get past them. On Linux this needs `openssl`, alongside `curl` or `wget`; there is no install-anyway fallback, so use the [DIY install](#diy-install) route on a machine that lacks it.

Options are `--release <dev|stable>`, `--target <user|system>`, and `--yes` to skip the prompt (`-Release`, `-Target`, `-Yes` on Windows).

##### Linux and WSL

```sh
curl -fsSL https://raw.githubusercontent.com/jim-collier/shcl/main/install.bash | bash
```

To pass options on Linux, add them after the pipe:

```sh
curl -fsSL https://raw.githubusercontent.com/jim-collier/shcl/main/install.bash | bash -s -- --target=user
```

##### Windows (PowerShell)

```powershell
irm https://raw.githubusercontent.com/jim-collier/shcl/main/install.ps1 | iex
```

On Windows, `irm | iex` cannot take arguments at all, so use the scriptblock form:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/jim-collier/shcl/main/install.ps1))) -Target user
```

| Target | Linux | Windows
| :-- | :-- | :--
| `system` (default) | `/opt/shcl` plus a `/usr/local/bin/shcl` symlink | `C:\Program Files\Shcl`, added to `PATH`
| `user` | `~/.local/share/shcl` plus a `~/.local/bin/shcl` symlink | `%LOCALAPPDATA%\Programs\Shcl`, added to your `PATH`

A `user` install needs no sudo or elevation.

macOS and the BSDs have no prebuilt binaries yet. Use `cargo install shcl`, a drop-in source file, or build the CLI.

#### DIY install

- **Prebuilt binary**. Grab the `shcl` binary for your platform from the releases page and put it anywhere on your `PATH`. To check it by hand, download the sums file, its `.sig`, and [`shcl-signing.pub`](shcl-signing.pub) from the repo, then verify the signature before the checksum. A checksum out of an unverified sums file proves nothing:

	```sh
	openssl dgst -sha256 -verify shcl-signing.pub \
		-signature shcl-1.0.0-sha256sums.txt.sig shcl-1.0.0-sha256sums.txt
	sha256sum -c --ignore-missing shcl-1.0.0-sha256sums.txt
	```

- **Drop-in source**. Copy one file into your project. No dependency, no build step. Rust `source/rust/src/lib.rs`, Go `source/go/shcl.go`, Python `source/python/shcl.py`, C `source/c/shcl.h`.

- **Build the CLI**. The reference lives in `source/rust/` and has zero dependencies:

	```sh
	cargo build --release --manifest-path source/rust/Cargo.toml
	# binary at source/rust/target/release/shcl
	```

	Each other binding builds with its own toolchain (`go build`, a C compiler, a Python interpreter). All of them run the same conformance corpus.

## Set up a development environment

`install-dev.bash` clones the repo, installs the toolchains and linters the pipeline gates on as far as it can without sudo, and prints the package-manager hint for anything left over. It states its plan first:

```sh
curl -fsSL https://raw.githubusercontent.com/jim-collier/shcl/main/install-dev.bash | bash
```

Linux and macOS; on Windows, use WSL, since the pipeline is bash. Then, from the clone:

```sh
cicd/cicd.bash --ci
```

`--ci` is the whole gate: format check, build, lint, tests, and the cross-binding differential. It is the same thing GitHub runs, so green locally means green upstream.

[`contributing.md`](contributing.md) has the full list - every toolchain and linter with its install command, the per-binding test commands, the extra tools a full cross-and-package run wants, and how to add a conformance case.

## Docs

- [`project/spec.md`](project/spec.md): the full language spec. Terminology, types, coercion, the read API, raw blocks, strictness levels.

- [`project/grammar.abnf`](project/grammar.abnf): the formal grammar.

- [`project/design.md`](project/design.md): the why behind the decisions.

- [`contributing.md`](contributing.md): how to help.

- [`style-guide.md`](style-guide.md): coding and prose style. The bindings deliberately mirror the reference's structure over per-language idiom, so they stay byte-for-byte in sync.

Generated API reference, per binding: [docs.rs](https://docs.rs/shcl) for Rust, [pkg.go.dev](https://pkg.go.dev/github.com/jim-collier/shcl/source/go) for Go.

## Contributing and support

This product has to be 100% bulletproof. Help is welcome. Bug reports, spec edge cases, and new-language bindings are all invaluable. See [`contributing.md`](contributing.md) to get started.

If SHCL helps but you can't contribute code or Issue reports, a star or a mention still helps other people find it.

## Legal stuff

> Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)<br />
> Licensed under the [MIT License](https://mit-license.org/)<br />
> SPDX-License-Identifier: `MIT`<br />
> No warranty.<br />
> SHCL™ is a [trademark](trademark.md) of Jim Collier. The name means it passes the conformance corpus.
