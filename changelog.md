# Changelog

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## v1.0.0-beta1

Pre-release alpha. Nothing tagged yet.

### Added

- Language spec and formal grammar (`project/spec.md`, `project/grammar.abnf`).
- Conformance corpus of golden cases that every binding must pass.
- Rust reference parser and the `shcl` CLI (`get`, `fmt`, `check`, `count`, `instances`).
- Independent parsers for Go, C (with a C++ veneer), and Python, each corpus-green.
- Bash wrapper around the CLI, usable as a script or sourced for typed helpers.
- Three strictness levels (loose, standard, strict) as a per-document knob.
- Raw fenced blocks for verbatim multi-line content.
