#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

# shcl CLI - the Python binding's command surface. Flags, output, and exit codes
# mirror the Rust reference exactly; the cicd cross-binding check compares them
# byte for byte, so any drift here fails the pipeline.

import math
import os
import signal
import sys

# The single-file library sits two directories up (lib in source/python/, CLI in
# source/python/cmd/shcl/, mirroring the Go layout).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__)))))
import shcl

class _BestEffort:
	"""stderr, wrapped so a write that fails is dropped. A stream that cannot
	be written has nowhere to report that fact, and the document on stdout is
	still good - where an uncaught OSError would lose that too."""

	def __init__(self, stream):
		self._stream = stream

	def write(self, text):
		try:
			self._stream.write(text)
		except OSError:
			pass
		return len(text)

	def flush(self):
		try:
			self._stream.flush()
		except OSError:
			pass

	def __getattr__(self, name):
		return getattr(self._stream, name)


def write_failed(e):
	"""A stdout write that failed. A reader that closed early is nothing to
	report - nobody is there to read it - so that leaves quietly; anything
	else lost the output, which is the same failure as a file that could not
	be written. The stream is swapped for a sink so the interpreter's own
	exit-time flush does not fail again over the top of the exit code."""
	sys.stdout = open(os.devnull, "w", encoding="utf-8")  # noqa: SIM115
	if isinstance(e, BrokenPipeError):
		return 0
	sys.stderr.write(f"stdout: {e.strerror or e}\n")
	return 8


# Keep in step with source/rust/Cargo.toml, the canonical version source.
VERSION = "2.0.0"

HELP = """shcl - Simple Hierarchical Config Language (reference CLI)

Usage:
  shcl get [type] [options] FILE PATH    read one value (or array) at a path
  shcl set [--write|-w] [options] FILE   apply edits (--set, or ops on stdin);
                                         print canonical (or rewrite FILE in
                                         place with --write)
  shcl fmt [--write|-w] FILE             print the canonical form (or rewrite
                                         FILE in place with --write)
  shcl check [options] FILE              load and print diagnostics
                                         (--schema=SCHEMA also validates FILE
                                         against a schema, itself a .shcl file)
  shcl init [--no-banner] --schema=S     print a commented starter config
                                         from a schema (required fields live,
                                         optional commented, wildcards noted)
  shcl count [options] FILE PATH         number of instances at a path
  shcl instances [options] FILE PATH     instance values at a path, one per line
  shcl children [options] FILE [PATH]    child field names under a path, one per
                                         line (the top level when PATH is left
                                         out)
  shcl paths [options] FILE              every field path in the document, one
                                         per line
  shcl help | version                    this help, or the version (also
                                         -h/--help, -v/-V/--version)
  shcl about | donate                    what shcl is, or how to support it
                                         (also --about, --donate)

set edits FILE, the base document. Values go in as repeatable --set PATH=VALUE
(data) or --set-literal PATH=TEXT (value syntax, so arrays work) options, which
persist with --write; given either, no ops are read from stdin. Raw blocks,
set-only-if-absent and removal go in as a write-ops script on stdin, one op per
line, tab-separated. FILE '-' follows stdin: the document when an option holds
the edits, an empty base when the ops script has stdin instead. With --write,
a FILE that does not exist yet is created. PATH ends at the first '=' outside
quotes and brackets, so a selector may hold one. Ops:
  int|float|bool|string|datetime<TAB>PATH<TAB>VALUE       set a scalar
  <type>-array<TAB>PATH<TAB>V1<TAB>V2...                  set an inline array
  <type>[-array]-default<TAB>...                          set only if absent
  literal[-default]<TAB>PATH<TAB>TEXT                     set from value syntax
  raw<TAB>PATH<TAB>INFO<TAB>CONTENT                       set a raw block
  empty<TAB>PATH   comment<TAB>PATH<TAB>TEXT   remove<TAB>PATH
string/raw values decode \\n \\t \\\\; a line starting with # is a script comment.

Types (get only; default --string):
  --int --float --bool --datetime --string --raw --rawinfo
  --array                                read the value as an array of the type
  --rawinfo reads a raw block's info-string (the fence tag), not its content

Options (the subcommands each belongs to are in parentheses):
  --default=VALUE                        (get) value to print when the read is
                                         not Good (implies --on-bad=default; for
                                         arrays, substituted per bad slot)
  --on-bad=error|default|flag            (get) error: fail loudly; default:
                                         print the default; flag: print the
                                         value anyway and report via exit code
                                         (the default)
  --slots                                (get) prefix each line with its slot
                                         status and a tab (per element, or per
                                         wildcard slot)
  --no-banner                            (init) leave out the footer naming the
                                         format and pointing at its spec
  --lossy                                (fmt/set) with --write, rewrite even
                                         when the load dropped lines this write
                                         would delete; without it the write
                                         refuses and nothing is changed
  --strictness=loose|standard|strict     (all but init) or 1|2|3 (default
                                         standard)
  --schema=SCHEMA                        (check/init) validate FILE against a
                                         schema; adds V### diagnostics
  --layer=FILE                           (all but check/init) merge a
                                         lower-priority layer under FILE;
                                         repeatable, earlier = lower priority
  --set=PATH=VALUE                       (all but check/init) override one path
                                         as the top layer, after all files;
                                         repeatable. On 'set' it is an edit to
                                         the document itself, so it persists
                                         with --write. VALUE goes in as data:
                                         its type still follows the text (8 is
                                         an int), but a comma or quote in it is
                                         content, not syntax
  --set-literal=PATH=TEXT                (same subcommands) as --set, except
                                         TEXT goes in as value
                                         syntax the way a file spells it, so
                                         'ports=80, 443' writes a two-element
                                         array. An unquoted # ends the value;
                                         text spanning lines is rejected
  --set-default=PATH=VALUE               (same) as --set, but only when nothing
  --set-literal-default=PATH=TEXT        is at the path yet - the write-out-
                                         defaults half of the writer
  --remove=PATH                          (same) delete what is at the path,
                                         with its subtree. Removing nothing is
                                         not an error
The five above share one ordered list, so two of them touching the same path
resolve in the order given. Raw blocks still go in through the ops script.

Value options accept either spelling: --default=VALUE or --default VALUE. In
the space form the next argument is taken as the value whatever it looks like,
so --default --int reads --int as the default. Use -- to end the options when a
FILE or PATH begins with a dash.
An option a subcommand does not use is a usage error, not ignored. Also
refused: --write with --layer; --write with --set outside 'set'; --lossy
without --write; --layer=- on 'set'; --array with --raw or --rawinfo; '-'
named more than once across FILE, --layer and --schema.
Every subcommand that loads a document prints the load's diagnostics to stderr,
once per run. An in-place write also refuses when the load dropped content the
rewrite would delete (--lossy overrides).
FILE may be '-' for stdin. With --layer, FILE is the highest file layer and
each --layer is merged under it in order; --set applies last. 'fmt' with
layers prints the merged canonical document.

Exit codes: 0 good, 1 usage error, 2 empty, 3 not found, 4 bad type,
5 multiple instances, 6 check failed, strict load failed, or init's schema
has faults, 7 in-place write refused (--lossy overrides), 8 a file or stream
could not be read or written.
"""

# About and donate are stdout, so they are byte-for-byte contracts across the
# bindings the same way the help text and the init banner are. The version
# concatenates from the constant above so it cannot drift from `shcl version`.
ABOUT = "shcl v" + VERSION + """
Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞).
Project: https://github.com/jim-collier/shcl
Licensed under the MIT License. Full text at:
  https://spdx.org/licenses/MIT.html
No warranty.

Simple Hierarchical Config Language. Forgiving to write, predictable to read.
Types live in your code, not in the file, so nothing is guessed at parse time.
One broken line is skipped with a note instead of taking down the whole file.
"""

DONATE = """shcl is free software under the MIT License, and stays that way.

If it saves you time and you want to give something back:
  https://github.com/sponsors/jim-collier

A star on the project, a clear bug report, or a mention to someone who needs it
are worth just as much.
"""


def status_code(st):
	return st.value


class _SetOpt:
	# One edit from the --set family. All five spellings share a list so they
	# apply in the order given, which is what decides the winner when two target
	# the same path.
	__slots__ = ("path", "value", "kind")

	def __init__(self, path, value, kind):
		self.path = path
		self.value = value
		self.kind = kind

	def apply(self, doc):
		if self.kind == "--set-literal":
			return doc.set_literal(self.path, self.value)
		if self.kind == "--set-default":
			return doc.set_string_default(self.path, self.value)
		if self.kind == "--set-literal-default":
			return doc.set_literal_default(self.path, self.value)
		if self.kind == "--remove":
			# Removing nothing is not a failure, the same as the ops script's
			# `remove`: the point of the option is the path's absence after.
			doc.remove(self.path)
			return True
		return doc.set_string(self.path, self.value)

	def opt(self):
		return self.kind


class _Opts:
	__slots__ = ("kind", "array", "slots", "default", "on_bad", "strictness", "write", "lossy", "no_banner", "schema", "layers", "sets", "args", "seen")

	def __init__(self):
		self.kind = "string"     # int|float|bool|datetime|string|raw
		self.array = False
		self.slots = False
		self.default = None
		self.on_bad = "flag"     # error|default|flag
		self.strictness = shcl.Strictness.Standard
		self.schema = None
		self.write = False
		self.lossy = False
		self.no_banner = False
		self.layers = []         # lower-priority layers, in listed order
		self.sets = []           # final override layer: _SetOpt, in the order given
		self.args = []           # positional: FILE [PATH]
		self.seen = []           # canonical names of options given, for per-command validation


def _ascii_lower(s):
	# ASCII-only folding, as the reference's to_ascii_lowercase; str.lower()
	# folds the whole of Unicode.
	return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in s)


def _set_value_opt(o, name, v):
	if name == "--default":
		o.default = v
		o.on_bad = "default"
		o.seen.append("--default")
	elif name == "--on-bad":
		low = _ascii_lower(v)
		if low not in ("error", "default", "flag"):
			raise ValueError(f"bad --on-bad value: {v}")
		o.on_bad = low
		o.seen.append("--on-bad")
	elif name == "--strictness":
		s = shcl.Strictness.from_arg(v)
		if s is None:
			raise ValueError(f"bad --strictness value: {v}")
		o.strictness = s
		o.seen.append("--strictness")
	elif name == "--schema":
		o.schema = v
		o.seen.append("--schema")
	elif name == "--layer":
		o.layers.append(v)
		o.seen.append("--layer")
	elif name == "--remove":
		if v == "":
			raise ValueError("bad --remove value (want PATH)")
		o.sets.append(_SetOpt(v, "", "--remove"))
		o.seen.append("--remove")
	elif name in ("--set", "--set-literal", "--set-default", "--set-literal-default"):
		ps = split_set(v)
		if ps is None or ps[0] == "":
			raise ValueError(f"bad {name} value (want PATH=VALUE, quotes and brackets balanced): {v}")
		o.sets.append(_SetOpt(ps[0], ps[1], name))
		o.seen.append(name)


def split_set(arg):
	# PATH=VALUE at the first `=` outside quotes and brackets, so a selector
	# holding one (`x[a=b].c=1`) still addresses its instance.
	in_quote = None
	depth = 0
	i = 0
	n = len(arg)
	while i < n:
		c = arg[i]
		if c == "\\":
			i += 2
			continue
		if in_quote is not None:
			if c == in_quote:
				in_quote = None
		elif c == '"' or c == "'":
			in_quote = c
		elif c == "[":
			depth += 1
		elif c == "]":
			depth = max(depth - 1, 0)
		elif c == "=" and depth == 0:
			return arg[:i], arg[i + 1:]
		i += 1
	return None


def asked_for(argv):
	# Did the command line ask for one of the informational outputs? Only tokens
	# in option position count: the value of a value-taking option and anything
	# after `--` are data (a FILE or PATH spelled `-h` needs the `--` anyway,
	# since the option parser would refuse it). Scanning values too once let a
	# read of a missing path answer with the help text and exit 0.
	i = 0
	while i < len(argv):
		a = argv[i]
		if a in ("-h", "--help"):
			return "help"
		if a in ("-v", "-V", "--version"):
			return "version"
		if a == "--about":
			return "about"
		if a == "--donate":
			return "donate"
		if a == "--":
			return None
		if a in ("--default", "--on-bad", "--strictness", "--schema", "--layer", "--set", "--set-literal", "--set-default", "--set-literal-default", "--remove"):
			i += 1
		i += 1
	return None


def kind_from_opt(opt):
	# The type option's kind, or None when the token is not one.
	if opt in ("--int", "--float", "--bool", "--datetime", "--string", "--raw", "--rawinfo"):
		return opt[2:]
	return None


def parse_opts(argv):
	o = _Opts()
	# Value-taking options accept both --opt=VALUE and the space form --opt VALUE.
	i = 0
	while i < len(argv):
		a = argv[i]
		# Everything after `--` is positional, so a file or path may begin
		# with a dash.
		if a == "--":
			o.args.extend(argv[i + 1:])
			return o
		k = kind_from_opt(a)
		if k is not None:
			o.kind = k
			o.seen.append("--<type>")
			i += 1
			continue
		if a == "--array":
			o.array = True
			o.seen.append("--array")
		elif a == "--slots":
			o.slots = True
			o.seen.append("--slots")
		elif a == "--no-banner":
			o.no_banner = True
			o.seen.append("--no-banner")
		elif a in ("--write", "-w"):
			o.write = True
			o.seen.append("--write")
		elif a == "--lossy":
			o.lossy = True
			o.seen.append("--lossy")
		elif a in ("--default", "--on-bad", "--strictness", "--schema", "--layer", "--set", "--set-literal", "--set-default", "--set-literal-default", "--remove"):
			i += 1
			if i >= len(argv):
				raise ValueError(f"missing value for {a} (try {a}=VALUE)")
			_set_value_opt(o, a, argv[i])
		elif a.startswith("--default="):
			_set_value_opt(o, "--default", a[len("--default="):])
		elif a.startswith("--on-bad="):
			_set_value_opt(o, "--on-bad", a[len("--on-bad="):])
		elif a.startswith("--strictness="):
			_set_value_opt(o, "--strictness", a[len("--strictness="):])
		elif a.startswith("--schema="):
			_set_value_opt(o, "--schema", a[len("--schema="):])
		elif a.startswith("--layer="):
			_set_value_opt(o, "--layer", a[len("--layer="):])
		elif a.startswith("--set-literal-default="):
			_set_value_opt(o, "--set-literal-default", a[len("--set-literal-default="):])
		elif a.startswith("--set-literal="):
			_set_value_opt(o, "--set-literal", a[len("--set-literal="):])
		elif a.startswith("--set-default="):
			_set_value_opt(o, "--set-default", a[len("--set-default="):])
		elif a.startswith("--remove="):
			_set_value_opt(o, "--remove", a[len("--remove="):])
		elif a.startswith("--set="):
			_set_value_opt(o, "--set", a[len("--set="):])
		elif a.startswith("-") and len(a) > 1:
			raise ValueError(f"unknown option: {a}")
		else:
			o.args.append(a)
		i += 1
	return o


# A file or stream that could not be read or written. Its own code since a
# script's remedy - fix the path, the permissions, the disk - has nothing to do
# with the remedy for a usage error, which keeps 1.
EXIT_IO = 8


def read_input(file):
	if file == "-":
		data = sys.stdin.buffer.read()
	else:
		with open(file, "rb") as f:
			data = f.read()
	# The reference reads as UTF-8 and fails on bad bytes; match its exit path.
	try:
		return data.decode("utf-8")
	except UnicodeDecodeError as e:
		raise ValueError(f"{file}: stream did not contain valid UTF-8") from e


def load_doc(text, strictness):
	# Returns (doc, None) or (None, code). On strict load failure, prints the
	# reference's diagnostic lines to stderr and reports code 6.
	try:
		return shcl.Document.parse_with(text, strictness), None
	except shcl.LoadError as le:
		say_diagnostics(le.diagnostics)
		errors = sum(1 for d in le.diagnostics if d.severity == shcl.Severity.Error)
		sys.stderr.write(f"strict load failed: {errors} error diagnostic(s)\n")
		return None, 6


def write_back(doc, file, o):
	# The in-place half of fmt/set. Overwriting the source is the one place a
	# recovered load turns destructive, so the save runs through the library's
	# own gate rather than a second copy of the rule - the CLI and a consumer
	# program cannot then disagree about which rewrites are safe.
	try:
		if o.lossy:
			doc.save_file_lossy(file)
		else:
			doc.save_file(file)
		return 0
	# The rule stays in the library; only the wording is the CLI's, because the
	# override a user has here is a flag, not a function.
	except shcl.SaveRefused as e:
		sys.stderr.write(f"{file}: refusing to rewrite: the load dropped {e.lost} line(s)/value(s) this write would delete (--lossy overrides)\n")
		return 7
	except shcl.SaveError as e:
		sys.stderr.write(str(e) + "\n")
	return EXIT_IO


def load_layered(o, file):
	# Load file with o's lower-priority --layer files underneath and its --set
	# overrides on top - the layered-load fold. Every layer parses at the
	# requested strictness; a strict-load failure on any layer aborts like a
	# single-file strict failure. Returns (doc, None) or (None, code).
	# It prints every layer's diagnostics itself, lowest first, before the --set
	# overrides run: they belong to the load, and a refused edit used to return
	# with nothing said about them. A merge does not carry diagnostics over, so
	# reading them off the merged document drops the ones for FILE itself, which
	# is the one the caller named.
	texts = []
	for lf in o.layers:
		texts.append(read_input(lf))
	texts.append(read_input(file))
	doc, code = load_doc(texts[0], o.strictness)
	if doc is None:
		return None, code
	diags = list(doc.diagnostics())
	for t in texts[1:]:
		over, c = load_doc(t, o.strictness)
		if over is None:
			return None, c
		diags.extend(over.diagnostics())
		doc.merge(over)
	say_diagnostics(diags)
	for st in o.sets:
		if not st.apply(doc):
			sys.stderr.write(f"{st.opt()}: cannot write {st.path}: {describe_refusal(doc, st.path)}\n")
			return None, 1
	return doc, None


def check_opts(cmd, o):
	# Every option must be meaningful for its subcommand; an option that would be
	# silently ignored (`set --write` before it existed, `--schema` on `get`) is a
	# usage error instead. Returns an exit code, or None to proceed.
	if cmd == "get":
		allowed = ("--<type>", "--array", "--slots", "--default", "--on-bad", "--strictness", "--layer", "--set", "--set-literal", "--set-default", "--set-literal-default", "--remove")
	elif cmd == "set":
		allowed = ("--strictness", "--layer", "--set", "--set-literal", "--set-default", "--set-literal-default", "--remove", "--write", "--lossy")
	elif cmd == "fmt":
		allowed = ("--write", "--lossy", "--strictness", "--layer", "--set", "--set-literal", "--set-default", "--set-literal-default", "--remove")
	elif cmd == "check":
		allowed = ("--strictness", "--schema")
	elif cmd == "init":
		allowed = ("--schema", "--no-banner")
	elif cmd in ("count", "instances", "children", "paths"):
		allowed = ("--strictness", "--layer", "--set", "--set-literal", "--set-default", "--set-literal-default", "--remove")
	else:
		allowed = ()
	for s in o.seen:
		if s not in allowed:
			if s == "--<type>":
				sys.stderr.write(f"type options are not valid for {cmd} (see --help)\n")
			elif cmd == "init" and s == "--strictness":
				# Deliberate, not an oversight: the schema is a program artifact,
				# so it always loads at Standard - the same rule `check --schema`
				# follows for the schema half.
				sys.stderr.write(
					"option --strictness not valid for init: a schema always loads at standard strictness, being a program artifact rather than user data\n"
				)
			elif cmd == "check" and s in ("--layer", "--set", "--set-literal"):
				# The one refusal a user is likely to want anyway: check reports
				# line numbers, and a merged document has no single file to
				# number against. Naming the pipeline turns a dead end into a
				# one-liner.
				sys.stderr.write(
					f"option {s} not valid for check: diagnostics cite line numbers, which a merged document has none of. Pipe instead: shcl fmt {s} ... FILE | shcl check --schema=SCHEMA -\n"
				)
			else:
				sys.stderr.write(f"option {s} not valid for {cmd} (see --help)\n")
			return 1
	# Writing back the merged document would fold the lower layers permanently
	# into the top file, which is the opposite of what layering is for. On 'set'
	# the --set values are edits to the document rather than a layer over it, so
	# persisting them is the whole point; everywhere else they stay ephemeral.
	if o.write and o.layers:
		sys.stderr.write("--write cannot be combined with --layer (see --help)\n")
		return 1
	if o.write and o.sets and cmd != "set":
		sys.stderr.write(f"--write cannot be combined with {o.sets[0].opt()} (see --help)\n")
		return 1
	# --lossy only overrides the in-place write's refusal, so on its own it says
	# nothing and would read as protection the command never had.
	if o.lossy and not o.write:
		sys.stderr.write("--lossy is only meaningful with --write (see --help)\n")
		return 1
	# The ops script already has stdin, so a layer cannot read it too.
	if cmd == "set" and any(lf == "-" for lf in o.layers):
		sys.stderr.write("--layer=- is not valid for set (stdin carries the ops script or the document)\n")
		return 1
	# Stdin reads once; a second '-' would silently get an empty document.
	stdin_uses = sum(1 for lf in o.layers if lf == "-") + int(o.schema == "-") + int(bool(o.args) and o.args[0] == "-")
	if stdin_uses > 1:
		sys.stderr.write("'-' (stdin) can be named only once across FILE, --layer and --schema\n")
		return 1
	return None


def describe_refusal(doc, path):
	# The per-binding wording behind a setter's bare False.
	reason = doc.write_reason(path)
	if reason == shcl.WriteReason.Writable:
		# The path itself is fine, so the value text must be what failed (a
		# literal that does not parse as one value).
		return "the value text is not one value"
	if reason == shcl.WriteReason.BadPath:
		return "not a usable path"
	if reason == shcl.WriteReason.ValueInPath:
		return "a path with a value part cannot be written"
	if reason == shcl.WriteReason.Wildcard:
		return "a wildcard path cannot be written"
	if reason == shcl.WriteReason.NoSuchIndex:
		return "no instance at that index"
	return "deeper than the nesting cap"


def say_diagnostics(diags):
	# The load's diagnostics, one line each, in the shape every command uses.
	for d in diags:
		space = "schema line" if d.code.startswith("V09") and d.code != "V099" else "line"
		sys.stderr.write(f"{space} {d.line}: {d.severity.name}: {d.code} {d.message}\n")


def _fmt_scalar(kind, value):
	if kind == "int":
		return str(value)
	if kind == "float":
		return shcl.format_float(value)
	if kind == "bool":
		return "true" if value else "false"
	# datetime / string / raw all stringify directly.
	return str(value)


def do_get(o):
	if len(o.args) != 2:
		sys.stderr.write("usage: shcl get [type] [options] FILE PATH (see --help)\n")
		return 1
	file, path = o.args[0], o.args[1]
	try:
		doc, code = load_layered(o, file)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return EXIT_IO
	if doc is None:
		return code
	if o.array:
		if o.kind == "int":
			r = doc.read_int_array(path)
			lines = [str(v) for v in r.value]
		elif o.kind == "float":
			r = doc.read_float_array(path)
			lines = [shcl.format_float(v) for v in r.value]
		elif o.kind == "bool":
			r = doc.read_bool_array(path)
			lines = ["true" if v else "false" for v in r.value]
		elif o.kind == "datetime":
			r = doc.read_datetime_array(path)
			lines = [str(v) for v in r.value]
		elif o.kind in ("raw", "rawinfo"):
			sys.stderr.write(f"--{o.kind} has no --array form\n")
			return 1
		else:
			r = doc.read_string_array(path)
			lines = r.value
		status = r.status
		slots = r.slots
	else:
		if o.kind == "int":
			r = doc.read_int(path)
		elif o.kind == "float":
			r = doc.read_float(path)
		elif o.kind == "bool":
			r = doc.read_bool(path)
		elif o.kind == "datetime":
			r = doc.read_datetime(path)
		elif o.kind == "raw":
			r = doc.read_raw(path)
		elif o.kind == "rawinfo":
			r = doc.read_raw_info(path)
		else:
			r = doc.read_string(path)
		if o.kind in ("string", "raw", "rawinfo"):
			lines = [r.value]
		else:
			lines = [_fmt_scalar(o.kind, r.value)]
		status = r.status
		slots = []

	def slot_at(i):
		# Per-line slot status: falls back to the aggregate for scalar reads.
		return slots[i] if i < len(slots) else status

	def emit(lns):
		for i, ln in enumerate(lns):
			if o.slots:
				print(f"{slot_at(i).name}\t{ln}")
			else:
				print(ln)

	# Why the read failed is worth saying even when the exit code already carries
	# it: at the default mode the user otherwise gets an empty line, a nonzero
	# code, and nothing to go on. Stdout is untouched - this only ever goes to
	# stderr. Two silences are deliberate: default mode, because a caller who
	# supplied a fallback has already said the miss is expected, and Empty
	# outside error mode, because an empty value is a legitimate answer here
	# rather than a failure - the same reason ok() counts it as fine.
	if (
		status != shcl.Status.Good
		and o.on_bad != "default"
		and (status != shcl.Status.Empty or o.on_bad == "error")
	):
		type_name = f"{o.kind} array" if o.array else o.kind
		if status == shcl.Status.BadType:
			raw = doc.read_string(path).raw
			reason = (
				f"value {quoted(raw)} is not a valid {type_name}"
				if raw is not None
				else f"value is not a valid {type_name}"
			)
		elif status == shcl.Status.NotFound:
			reason = "no value at that path"
		elif status == shcl.Status.Empty:
			reason = "the value is empty"
		else:
			reason = "the path matches multiple instances"
		sys.stderr.write(
			f"cannot read {path} as {type_name}: {reason} (in {file})\n"
		)
	if status == shcl.Status.Good or (status == shcl.Status.Empty and o.on_bad == "flag"):
		emit(lines)
		return status_code(status)
	if o.on_bad == "default":
		dv = o.default if o.default is not None else ""
		if slots:
			# Array read: the default substitutes per bad slot; alignment holds.
			emit([ln if slot_at(i) == shcl.Status.Good else dv for i, ln in enumerate(lines)])
		elif o.slots:
			print(f"{status.name}\t{dv}")
		else:
			print(dv)
		return 0
	if o.on_bad == "error":
		# The message already went to stderr above; error mode differs only in
		# printing nothing on stdout.
		return status_code(status)
	# flag: print the zero/empty value anyway; the exit code carries the status.
	emit(lines)
	return status_code(status)


def quoted(s):
	# The source text, quoted for a message: one line whatever it holds, with
	# the same escapes in every binding.
	out = ['"']
	for c in s:
		if c == '"':
			out.append('\\"')
		elif c == "\\":
			out.append("\\\\")
		elif c == "\n":
			out.append("\\n")
		elif c == "\r":
			out.append("\\r")
		elif c == "\t":
			out.append("\\t")
		elif ord(c) < 0x20 or c == "\x7f":
			out.append(f"\\u{{{ord(c):x}}}")
		else:
			out.append(c)
	out.append('"')
	return "".join(out)


def do_fmt(o):
	if len(o.args) != 1:
		sys.stderr.write("usage: shcl fmt [--write|-w] [options] FILE (see --help)\n")
		return 1
	file = o.args[0]
	if o.write and file == "-":
		sys.stderr.write("fmt --write cannot rewrite stdin; drop --write to print, or pass a FILE\n")
		return 1
	try:
		doc, code = load_layered(o, file)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return EXIT_IO
	if doc is None:
		return code
	if o.write:
		return write_back(doc, file, o)
	sys.stdout.write(doc.to_canonical())
	return 0


def _unescape_ops(s):
	# Decode an ops value: \n \t \\ only; other `\x` stays verbatim.
	out = []
	i = 0
	while i < len(s):
		c = s[i]
		if c != "\\" or i + 1 >= len(s):
			out.append(c)
			i += 1
			continue
		nxt = s[i + 1]
		if nxt == "n":
			out.append("\n")
		elif nxt == "t":
			out.append("\t")
		elif nxt == "\\":
			out.append("\\")
		else:
			out.append("\\")
			out.append(nxt)
		i += 2
	return "".join(out)


def _op_dt(s):
	dt = shcl.parse_datetime(s)
	if dt is None:
		raise ValueError(f"bad datetime: {s}")
	return dt


def _op_bool(s):
	if s == "true":
		return True
	if s == "false":
		return False
	raise ValueError(f"bad bool: {s}")


def _op_int(s):
	# Rust i64 FromStr grammar by hand: int() alone is too lax (it accepts
	# underscores, surrounding whitespace, and non-ASCII digits).
	t = s[1:] if s[:1] in ("+", "-") else s
	if t == "" or any(c < "0" or c > "9" for c in t):
		raise ValueError(f"bad int: {s}")
	# Length-gate before int(): CPython 3.11+ refuses >4300 decimal digits, but the
	# reference just overflows. Leading zeros are legal and don't count toward range.
	digits = t.lstrip("0") or "0"
	if len(digits) > 19:
		raise ValueError(f"bad int: {s}")
	v = -int(digits) if s[:1] == "-" else int(digits)
	if v < -(2 ** 63) or v > 2 ** 63 - 1:
		raise ValueError(f"bad int: {s}")
	return v


def _float_grammar_ok(s):
	# Rust f64 FromStr grammar: optional sign, then inf|infinity|nan (ASCII
	# case-insensitive) or digits['.'[digits]] / '.'digits, with an optional
	# e|E[sign]digits exponent. ASCII digits only, whole string must match.
	t = s[1:] if s[:1] in ("+", "-") else s
	low = "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in t)
	if low in ("inf", "infinity", "nan"):
		return True
	n = len(t)

	def digits(j):
		while j < n and "0" <= t[j] <= "9":
			j += 1
		return j

	j = digits(0)
	int_digits = j > 0
	frac_digits = False
	if j < n and t[j] == ".":
		k = digits(j + 1)
		frac_digits = k > j + 1
		j = k
	if not int_digits and not frac_digits:
		return False
	if j < n and t[j] in ("e", "E"):
		j += 1
		if j < n and t[j] in ("+", "-"):
			j += 1
		k = digits(j)
		if k == j:
			return False
		j = k
	return j == n


def _op_flt(s):
	# The language's own float reader takes inf and nan, and overflow (1e400)
	# lands on them too; the document's reader does not, so they are bad
	# values here, the way a bad datetime is.
	if not _float_grammar_ok(s):
		raise ValueError(f"bad float: {s}")
	x = float(s)
	if not math.isfinite(x):
		raise ValueError(f"bad float: {s}")
	return x


def apply_op(doc, line):
	f = line.split("\t")

	def get(i):
		return f[i] if i < len(f) else ""

	path, v = get(1), get(2)
	arr = f[2:] if len(f) > 2 else []
	op = f[0]
	if op == "int":
		wrote = doc.set_int(path, _op_int(v))
	elif op == "float":
		wrote = doc.set_float(path, _op_flt(v))
	elif op == "bool":
		wrote = doc.set_bool(path, _op_bool(v))
	elif op == "string":
		wrote = doc.set_string(path, _unescape_ops(v))
	elif op == "datetime":
		wrote = doc.set_datetime(path, _op_dt(v))
	elif op == "literal":
		wrote = doc.set_literal(path, v)
	elif op == "literal-default":
		wrote = doc.set_literal_default(path, v)
	elif op == "int-default":
		wrote = doc.set_int_default(path, _op_int(v))
	elif op == "float-default":
		wrote = doc.set_float_default(path, _op_flt(v))
	elif op == "bool-default":
		wrote = doc.set_bool_default(path, _op_bool(v))
	elif op == "string-default":
		wrote = doc.set_string_default(path, _unescape_ops(v))
	elif op == "datetime-default":
		wrote = doc.set_datetime_default(path, _op_dt(v))
	elif op == "int-array":
		wrote = doc.set_int_array(path, [_op_int(x) for x in arr])
	elif op == "float-array":
		wrote = doc.set_float_array(path, [_op_flt(x) for x in arr])
	elif op == "bool-array":
		wrote = doc.set_bool_array(path, [_op_bool(x) for x in arr])
	elif op == "string-array":
		wrote = doc.set_string_array(path, [_unescape_ops(x) for x in arr])
	elif op == "datetime-array":
		wrote = doc.set_datetime_array(path, [_op_dt(x) for x in arr])
	elif op == "int-array-default":
		wrote = doc.set_int_array_default(path, [_op_int(x) for x in arr])
	elif op == "float-array-default":
		wrote = doc.set_float_array_default(path, [_op_flt(x) for x in arr])
	elif op == "bool-array-default":
		wrote = doc.set_bool_array_default(path, [_op_bool(x) for x in arr])
	elif op == "string-array-default":
		wrote = doc.set_string_array_default(path, [_unescape_ops(x) for x in arr])
	elif op == "datetime-array-default":
		wrote = doc.set_datetime_array_default(path, [_op_dt(x) for x in arr])
	elif op == "raw":
		wrote = doc.set_raw(path, _unescape_ops(get(3)), v)
	elif op == "raw-default":
		wrote = doc.set_raw_default(path, _unescape_ops(get(3)), v)
	elif op == "empty":
		wrote = doc.set_empty(path)
	elif op == "comment":
		wrote = doc.set_comment(path, v)
	elif op == "remove":
		doc.remove(path)
		wrote = True
	else:
		raise ValueError(f"unknown op: {op}")
	if not wrote:
		raise ValueError(f"cannot write {path}: {describe_refusal(doc, path)}")


def do_set(o):
	if len(o.args) != 1:
		sys.stderr.write("usage: shcl set [--write|-w] [options] FILE (see --help)\n")
		return 1
	file = o.args[0]
	if o.write and file == "-":
		sys.stderr.write("set --write cannot rewrite stdin; drop --write to print, or pass a FILE\n")
		return 1
	# Base doc: with the edits given as options no ops script is read, so a '-'
	# file is the document on stdin the way it is everywhere else; only when
	# stdin is the ops script does '-' mean an empty base. Reading neither threw
	# a piped document away at exit 0.
	# Any --layer files sit under it and --set overrides sit on top, before ops.
	# --write names the file this command produces, so a FILE that is not there
	# yet is a create and the edits land in a new document. Only under --write,
	# and only when nothing is at the path at all: without --write there is
	# nothing to create, and a file that exists but cannot be read is still an
	# error rather than something to quietly write over.
	creating = o.write and file != "-" and not os.path.exists(file)
	try:
		layer_texts = [read_input(lf) for lf in o.layers]
		base = "" if creating or (file == "-" and not o.sets) else read_input(file)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return EXIT_IO
	layer_texts.append(base)
	doc, code = load_doc(layer_texts[0], o.strictness)
	if doc is None:
		return code
	diags = list(doc.diagnostics())
	for t in layer_texts[1:]:
		over, c = load_doc(t, o.strictness)
		if over is None:
			return c
		diags.extend(over.diagnostics())
		doc.merge(over)
	# The load's diagnostics belong to the load, so they go out before any edit
	# runs: a refused --set or a failing op used to return with nothing said.
	say_diagnostics(diags)
	for st in o.sets:
		if not st.apply(doc):
			sys.stderr.write(f"{st.opt()}: cannot write {st.path}: {describe_refusal(doc, st.path)}\n")
			return 1
	# --set carries the edits, so stdin is left alone: reading it here would
	# block on the console for anyone who passed edits as options.
	# The ops script is contract input like the reference's read_to_string:
	# bad bytes are a hard error, never silently replaced.
	ops = ""
	if not o.sets:
		# Say so before blocking. With nothing on stdin this used to sit there
		# silently, which reads as a hang rather than as a prompt; the note is
		# unconditional so a pipeline and a terminal behave identically. The
		# program-name prefix marks it as a notice; errors carry none.
		sys.stderr.write(
			"shcl: reading write-ops from stdin (one op per line, tab-separated; end with EOF)\n"
		)
		try:
			ops = sys.stdin.buffer.read().decode("utf-8")
		except UnicodeDecodeError:
			sys.stderr.write("stdin: invalid UTF-8\n")
			return EXIT_IO
	pieces = ops.split("\n")
	for n, line in enumerate(pieces):
		# The CR of a CRLF comes off with the LF, as the reference's line split
		# takes it; then one more, so a bare CR at EOF (the CR of a CRLF that
		# lost its LF) goes the same way.
		if n + 1 < len(pieces) and line.endswith("\r"):
			line = line[:-1]
		line = line[:-1] if line.endswith("\r") else line
		if line == "" or line.startswith("#"):
			continue
		try:
			apply_op(doc, line)
		except ValueError as e:
			sys.stderr.write(f"op line {n + 1}: {e}\n")
			return 1
	if o.write:
		return write_back(doc, file, o)
	sys.stdout.write(doc.to_canonical())
	return 0


def do_check(o):
	if len(o.args) != 1:
		sys.stderr.write("usage: shcl check [options] FILE (see --help)\n")
		return 1
	try:
		text = read_input(o.args[0])
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return EXIT_IO
	strict_failed = False
	try:
		doc = shcl.Document.parse_with(text, o.strictness)
		diags = list(doc.diagnostics())
		# --schema: append validation diagnostics under the same contract. The
		# schema itself always loads at Standard (a program artifact); one that
		# does not load cleanly is a single V099 schema fault.
		if o.schema is not None:
			try:
				stext = read_input(o.schema)
			except (OSError, ValueError) as e:
				sys.stderr.write(str(e) + "\n")
				return EXIT_IO
			sdoc = shcl.Document.parse(stext)
			if any(sd.severity == shcl.Severity.Error for sd in sdoc.diagnostics()):
				for sd in sdoc.diagnostics():
					sys.stderr.write(f"schema line {sd.line}: {sd.severity.name}: {sd.code} {sd.message}\n")
				diags.append(shcl.Diagnostic(0, shcl.Severity.Error, "schema failed to load", "V099"))
			else:
				diags.extend(doc.validate(sdoc))
				shcl.suppress_declared_repeats(sdoc, diags)
				shcl.suppress_declared_reopens(sdoc, diags)
	except shcl.LoadError as le:
		diags = le.diagnostics
		strict_failed = True
	# stdout carries the stable codes - the cross-binding contract. The prose is
	# per-binding voice and goes to stderr (which the differential check drops).
	# A V090-V093 line number is a SCHEMA line (the code table says so); the
	# prose names the file so the two number spaces cannot be confused.
	for d in diags:
		print(f"line {d.line}: {d.severity.name}: {d.code}")
	say_diagnostics(diags)
	errors = sum(1 for d in diags if d.severity == shcl.Severity.Error)
	if strict_failed:
		print(f"strict load failed: {len(diags)} diagnostic(s)")
		return 6
	if errors > 0:
		# Loaded, but lines were dropped: nonzero so a CI gate on check catches it.
		print(f"failed: {len(diags)} diagnostic(s), {errors} error(s)")
		return 6
	print(f"ok ({len(diags)} diagnostic(s))")
	return 0


def do_init(o):
	if o.args:
		sys.stderr.write("init takes no file argument (see --help)\n")
		return 1
	if o.schema is None:
		sys.stderr.write("init needs --schema=FILE (see --help)\n")
		return 1
	try:
		stext = read_input(o.schema)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return EXIT_IO
	# The schema always loads at Standard - a program artifact, not user data.
	sdoc = shcl.Document.parse(stext)
	if any(d.severity == shcl.Severity.Error for d in sdoc.diagnostics()):
		for d in sdoc.diagnostics():
			sys.stderr.write(f"schema line {d.line}: {d.severity.name}: {d.code} {d.message}\n")
		sys.stderr.write("init: schema failed to load\n")
		# A broken schema is a config-semantics failure, not a usage error:
		# same exit as `check --schema` reporting it.
		return 6
	text, faults = shcl.generate(sdoc, o.no_banner)
	if faults:
		for d in faults:
			sys.stderr.write(f"schema line {d.line}: {d.severity.name}: {d.code} {d.message}\n")
		sys.stderr.write("init: schema has faults\n")
		return 6
	sys.stdout.write(text)
	return 0


def do_enum(o, want_count):
	if len(o.args) != 2:
		name = "count" if want_count else "instances"
		sys.stderr.write(f"usage: shcl {name} [options] FILE PATH (see --help)\n")
		return 1
	file, path = o.args[0], o.args[1]
	try:
		doc, code = load_layered(o, file)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return EXIT_IO
	if doc is None:
		return code
	if want_count:
		print(doc.count(path))
	else:
		for v in doc.instances(path):
			print(v)
	return 0


def do_children(o):
	# Child field names under a path, one per line, in file order and with
	# duplicates kept. PATH may be left out to enumerate the top level. Each name
	# comes out in the form a path accepts, so one holding a dot or a quote
	# splices back into a path with no further work.
	if len(o.args) == 1:
		file, path = o.args[0], ""
	elif len(o.args) == 2:
		file, path = o.args[0], o.args[1]
	else:
		sys.stderr.write("usage: shcl children [options] FILE [PATH] (see --help)\n")
		return 1
	try:
		doc, code = load_layered(o, file)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return EXIT_IO
	if doc is None:
		return code
	for name in doc.children(path):
		print(shcl.quote_segment(name))
	return 0


def do_paths(o):
	# Every field path in the document, one per line, in file order and
	# deduplicated - the whole-document counterpart of do_children.
	if len(o.args) != 1:
		sys.stderr.write("usage: shcl paths [options] FILE (see --help)\n")
		return 1
	try:
		doc, code = load_layered(o, o.args[0])
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return EXIT_IO
	if doc is None:
		return code
	for p in doc.paths():
		print(p)
	return 0


COMMANDS = ("get", "set", "fmt", "check", "init", "count", "instances", "children", "paths")


def run(argv):
	# Undecodable argv bytes arrive as surrogate-escaped chars; reject like the
	# reference (exit 1) instead of feeding a garbled path or query downstream.
	for a in argv:
		try:
			a.encode("utf-8")
		except UnicodeEncodeError:
			sys.stderr.write("invalid argument encoding (expected UTF-8)\n")
			return 1
	asked = asked_for(argv)
	# One convention: asking for the help - by name, by flag, or by asking for
	# nothing at all - prints it and succeeds. The blank lines separate the
	# block from the surrounding prompts. A bare run used to print the same
	# text unpadded and exit 1, which read as neither a help nor an error.
	if not argv:
		sys.stdout.write("\n" + HELP + "\n")
		return 0
	if asked == "help" or argv[0] == "help":
		sys.stdout.write("\n" + HELP + "\n")
		return 0
	if asked == "version" or argv[0] == "version":
		print(f"shcl {VERSION}")
		return 0
	if asked == "about" or argv[0] == "about":
		sys.stdout.write("\n" + ABOUT + "\n")
		return 0
	if asked == "donate" or argv[0] == "donate":
		sys.stdout.write("\n" + DONATE + "\n")
		return 0
	cmd = argv[0]
	if cmd not in COMMANDS:
		# Before the options are judged, so a typo in the command is reported
		# as that and not as an option the wrong command cannot take.
		if cmd.startswith("-") and cmd != "--":
			sys.stderr.write(f"unknown option: {cmd} (see --help)\n")
		else:
			sys.stderr.write(f"unknown command: {cmd} (see --help)\n")
		return 1
	try:
		o = parse_opts(argv[1:])
	except ValueError as e:
		sys.stderr.write(str(e) + "\n")
		return 1
	code = check_opts(cmd, o)
	if code is not None:
		return code
	if cmd == "get":
		return do_get(o)
	if cmd == "set":
		return do_set(o)
	if cmd == "fmt":
		return do_fmt(o)
	if cmd == "check":
		return do_check(o)
	if cmd == "init":
		return do_init(o)
	if cmd == "count":
		return do_enum(o, True)
	if cmd == "instances":
		return do_enum(o, False)
	if cmd == "children":
		return do_children(o)
	if cmd == "paths":
		return do_paths(o)
	# A refusal rather than a fall-through: with one, adding a name to COMMANDS
	# without adding a branch here quietly ran whichever command the last line
	# named, with no message. run() gates on COMMANDS first, so this is only
	# reachable through that mistake.
	sys.stderr.write(f"{cmd}: no dispatch arm (see --help)\n")
	return 1


def main():
	# Restore the default SIGPIPE disposition: Python installs SIG_IGN, which turns
	# a closed stdout into a BrokenPipeError instead of the conventional signal
	# death (exit 141). With SIG_DFL a broken pipe kills us like head/cat, matching
	# the other bindings; no BrokenPipeError to catch.
	if hasattr(signal, "SIGPIPE"):
		signal.signal(signal.SIGPIPE, signal.SIG_DFL)
	# A standard stream that was closed before the start (fmt - <&-, fmt FILE
	# >&-) is None here. The reference reads such a stdin as empty and drops
	# what it writes to such a stdout or stderr, so each gets the same: an
	# empty document in, a sink for the output. The stdin one is a text stream
	# so the .buffer reads below find one.
	if sys.stdin is None:
		sys.stdin = open(os.devnull, encoding="utf-8")  # noqa: SIM115
	if sys.stdout is None:
		sys.stdout = open(os.devnull, "w", encoding="utf-8")  # noqa: SIM115
	if sys.stderr is None:
		sys.stderr = open(os.devnull, "w", encoding="utf-8")  # noqa: SIM115
	# Output is UTF-8 and LF on every platform, as the reference writes it:
	# the text streams take the locale's encoding, and translate \n to \r\n on
	# windows, unless told not to. stdin is read through .buffer everywhere,
	# so it never decodes or translates. A stream that cannot be reconfigured
	# (replaced, or not a text stream) is left alone.
	for stream in (sys.stdout, sys.stderr):
		reconfigure = getattr(stream, "reconfigure", None)
		if reconfigure is not None:
			try:
				reconfigure(encoding="utf-8", newline="\n")
			except (ValueError, OSError):
				pass
	sys.stderr = _BestEffort(sys.stderr)
	try:
		code = run(sys.argv[1:])
		# A tail still sitting in the buffer when the work is done fails the
		# same way a write does.
		sys.stdout.flush()
	except OSError as e:
		return write_failed(e)
	return code


if __name__ == "__main__":
	sys.exit(main())
