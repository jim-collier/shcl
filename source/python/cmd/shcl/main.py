#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Jim Collier

# shcl CLI - the Python binding's command surface. Flags, output, and exit codes
# mirror the Rust reference exactly; the cicd cross-binding check compares them
# byte for byte, so any drift here fails the pipeline.

import os
import signal
import sys

# The single-file library sits two directories up (lib in source/python/, CLI in
# source/python/cmd/shcl/, mirroring the Go layout).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__)))))
import shcl  # noqa: E402

# Keep in step with source/rust/Cargo.toml, the canonical version source.
VERSION = "1.0.0-beta2"

HELP = """shcl - Simple Hierarchical Config Language (reference CLI)

Usage:
  shcl get [type] [options] FILE PATH    read one value (or array) at a path
  shcl set [options] FILE                apply write-ops (stdin) and print canonical
  shcl fmt [--write|-w] FILE             print (or rewrite in place) the canonical form
  shcl check [options] FILE              load and print diagnostics
                                         (--schema=SCHEMA also validates FILE
                                         against a schema, itself a .shcl file)
  shcl count [options] FILE PATH         number of instances at a path
  shcl instances [options] FILE PATH     instance values at a path, one per line
  shcl help | version                    this help, or the version (also -h/--help, -V/--version)

set reads a write-ops script from stdin (one op per line, tab-separated) and
prints the canonical document. FILE is the base ('-' = empty base). Ops:
  int|float|bool|string|datetime<TAB>PATH<TAB>VALUE       set a scalar
  <type>-array<TAB>PATH<TAB>V1<TAB>V2...                  set an inline array
  <type>[-array]-default<TAB>...                          set only if absent
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
  --strictness=loose|standard|strict     or 1|2|3 (default standard)
  --schema=SCHEMA                        (check only) validate FILE against a
                                         schema; adds V### diagnostics

Value options accept either spelling: --default=VALUE or --default VALUE.
FILE may be '-' for stdin.

Exit codes: 0 good, 1 usage or I/O error, 2 empty, 3 not found, 4 bad type,
5 multiple instances, 6 strict load failure.
"""


def status_code(st):
	return st.value


class _Opts:
	__slots__ = ("kind", "array", "slots", "default", "on_bad", "strictness", "write", "schema", "args")

	def __init__(self):
		self.kind = "string"     # int|float|bool|datetime|string|raw
		self.array = False
		self.slots = False
		self.default = None
		self.on_bad = "flag"     # error|default|flag
		self.strictness = shcl.Strictness.Standard
		self.schema = None
		self.write = False
		self.args = []           # positional: FILE [PATH]


def _set_value_opt(o, name, v):
	if name == "--default":
		o.default = v
		o.on_bad = "default"
	elif name == "--on-bad":
		if v not in ("error", "default", "flag"):
			raise ValueError("bad --on-bad value: {}".format(v))
		o.on_bad = v
	elif name == "--strictness":
		s = shcl.Strictness.from_arg(v)
		if s is None:
			raise ValueError("bad --strictness value: {}".format(v))
		o.strictness = s
	elif name == "--schema":
		o.schema = v


def parse_opts(argv):
	o = _Opts()
	# Value-taking options accept both --opt=VALUE and the space form --opt VALUE.
	i = 0
	while i < len(argv):
		a = argv[i]
		if a in ("--int", "--float", "--bool", "--datetime", "--string", "--raw", "--rawinfo"):
			o.kind = a[2:]
		elif a == "--array":
			o.array = True
		elif a == "--slots":
			o.slots = True
		elif a in ("--write", "-w"):
			o.write = True
		elif a in ("--default", "--on-bad", "--strictness", "--schema"):
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
		text = read_input(file)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return 1
	doc, code = load_doc(text, o.strictness)
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
		text = read_input(file)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return 1
	doc, code = load_doc(text, o.strictness)
	if doc is None:
		return code
	canonical = doc.to_canonical()
	if o.write:
		try:
			with open(file, "w", encoding="utf-8", newline="") as f:
				f.write(canonical)
		except OSError as e:
			sys.stderr.write("{}: {}\n".format(file, e))
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


def apply_op(doc, line):
	f = line.split("\t")

	def get(i):
		return f[i] if i < len(f) else ""

	path, v = get(1), get(2)
	arr = f[2:] if len(f) > 2 else []
	op = f[0]
	if op == "int":
		doc.set_int(path, int(v))
	elif op == "float":
		doc.set_float(path, float(v))
	elif op == "bool":
		doc.set_bool(path, v == "true")
	elif op == "string":
		doc.set_string(path, _unescape_ops(v))
	elif op == "datetime":
		doc.set_datetime(path, _op_dt(v))
	elif op == "int-default":
		doc.set_int_default(path, int(v))
	elif op == "float-default":
		doc.set_float_default(path, float(v))
	elif op == "bool-default":
		doc.set_bool_default(path, v == "true")
	elif op == "string-default":
		doc.set_string_default(path, _unescape_ops(v))
	elif op == "datetime-default":
		doc.set_datetime_default(path, _op_dt(v))
	elif op == "int-array":
		doc.set_int_array(path, [int(x) for x in arr])
	elif op == "float-array":
		doc.set_float_array(path, [float(x) for x in arr])
	elif op == "bool-array":
		doc.set_bool_array(path, [x == "true" for x in arr])
	elif op == "string-array":
		doc.set_string_array(path, [_unescape_ops(x) for x in arr])
	elif op == "datetime-array":
		doc.set_datetime_array(path, [_op_dt(x) for x in arr])
	elif op == "int-array-default":
		doc.set_int_array_default(path, [int(x) for x in arr])
	elif op == "float-array-default":
		doc.set_float_array_default(path, [float(x) for x in arr])
	elif op == "bool-array-default":
		doc.set_bool_array_default(path, [x == "true" for x in arr])
	elif op == "string-array-default":
		doc.set_string_array_default(path, [_unescape_ops(x) for x in arr])
	elif op == "datetime-array-default":
		doc.set_datetime_array_default(path, [_op_dt(x) for x in arr])
	elif op == "raw":
		doc.set_raw(path, _unescape_ops(get(3)), v)
	elif op == "raw-default":
		doc.set_raw_default(path, _unescape_ops(get(3)), v)
	elif op == "empty":
		doc.set_empty(path)
	elif op == "comment":
		doc.set_comment(path, v)
	elif op == "remove":
		doc.remove(path)
	else:
		raise ValueError("unknown op: {}".format(op))


def do_set(o):
	if len(o.args) != 1:
		sys.stderr.write("set needs FILE (ops on stdin; see --help)\n")
		return 1
	file = o.args[0]
	# Base doc: '-' means an empty base, since stdin carries the ops script.
	if file == "-":
		text = ""
	else:
		try:
			text = read_input(file)
		except (OSError, ValueError) as e:
			sys.stderr.write(str(e) + "\n")
			return 1
	doc, code = load_doc(text, o.strictness)
	if doc is None:
		return code
	ops = sys.stdin.buffer.read().decode("utf-8", "replace")
	for n, line in enumerate(ops.split("\n")):
		line = line[:-1] if line.endswith("\r") else line
		if line == "" or line.startswith("#"):
			continue
		try:
			apply_op(doc, line)
		except ValueError as e:
			sys.stderr.write("op line {}: {}\n".format(n + 1, e))
			return 1
	sys.stdout.write(doc.to_canonical())
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
	except shcl.LoadError as le:
		diags = le.diagnostics
		strict_failed = True
	# stdout carries the stable codes - the cross-binding contract. The prose is
	# per-binding voice and goes to stderr (which the differential check drops).
	errors = 0
	for d in diags:
		print("line {}: {}: {}".format(d.line, d.severity.name, d.code))
		sys.stderr.write("line {}: {}: {}\n".format(d.line, d.severity.name, d.message))
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


def do_enum(o, want_count):
	if len(o.args) != 2:
		sys.stderr.write("count/instances need FILE and PATH (see --help)\n")
		return 1
	file, path = o.args[0], o.args[1]
	try:
		text = read_input(file)
	except (OSError, ValueError) as e:
		sys.stderr.write(str(e) + "\n")
		return 1
	doc, code = load_doc(text, o.strictness)
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
	wants_help = any(a in ("-h", "--help") for a in argv)
	wants_version = any(a in ("-V", "--version") for a in argv)
	if not argv or wants_help or argv[0] == "help":
		sys.stdout.write(HELP)
		return 1 if not argv else 0
	if wants_version or argv[0] == "version":
		print("shcl {}".format(VERSION))
		return 0
	try:
		o = parse_opts(argv[1:])
	except ValueError as e:
		sys.stderr.write(str(e) + "\n")
		return 1
	cmd = argv[0]
	if cmd == "get":
		return do_get(o)
	if cmd == "set":
		return do_set(o)
	if cmd == "fmt":
		return do_fmt(o)
	if cmd == "check":
		return do_check(o)
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
