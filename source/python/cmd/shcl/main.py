#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Jim Collier

# shcl CLI - the Python binding's command surface. Flags, output, and exit codes
# mirror the Rust reference exactly; the cicd cross-binding check compares them
# byte for byte, so any drift here fails the pipeline.

import os
import signal
import stat
import sys

# The single-file library sits two directories up (lib in source/python/, CLI in
# source/python/cmd/shcl/, mirroring the Go layout).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__)))))
import shcl  # noqa: E402

# Keep in step with source/rust/Cargo.toml, the canonical version source.
VERSION = "1.1.0"

HELP = """shcl - Simple Hierarchical Config Language (reference CLI)

Usage:
  shcl get [type] [options] FILE PATH    read one value (or array) at a path
  shcl set [--write|-w] [options] FILE   apply edits (--set, or ops on stdin);
                                         print canonical (or rewrite FILE in
                                         place with --write)
  shcl fmt [--write|-w] FILE             print (or rewrite in place) the canonical form
  shcl check [options] FILE              load and print diagnostics
                                         (--schema=SCHEMA also validates FILE
                                         against a schema, itself a .shcl file)
  shcl init [--no-banner] --schema=S     print a commented starter config from
                                         a schema (required fields live, optional
                                         commented, wildcards noted)
  shcl count [options] FILE PATH         number of instances at a path
  shcl instances [options] FILE PATH     instance values at a path, one per line
  shcl help | version                    this help, or the version (also -h/--help, -V/--version)

set edits FILE, the base document ('-' = empty base). Values go in as
repeatable --set PATH=VALUE (data) or --set-literal PATH=TEXT (value syntax, so
arrays work) options, which persist with --write; given either, no ops are read
from stdin. Raw blocks, set-only-if-absent and removal go in as a write-ops
script on stdin, one op per line, tab-separated. Ops:
  int|float|bool|string|datetime<TAB>PATH<TAB>VALUE       set a scalar
  <type>-array<TAB>PATH<TAB>V1<TAB>V2...                  set an inline array
  <type>[-array]-default<TAB>...                          set only if absent
  literal[-default]<TAB>PATH<TAB>TEXT                     set from value syntax
  raw<TAB>PATH<TAB>INFO<TAB>CONTENT                       set a raw block
  empty<TAB>PATH   comment<TAB>PATH<TAB>TEXT   remove<TAB>PATH
string/raw values decode \\n \\t \\\\; a line starting with # is a script comment.

Types (default --string):
  --int --float --bool --datetime --string --raw --rawinfo
  --array                                read the value as an array of the type
  --rawinfo reads a raw block's info-string (the fence tag), not its content

Options:
  --default=VALUE                        value to print when the read is not Good
                                         (implies --on-bad=default; for arrays,
                                         substituted per bad slot)
  --on-bad=error|default|flag            error: fail loudly; default: print the
                                         default; flag: print the value anyway and
                                         report via exit code (the default mode)
  --slots                                prefix each line with its slot status and
                                         a tab (per element, or per wildcard slot)
  --no-banner                            (init) leave out the footer naming the
                                         format and pointing at its spec
  --strictness=loose|standard|strict     or 1|2|3 (default standard)
  --schema=SCHEMA                        (check/init) validate FILE against a
                                         schema; adds V### diagnostics
  --layer=FILE                           (get/fmt/count/instances/set) merge a
                                         lower-priority layer under FILE;
                                         repeatable, earlier = lower priority
  --set=PATH=VALUE                       override one path as the top layer,
                                         after all files; repeatable. On 'set'
                                         it is an edit to the document itself,
                                         so it persists with --write. VALUE
                                         goes in as data: its type still
                                         follows the text (8 is an int), but a
                                         comma or quote in it is content, not
                                         syntax
  --set-literal=PATH=TEXT                as --set, except TEXT goes in as value
                                         syntax the way a file spells it, so
                                         'ports=80, 443' writes a two-element
                                         array. An unquoted # ends the value;
                                         text spanning lines is rejected

Value options accept either spelling: --default=VALUE or --default VALUE. In
the space form the next argument is taken as the value whatever it looks like,
so --default --int reads --int as the default. Use -- to end the options when a
FILE or PATH begins with a dash.
An option a subcommand does not use is a usage error, not ignored.
FILE may be '-' for stdin. With --layer, FILE is the highest file layer and
each --layer is merged under it in order; --set applies last. 'fmt' with
layers prints the merged canonical document.

Exit codes: 0 good, 1 usage or I/O error, 2 empty, 3 not found, 4 bad type,
5 multiple instances, 6 check failed or strict load failure.
"""


def status_code(st):
	return st.value


class _SetOpt:
	# One --set/--set-literal override. Both spellings share a list so they apply
	# in the order given, which is what decides the winner when two target the
	# same path.
	__slots__ = ("path", "value", "literal")

	def __init__(self, path, value, literal):
		self.path = path
		self.value = value
		self.literal = literal

	def apply(self, doc):
		if self.literal:
			return doc.set_literal(self.path, self.value)
		return doc.set_string(self.path, self.value)

	def opt(self):
		return "--set-literal" if self.literal else "--set"


class _Opts:
	__slots__ = ("kind", "array", "slots", "default", "on_bad", "strictness", "write", "no_banner", "schema", "layers", "sets", "args", "seen")

	def __init__(self):
		self.kind = "string"     # int|float|bool|datetime|string|raw
		self.array = False
		self.slots = False
		self.default = None
		self.on_bad = "flag"     # error|default|flag
		self.strictness = shcl.Strictness.Standard
		self.schema = None
		self.write = False
		self.no_banner = False
		self.layers = []         # lower-priority layers, in listed order
		self.sets = []           # final override layer: _SetOpt, in the order given
		self.args = []           # positional: FILE [PATH]
		self.seen = []           # canonical names of options given, for per-command validation


def _set_value_opt(o, name, v):
	if name == "--default":
		o.default = v
		o.on_bad = "default"
		o.seen.append("--default")
	elif name == "--on-bad":
		if v not in ("error", "default", "flag"):
			raise ValueError("bad --on-bad value: {}".format(v))
		o.on_bad = v
		o.seen.append("--on-bad")
	elif name == "--strictness":
		s = shcl.Strictness.from_arg(v)
		if s is None:
			raise ValueError("bad --strictness value: {}".format(v))
		o.strictness = s
		o.seen.append("--strictness")
	elif name == "--schema":
		o.schema = v
		o.seen.append("--schema")
	elif name == "--layer":
		o.layers.append(v)
		o.seen.append("--layer")
	elif name in ("--set", "--set-literal"):
		eq = v.find("=")
		if eq < 0:
			raise ValueError("bad {} value (want PATH=VALUE): {}".format(name, v))
		o.sets.append(_SetOpt(v[:eq], v[eq + 1:], name == "--set-literal"))
		o.seen.append(name)


def asked_for(argv):
	# Did the command line ask for help or the version? Only tokens in option
	# position count: a value that happens to read `-h`, and anything after the
	# file, are data. Scanning the whole line for them let a read of a missing
	# path answer with the help text and exit 0.
	i = 0
	while i < len(argv):
		a = argv[i]
		if a in ("-h", "--help"):
			return "help"
		if a in ("-V", "--version"):
			return "version"
		if a == "--":
			return None
		if a in ("--default", "--on-bad", "--strictness", "--schema", "--layer", "--set", "--set-literal"):
			i += 1
		elif a.startswith("-") and len(a) > 1:
			pass
		elif i > 0:
			# The subcommand, then the file: past that everything is a path.
			return None
		i += 1
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
		if a in ("--int", "--float", "--bool", "--datetime", "--string", "--raw", "--rawinfo"):
			o.kind = a[2:]
			o.seen.append("--<type>")
		elif a == "--array":
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
		elif a in ("--default", "--on-bad", "--strictness", "--schema", "--layer", "--set", "--set-literal"):
			i += 1
			if i >= len(argv):
				raise ValueError("missing value for {0} (try {0}=VALUE)".format(a))
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
		elif a.startswith("--set-literal="):
			_set_value_opt(o, "--set-literal", a[len("--set-literal="):])
		elif a.startswith("--set="):
			_set_value_opt(o, "--set", a[len("--set="):])
		elif a.startswith("-") and len(a) > 1:
			raise ValueError("unknown option: {}".format(a))
		else:
			o.args.append(a)
		i += 1
	return o


def read_input(file):
	if file == "-":
		data = sys.stdin.buffer.read()
	else:
		with open(file, "rb") as f:
			data = f.read()
	# The reference reads as UTF-8 and fails on bad bytes; match its exit path.
	try:
		return data.decode("utf-8")
	except UnicodeDecodeError:
		raise ValueError("{}: stream did not contain valid UTF-8".format(file))


def load_doc(text, strictness):
	# Returns (doc, None) or (None, code). On strict load failure, prints the
	# reference's diagnostic lines to stderr and reports code 6.
	try:
		return shcl.Document.parse_with(text, strictness), None
	except shcl.LoadError as le:
		for d in le.diagnostics:
			sys.stderr.write("line {}: {}: {}\n".format(d.line, d.severity.name, d.message))
		sys.stderr.write(str(le) + "\n")
		return None, 6


def load_layered(o, file):
	# Load file with o's lower-priority --layer files underneath and its --set
	# overrides on top - the layered-load fold. Every layer parses at the
	# requested strictness; a strict-load failure on any layer aborts like a
	# single-file strict failure. Returns (doc, None) or (None, code).
	texts = []
	for lf in o.layers:
		texts.append(read_input(lf))
	texts.append(read_input(file))
	doc, code = load_doc(texts[0], o.strictness)
	if doc is None:
		return None, code
	for t in texts[1:]:
		over, c = load_doc(t, o.strictness)
		if over is None:
			return None, c
		doc.merge(over)
	for st in o.sets:
		if not st.apply(doc):
			sys.stderr.write("shcl: cannot write {} (from {})\n".format(st.path, st.opt()))
			return None, 1
	return doc, None


def check_opts(cmd, o):
	# Every option must be meaningful for its subcommand; an option that would be
	# silently ignored (`set --write` before it existed, `--schema` on `get`) is a
	# usage error instead. Returns an exit code, or None to proceed.
	if cmd == "get":
		allowed = ("--<type>", "--array", "--slots", "--default", "--on-bad", "--strictness", "--layer", "--set", "--set-literal")
	elif cmd == "set":
		allowed = ("--strictness", "--layer", "--set", "--set-literal", "--write")
	elif cmd == "fmt":
		allowed = ("--write", "--strictness", "--layer", "--set", "--set-literal")
	elif cmd == "check":
		allowed = ("--strictness", "--schema")
	elif cmd == "init":
		allowed = ("--schema", "--no-banner")
	elif cmd in ("count", "instances"):
		allowed = ("--strictness", "--layer", "--set", "--set-literal")
	else:
		allowed = ()
	for s in o.seen:
		if s not in allowed:
			if s == "--<type>":
				sys.stderr.write("type options are not valid for {} (see --help)\n".format(cmd))
			else:
				sys.stderr.write("option {} not valid for {} (see --help)\n".format(s, cmd))
			return 1
	# Writing back the merged document would fold the lower layers permanently
	# into the top file, which is the opposite of what layering is for. On 'set'
	# the --set values are edits to the document rather than a layer over it, so
	# persisting them is the whole point; everywhere else they stay ephemeral.
	if o.write and o.layers:
		sys.stderr.write("--write cannot be combined with --layer (see --help)\n")
		return 1
	if o.write and o.sets and cmd != "set":
		sys.stderr.write("--write cannot be combined with --set (see --help)\n")
		return 1
	# The ops script already has stdin, so a layer cannot read it too.
	if cmd == "set" and any(lf == "-" for lf in o.layers):
		sys.stderr.write("--layer=- is not valid for set (stdin carries the ops script)\n")
		return 1
	return None


def write_atomic(file, data):
	# Write atomically: temp file in the same dir, then rename over the target,
	# so an interrupted write can never truncate the config it rewrites. The data
	# is synced before the rename so a crash cannot publish an empty file.
	# Returns None on success, or the error message to report.
	#
	# A rename publishes a new inode, so the target is resolved through symlinks
	# first (otherwise a linked-in config gets replaced by a regular file and the
	# real one is left stale) and the original's mode is copied onto the temp file
	# (otherwise a 600 config comes back at whatever the umask allows). Other hard
	# links to the old inode cannot survive a rename and keep the old content.
	target = os.path.realpath(file)
	d = os.path.dirname(target)
	if d == "":
		d = "."
	base = os.path.basename(target)
	if base == "":
		base = target
	# Exclusive create: the name is predictable, so anything already sitting
	# there - including a symlink someone else planted - must make this fail
	# rather than be written through. Retry past a stale collision, then give
	# up; refusing to write beats writing somewhere unintended.
	f = None
	tmp = ""
	last = ""
	for attempt in range(8):
		tmp = os.path.join(d, ".{}.tmp{}.{}".format(base, os.getpid(), attempt))
		try:
			# Born private, so the copy is never briefly readable to anyone the
			# original was not. The real mode goes on below, before any data.
			fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
			f = os.fdopen(fd, "w", encoding="utf-8", newline="")
			break
		except OSError as e:
			last = str(e)
	if f is None:
		return "{}: cannot create temporary file: {}".format(file, last)
	try:
		try:
			# On the handle, so umask cannot narrow it the way it narrows a
			# create mode. Best effort: a filesystem that cannot carry the mode
			# is not a reason to fail a write that otherwise succeeded.
			try:
				os.fchmod(f.fileno(), stat.S_IMODE(os.stat(target).st_mode))
			except OSError:
				pass
			f.write(data)
			f.flush()
			os.fsync(f.fileno())
		finally:
			f.close()
	except OSError as e:
		try:
			os.remove(tmp)
		except OSError:
			pass
		return "{}: {}".format(file, e)
	try:
		os.replace(tmp, target)
	except OSError as e:
		try:
			os.remove(tmp)
		except OSError:
			pass
		return "{}: {}".format(file, e)
	return None


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
		sys.stderr.write("get needs FILE and PATH (see --help)\n")
		return 1
	file, path = o.args[0], o.args[1]
	try:
		doc, code = load_layered(o, file)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return 1
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
			sys.stderr.write("--{} has no --array form\n".format(o.kind))
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
				print("{}\t{}".format(slot_at(i).name, ln))
			else:
				print(ln)

	if status == shcl.Status.Good or (status == shcl.Status.Empty and o.on_bad == "flag"):
		emit(lines)
		return status_code(status)
	if o.on_bad == "default":
		dv = o.default if o.default is not None else ""
		if slots:
			# Array read: the default substitutes per bad slot; alignment holds.
			emit([ln if slot_at(i) == shcl.Status.Good else dv for i, ln in enumerate(lines)])
		elif o.slots:
			print("{}\t{}".format(status.name, dv))
		else:
			print(dv)
		return 0
	if o.on_bad == "error":
		type_name = "{} array".format(o.kind) if o.array else o.kind
		if status == shcl.Status.BadType:
			raw = doc.read_string(path).raw
			reason = (
				'value "{}" is not a valid {}'.format(raw, type_name)
				if raw is not None
				else "value is not a valid {}".format(type_name)
			)
		elif status == shcl.Status.NotFound:
			reason = "no value at that path"
		elif status == shcl.Status.Empty:
			reason = "the value is empty"
		else:
			reason = "the path matches multiple instances"
		sys.stderr.write(
			"shcl: cannot read {} as {}: {} (in {})\n".format(path, type_name, reason, file)
		)
		return status_code(status)
	# flag: print the zero/empty value anyway; the exit code carries the status.
	emit(lines)
	return status_code(status)


def do_fmt(o):
	if len(o.args) != 1:
		sys.stderr.write("fmt needs FILE (see --help)\n")
		return 1
	file = o.args[0]
	if o.write and file == "-":
		sys.stderr.write("fmt --write cannot rewrite stdin; drop --write to print, or pass a FILE\n")
		return 1
	try:
		doc, code = load_layered(o, file)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return 1
	if doc is None:
		return code
	canonical = doc.to_canonical()
	if o.write:
		err = write_atomic(file, canonical)
		if err is not None:
			sys.stderr.write(err + "\n")
			return 1
	else:
		sys.stdout.write(canonical)
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
		raise ValueError("bad datetime: {}".format(s))
	return dt


def _op_int(s):
	# Rust i64 FromStr grammar by hand: int() alone is too lax (it accepts
	# underscores, surrounding whitespace, and non-ASCII digits).
	t = s[1:] if s[:1] in ("+", "-") else s
	if t == "" or any(c < "0" or c > "9" for c in t):
		raise ValueError("bad int: {}".format(s))
	# Length-gate before int(): CPython 3.11+ refuses >4300 decimal digits, but the
	# reference just overflows. Leading zeros are legal and don't count toward range.
	digits = t.lstrip("0") or "0"
	if len(digits) > 19:
		raise ValueError("bad int: {}".format(s))
	v = -int(digits) if s[:1] == "-" else int(digits)
	if v < -(2 ** 63) or v > 2 ** 63 - 1:
		raise ValueError("bad int: {}".format(s))
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
	# float() after the grammar gate is safe; overflow (1e400) yields inf,
	# matching Rust's parse.
	if not _float_grammar_ok(s):
		raise ValueError("bad float: {}".format(s))
	return float(s)


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
		wrote = doc.set_bool(path, v == "true")
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
		wrote = doc.set_bool_default(path, v == "true")
	elif op == "string-default":
		wrote = doc.set_string_default(path, _unescape_ops(v))
	elif op == "datetime-default":
		wrote = doc.set_datetime_default(path, _op_dt(v))
	elif op == "int-array":
		wrote = doc.set_int_array(path, [_op_int(x) for x in arr])
	elif op == "float-array":
		wrote = doc.set_float_array(path, [_op_flt(x) for x in arr])
	elif op == "bool-array":
		wrote = doc.set_bool_array(path, [x == "true" for x in arr])
	elif op == "string-array":
		wrote = doc.set_string_array(path, [_unescape_ops(x) for x in arr])
	elif op == "datetime-array":
		wrote = doc.set_datetime_array(path, [_op_dt(x) for x in arr])
	elif op == "int-array-default":
		wrote = doc.set_int_array_default(path, [_op_int(x) for x in arr])
	elif op == "float-array-default":
		wrote = doc.set_float_array_default(path, [_op_flt(x) for x in arr])
	elif op == "bool-array-default":
		wrote = doc.set_bool_array_default(path, [x == "true" for x in arr])
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
		raise ValueError("unknown op: {}".format(op))
	if not wrote:
		raise ValueError("cannot write {}".format(path))


def do_set(o):
	if len(o.args) != 1:
		sys.stderr.write("set needs FILE (ops on stdin; see --help)\n")
		return 1
	file = o.args[0]
	if o.write and file == "-":
		sys.stderr.write("set --write cannot rewrite stdin; drop --write to print, or pass a FILE\n")
		return 1
	# Base doc: '-' means an empty base, since stdin carries the ops script.
	# Any --layer files sit under it and --set overrides sit on top, before ops.
	try:
		layer_texts = [read_input(lf) for lf in o.layers]
		base = "" if file == "-" else read_input(file)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return 1
	layer_texts.append(base)
	doc, code = load_doc(layer_texts[0], o.strictness)
	if doc is None:
		return code
	for t in layer_texts[1:]:
		over, c = load_doc(t, o.strictness)
		if over is None:
			return c
		doc.merge(over)
	for st in o.sets:
		if not st.apply(doc):
			sys.stderr.write("shcl: cannot write {} (from {})\n".format(st.path, st.opt()))
			return 1
	# --set carries the edits, so stdin is left alone: reading it here would
	# block on the console for anyone who passed edits as options.
	# The ops script is contract input like the reference's read_to_string:
	# bad bytes are a hard error, never silently replaced.
	ops = ""
	if not o.sets:
		try:
			ops = sys.stdin.buffer.read().decode("utf-8")
		except UnicodeDecodeError:
			sys.stderr.write("stdin: invalid UTF-8\n")
			return 1
	for n, line in enumerate(ops.split("\n")):
		line = line[:-1] if line.endswith("\r") else line
		if line == "" or line.startswith("#"):
			continue
		try:
			apply_op(doc, line)
		except ValueError as e:
			sys.stderr.write("op line {}: {}\n".format(n + 1, e))
			return 1
	canonical = doc.to_canonical()
	if o.write:
		err = write_atomic(file, canonical)
		if err is not None:
			sys.stderr.write(err + "\n")
			return 1
	else:
		sys.stdout.write(canonical)
	return 0


def do_check(o):
	if len(o.args) != 1:
		sys.stderr.write("check needs FILE (see --help)\n")
		return 1
	try:
		text = read_input(o.args[0])
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return 1
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
				return 1
			sdoc = shcl.Document.parse(stext)
			if any(sd.severity == shcl.Severity.Error for sd in sdoc.diagnostics()):
				for sd in sdoc.diagnostics():
					sys.stderr.write("schema line {}: {}: {}\n".format(sd.line, sd.severity.name, sd.message))
				diags.append(shcl.Diagnostic(0, shcl.Severity.Error, "schema failed to load", "V099"))
			else:
				diags.extend(doc.validate(sdoc))
				shcl.suppress_declared_repeats(sdoc, diags)
	except shcl.LoadError as le:
		diags = le.diagnostics
		strict_failed = True
	# stdout carries the stable codes - the cross-binding contract. The prose is
	# per-binding voice and goes to stderr (which the differential check drops).
	# A V090-V093 line number is a SCHEMA line (the code table says so); the
	# prose names the file so the two number spaces cannot be confused.
	errors = 0
	for d in diags:
		print("line {}: {}: {}".format(d.line, d.severity.name, d.code))
		space = "schema line" if d.code.startswith("V09") and d.code != "V099" else "line"
		sys.stderr.write("{} {}: {}: {}\n".format(space, d.line, d.severity.name, d.message))
		if d.severity == shcl.Severity.Error:
			errors += 1
	if strict_failed:
		print("strict load failed: {} diagnostic(s)".format(len(diags)))
		return 6
	if errors > 0:
		# Loaded, but lines were dropped: nonzero so a CI gate on check catches it.
		print("failed: {} diagnostic(s), {} error(s)".format(len(diags), errors))
		return 6
	print("ok ({} diagnostic(s))".format(len(diags)))
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
		return 1
	# The schema always loads at Standard - a program artifact, not user data.
	sdoc = shcl.Document.parse(stext)
	if any(d.severity == shcl.Severity.Error for d in sdoc.diagnostics()):
		for d in sdoc.diagnostics():
			sys.stderr.write("schema line {}: {}: {}\n".format(d.line, d.severity.name, d.message))
		sys.stderr.write("init: schema failed to load\n")
		# A broken schema is a config-semantics failure, not a usage error:
		# same exit as `check --schema` reporting it.
		return 6
	text, faults = shcl.generate(sdoc, o.no_banner)
	if faults:
		for d in faults:
			sys.stderr.write("schema line {}: {}: {}\n".format(d.line, d.severity.name, d.message))
		sys.stderr.write("init: schema has faults\n")
		return 6
	sys.stdout.write(text)
	return 0


def do_enum(o, want_count):
	if len(o.args) != 2:
		sys.stderr.write("count/instances need FILE and PATH (see --help)\n")
		return 1
	file, path = o.args[0], o.args[1]
	try:
		doc, code = load_layered(o, file)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return 1
	if doc is None:
		return code
	if want_count:
		print(doc.count(path))
	else:
		for v in doc.instances(path):
			print(v)
	return 0


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
	if not argv or asked == "help" or argv[0] == "help":
		sys.stdout.write(HELP)
		return 1 if not argv else 0
	if asked == "version" or argv[0] == "version":
		print("shcl {}".format(VERSION))
		return 0
	try:
		o = parse_opts(argv[1:])
	except ValueError as e:
		sys.stderr.write(str(e) + "\n")
		return 1
	cmd = argv[0]
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
	sys.stderr.write("unknown command: {} (see --help)\n".format(cmd))
	return 1


def main():
	# Restore the default SIGPIPE disposition: Python installs SIG_IGN, which turns
	# a closed stdout into a BrokenPipeError instead of the conventional signal
	# death (exit 141). With SIG_DFL a broken pipe kills us like head/cat, matching
	# the other bindings; no BrokenPipeError to catch.
	if hasattr(signal, "SIGPIPE"):
		signal.signal(signal.SIGPIPE, signal.SIG_DFL)
	return run(sys.argv[1:])


if __name__ == "__main__":
	sys.exit(main())
