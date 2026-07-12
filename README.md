<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->
<div align="center">

[![made-with-rust](https://img.shields.io/badge/Made%20with-Rust-1f425f.svg)](https://www.rust-lang.org/)
![Go](https://img.shields.io/badge/Go-00ADD8?logo=go&logoColor=white)
![Made with](https://img.shields.io/badge/Made%20with-C%2B%2B-brightgreen?style=plastic)
[![made-with-python](https://img.shields.io/badge/Made%20with-Python-1f425f.svg)](https://www.python.org/)
[![made-with-javascript](https://img.shields.io/badge/Made%20with-JavaScript-1f425f.svg)](https://www.javascript.com)
[![!#/bin/bash](https://img.shields.io/badge/-%23!%2Fbin%2Fbash-1f425f.svg?logo=gnu-bash)](https://www.gnu.org/software/bash/)
![Lifecycle: Alpha](https://img.shields.io/badge/Lifecycle-Alpha-orange)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
![Support](https://img.shields.io/badge/Support-Maintained-brightgreen)

</div>
<!--
[![!#/bin/bash](https://img.shields.io/badge/-%23!%2Fbin%2Fbash-1f425f.svg?logo=gnu-bash)](https://www.gnu.org/software/bash/)
[![made-with-python](https://img.shields.io/badge/Made%20with-Python-1f425f.svg)](https://www.python.org/)
[![made-with-rust](https://img.shields.io/badge/Made%20with-Rust-1f425f.svg)](https://www.rust-lang.org/)
![Go](https://img.shields.io/badge/Go-00ADD8?logo=go&logoColor=white)
![Made with](https://img.shields.io/badge/Made%20with-C%2B%2B-brightgreen?style=plastic)
![Made with](https://img.shields.io/badge/Made%20with-Unreal%20Engine-critical?style=plastic)
[![made-with-javascript](https://img.shields.io/badge/Made%20with-JavaScript-1f425f.svg)](https://www.javascript.com)
![License: GPL v2](https://img.shields.io/badge/License-GPLv2-blue.svg)
![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![License: MPL 2.0](https://img.shields.io/badge/License-MPL_2.0-brightgreen.svg)](https://opensource.org/licenses/MPL-2.0)
![Lifecycle: Alpha](https://img.shields.io/badge/Lifecycle-Alpha-orange)
![Lifecycle: Beta](https://img.shields.io/badge/Lifecycle-Beta-yellow)
![Lifecycle: RC](https://img.shields.io/badge/Lifecycle-RC-blue)
![Lifecycle: Stable](https://img.shields.io/badge/Lifecycle-Stable-brightgreen)
![Lifecycle: Deprecated](https://img.shields.io/badge/Lifecycle-Deprecated-red)
![Status: Deprecated](https://img.shields.io/badge/Status-Deprecated-orange)
![Status: Archived](https://img.shields.io/badge/Status-Archived-lightgrey)
![Lifecycle: EOL](https://img.shields.io/badge/Lifecycle-EOL-lightgrey)
![Coverage](https://img.shields.io/badge/Coverage-25%25-red)
![Coverage](https://img.shields.io/badge/Coverage-50%25-orange)
![Coverage](https://img.shields.io/badge/Coverage-75%25-yellow)
![Coverage](https://img.shields.io/badge/Coverage-90%25-brightgreen)
![Status: Passing](https://img.shields.io/badge/Status-Passing-brightgreen)
![Status: Failing](https://img.shields.io/badge/Status-Failing-red)
-->

<!-- TOC ignore:true -->
# SHCL

<table style="border: none; border-collapse: collapse;">
	<tr style="border: none; border-collapse: collapse;">
		<td style="border: none; border-collapse: collapse;"><img src="assets/logo.png" alt="Logo" width="320"/></td>
		<td style="border: none;"><b>S</b>imple <b>H</b>ierarchical <b>C</b>onfig <b>L</b>anguage</td>
	</tr style="border: none; border-collapse: collapse;">
</table>

Forgiving to write. Predictable to read. The friendliest read API in the space.

<!-- TOC ignore:true -->
## Table of contents

<!-- TOC -->

- [The problem](#the-problem)
- [What SHCL does about it](#what-shcl-does-about-it)
- [What it looks like](#what-it-looks-like)
- [Reading it from code](#reading-it-from-code)
- [How it compares](#how-it-compares)
	- [Everyday config formats](#everyday-config-formats)
	- [The programmable ones: Pkl, CUE, Dhall](#the-programmable-ones-pkl-cue-dhall)
	- [When SHCL is the wrong choice](#when-shcl-is-the-wrong-choice)
- [Features](#features)
- [Status](#status)
- [Installing](#installing)
- [Building from source](#building-from-source)
- [Docs](#docs)
- [Copyright and license](#copyright-and-license)

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

SHCL flips that around. Modern CPU cycles are cheap. Brainpower isn't. So the parser does the hard work, not the person writing the file - and not the programmer reading it.

- If a human can tell what a line means, the parser figures it out too.
- One broken line never takes down the file. It is skipped with a note, and everything else still loads.
- Types live in your code, not in the file. The file stores text; you ask for an int when you read it. Nothing gets guessed at parse time, so there is no "Norway Problem".
- Every convenience read states a fallback at the call site. A missing value cannot sneak in as a silent zero.

When you do want zero-tolerance rigor: schema validation, plus a strict mode that fails loudly.

## What it looks like

All of this is one valid file. Indentation and dotted paths are interchangeable, quoting is only needed when a value contains a reserved character, and messy spacing is fine.

```text
# Indented style
base: Chicago
	metrics:
		Population : 30200
		weather: Hot, Cold, "All around not that great"

# Same tree, inline style
base[Boston].metrics.population: 700

# Adding to an instance defined earlier just works
base: Chicago
	metrics:
		square-miles: 300

# Multi-line content goes in a fenced block, kept verbatim
schema:
	~~~sql
	CREATE TABLE users (
		id   INTEGER PRIMARY KEY,
		name TEXT NOT NULL
	);
	~~~
```

Field names are case-insensitive. Repeated paths merge. `base` here is not one key but a set of instances (Chicago, Boston), which is how you write arrays of objects without inventing syntax for them.

## Reading it from code

One call. A typed value. A visible fallback. This is the call you write 90% of the time:

```go
pop := doc.GetIntOr("base[Boston].metrics.population", 0)
```

```python
pop = doc.get_int("base[Boston].metrics.population", default=0)
```

```sh
pop=$(shcl get --int --default=0 config.shcl 'base[Boston].metrics.population')
```

When you need to know *why* a read failed, the full form returns a status instead: `Good`, `Empty`, `NotFound`, `BadType`, or `Multiple`. Wildcards read across instances (`base[*].metrics.population` gives you every city's population, in file order).

## How it compares

### Everyday config formats

| | SHCL | JSON | YAML | TOML | XML
| :-- | :-- | :-- | :-- | :-- | :--
| Comments | yes | no | yes | yes | yes
| Unquoted strings | yes | no | yes, but they can change type on you | no | n/a
| One bad line breaks the whole file | no | yes | yes | yes | yes
| Who decides a value's type | your code, at read time | the file | the parser guesses | the file | your code
| Deep nesting | indent or dot paths, either or both | brace pyramids | indent only, whitespace-fragile | `[a.b.c]` headers get old fast | tag soup
| Multi-line verbatim blocks | fenced, like Markdown | escaped strings | block scalars, with rules to memorize | multi-line strings | CDATA
| Hand-editable by a non-programmer | ✅ | risky | risky | ✅ mostly | 🚫
| Tells you what it fixed | ✅ structured diagnostics | 🚫 | 🚫 | 🚫 | 🚫

A note on that type row, because it is the big design difference. JSON and TOML store types in the file, so the author has to get them right. YAML infers types from the text, which is where `NO` becomes `false`. SHCL stores plain text and coerces when *you* ask for a type, so the only code that decides a value is an int is the code that needed an int.

### The programmable ones: Pkl, CUE, Dhall

These are a different species. They make the config file itself powerful.

- **Pkl** (from Apple) is a real language: classes, inheritance, built-in validation. Great when your config genuinely is a program.
- **CUE** unifies types and values into one thing. Extremely strong validation, and a mental model that takes real time to absorb.
- **Dhall** is functional programming for config: imports, functions, guaranteed termination. Closer to writing Haskell than editing a file.

They are all good at what they do. The shared cost is that once a config file can compute, it can be wrong in ways you have to debug.

SHCL deliberately stays off that cliff. The file stays dumb, and the power moves into the library instead:

- **Schema validation.** A schema is just another SHCL file. `Validate(doc, schema)` catches unknown fields, wrong types, and out-of-range values, including the "did you mean `enabled`?" typo case.
- **Layered loading.** `Load(defaults, site, user)` merges files in order, with CLI and environment overrides on top. That covers most of what people actually use imports for.
- **Generated starter configs.** The schema plus the writer can emit a fully commented, correctly typed starting file.

Your config never needs a debugger, and a non-programmer can still edit it.

### When SHCL is the wrong choice

- You need expressions, functions, or imports inside the file itself. Use Pkl (arguably best), CUE, or Dhall.
- You are serializing machine-to-machine data at high volume. Use JSON or something binary; SHCL is for files that humans use.

## Features

- Hierarchy by indentation or dot-notation (`base[Boston].metrics.population: 700`), freely mixed. Both spell the same tree.
- Values are typed on *read*, not on parse. The file stores text; your code asks for an int.
- Never bails on a whole file over one bad line. Bad lines are skipped or repaired with diagnostics, and the rest still loads.
- Every convenience read takes a call-site fallback (`GetIntOr(path, 0)`), so a missing value can't masquerade as a real zero.
- Three strictness levels. Loose, standard, strict: one knob from maximum-forgiving to fail-on-anything.
- Schema validation, layered loading (defaults, site, user), and commented starter-config generation, all as library features.
- Raw fenced blocks embed anything verbatim: SQL, code, templates, Markdown-style.
- One conformance corpus pins every shipped binding to identical behavior. Rust reference implementation plus the `shcl` CLI first; more bindings gated behind the corpus.

## Status

Alpha, and spec-first on purpose. Five parsers that "mostly agree" would be worse than none, so the order of work is:

1. Language spec and formal grammar. Done: [`project/spec.md`](project/spec.md), [`project/grammar.abnf`](project/grammar.abnf).
2. A conformance corpus of golden test cases that every future parser must pass. Started: [`project/conformance/`](project/conformance/).
3. The Rust reference parser and the `shcl` CLI. Next up. Nothing ships until it is corpus-green.
4. Go, C, and Python bindings, each held to the same corpus. More after v1.0.

Star or watch the repo if you want to know when the parser lands.

## Installing

Nothing to install yet. The `shcl` CLI and the first library builds arrive with the Rust reference implementation (see Status).

## Building from source

Nothing to build yet, for the same reason.

## Docs

- [`project/spec.md`](project/spec.md): the full language spec. Terminology, types, coercion, the read API, raw blocks, strictness levels.
- [`project/grammar.abnf`](project/grammar.abnf): the formal grammar.
- [`project/design.md`](project/design.md): the why behind the decisions.
- [`contributing.md`](contributing.md): how to help.

## Copyright and license

> Copyright © 2026 Jim Collier (ID: 1cv◂‡Vᛦ)<br />
> Licensed under the [MIT License](https://mit-license.org/). No warranty.
<!--
> Licensed under the [MIT License](https://mit-license.org/). No warranty.
> Licensed under the [GNU General Public License v2.0](https://www.gnu.org/licenses/gpl-2.0.html). No warranty.
> Licensed under the [GNU General Public License v2.0 or later](https://spdx.org/licenses/GPL-2.0-or-later.html). No warranty.
> Licensed under the [GNU General Public License v3](https://www.gnu.org/licenses/gpl-3.0.en.html) license. No warranty.
> Licensed under the [Mozilla Public License 2.0](https://mozilla.org/MPL/2.0/). No warranty.
-->
