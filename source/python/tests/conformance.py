#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Jim Collier

# Conformance-corpus runner for the Python binding. Same corpus every shipped
# binding must pass; column meanings live in project/conformance/README.md. Plain
# stdlib (no pytest) so cicd runs it with a bare python3. Exit nonzero on any miss.

import os
import sys

_HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))            # source/python (the lib)
import shcl  # noqa: E402

_REPO = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))   # github/
CORPUS = os.path.join(_REPO, "project", "conformance")


def tsv_escape(s):
	return s.replace("\n", "\\n").replace("\t", "\\t")


def parse_level(s):
	if s in (None, "", "standard"):
		return shcl.Strictness.Standard
	if s == "loose":
		return shcl.Strictness.Loose
	if s == "strict":
		return shcl.Strictness.Strict
	raise SystemExit("unknown level '{}' in reads.tsv".format(s))


def load_cases():
	cases = []
	for name in sorted(os.listdir(CORPUS)):
		d = os.path.join(CORPUS, name)
		if not os.path.isdir(d):
			continue
		inp = os.path.join(d, "input.shcl")
		if not os.path.exists(inp):
			continue
		case = {
			"name": name,
			"input": _read(inp),
			"expected": _read(os.path.join(d, "expected.shcl")),
			"reads": _read(os.path.join(d, "reads.tsv")),
			"expected_diags": _read(os.path.join(d, "expected-diags.txt")),
			"write_ops": None,
			"expected_write": None,
			"write_bad_ops": None,
		}
		ops = os.path.join(d, "write.ops")
		if os.path.exists(ops):
			case["write_ops"] = _read(ops)
			case["expected_write"] = _read(os.path.join(d, "expected-write.shcl"))
		bad = os.path.join(d, "write-bad.ops")
		if os.path.exists(bad):
			case["write_bad_ops"] = _read(bad)
		case["schema"] = None
		case["expected_validate"] = None
		sch = os.path.join(d, "schema.shcl")
		if os.path.exists(sch):
			case["schema"] = _read(sch)
			case["expected_validate"] = _read(os.path.join(d, "expected-validate.txt"))
		case["layers"] = []
		case["merge_sets"] = ""
		case["expected_merged"] = None
		em = os.path.join(d, "expected-merged.shcl")
		if os.path.exists(em):
			# Layer files: every layer*.shcl, in filename (= priority) order.
			for n in sorted(os.listdir(d)):
				if n.startswith("layer") and n.endswith(".shcl"):
					case["layers"].append(_read(os.path.join(d, n)))
			ms = os.path.join(d, "merge.sets")
			if os.path.exists(ms):
				case["merge_sets"] = _read(ms)
			case["expected_merged"] = _read(em)
		case["init_schema"] = None
		case["expected_init"] = None
		isch = os.path.join(d, "init-schema.shcl")
		if os.path.exists(isch):
			case["init_schema"] = _read(isch)
			case["expected_init"] = _read(os.path.join(d, "expected-init.shcl"))
		cases.append(case)
	if not cases:
		raise SystemExit("no corpus cases found under {}".format(CORPUS))
	return cases


def _read(path):
	with open(path, "rb") as f:
		return f.read().decode("utf-8")


def scalar_read(doc, kind, query):
	# Returns (value_string, status, slot_statuses).
	if kind == "int":
		r = doc.read_int(query)
		return str(r.value), r.status, r.slots
	if kind == "float":
		r = doc.read_float(query)
		return shcl.format_float(r.value), r.status, r.slots
	if kind == "bool":
		r = doc.read_bool(query)
		return ("true" if r.value else "false"), r.status, r.slots
	if kind == "datetime":
		r = doc.read_datetime(query)
		return str(r.value), r.status, r.slots
	if kind == "string":
		r = doc.read_string(query)
		return tsv_escape(r.value), r.status, r.slots
	if kind == "raw":
		r = doc.read_raw(query)
		return tsv_escape(r.value), r.status, r.slots
	if kind == "rawinfo":
		r = doc.read_raw_info(query)
		return tsv_escape(r.value), r.status, r.slots
	if kind == "int[]":
		r = doc.read_int_array(query)
		return "|".join(str(v) for v in r.value), r.status, r.slots
	if kind == "float[]":
		r = doc.read_float_array(query)
		return "|".join(shcl.format_float(v) for v in r.value), r.status, r.slots
	if kind == "bool[]":
		r = doc.read_bool_array(query)
		return "|".join("true" if v else "false" for v in r.value), r.status, r.slots
	if kind == "datetime[]":
		r = doc.read_datetime_array(query)
		return "|".join(str(v) for v in r.value), r.status, r.slots
	if kind == "string[]":
		r = doc.read_string_array(query)
		return "|".join(tsv_escape(v) for v in r.value), r.status, r.slots
	raise SystemExit("unknown type '{}'".format(kind))


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
		out.append({"n": "\n", "t": "\t", "\\": "\\"}.get(nxt, "\\" + nxt))
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
	v = int(s)
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


def try_apply_op(doc, line):
	# Apply one write-ops line via the library Writer, with the same value gates
	# the CLI applies. A returned string = the op must be rejected (bad value or
	# unusable path); None = applied.
	f = line.split("\t")

	def g(i):
		return f[i] if i < len(f) else ""

	path, v = g(1), g(2)
	arr = f[2:] if len(f) > 2 else []
	op = f[0]
	try:
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
			wrote = doc.set_raw(path, _unescape_ops(g(3)), v)
		elif op == "raw-default":
			wrote = doc.set_raw_default(path, _unescape_ops(g(3)), v)
		elif op == "empty":
			wrote = doc.set_empty(path)
		elif op == "comment":
			wrote = doc.set_comment(path, v)
		elif op == "remove":
			doc.remove(path)
			wrote = True
		else:
			return "unknown op: {}".format(op)
	except ValueError as e:
		return str(e)
	if not wrote:
		return "cannot write {}".format(path)
	return None


def apply_op(doc, line, at):
	# Good-path wrapper: the op must apply.
	err = try_apply_op(doc, line)
	if err is not None:
		raise SystemExit("{}: {}".format(at, err))


def main():
	fails = []
	cases = load_cases()

	# Write dimension: the library Writer must reproduce expected-write.shcl and
	# the result must be a formatter fixpoint.
	for case in cases:
		if case["write_ops"] is None:
			continue
		doc = shcl.Document.parse(case["input"])
		for n, line in enumerate(case["write_ops"].split("\n")):
			line = line[:-1] if line.endswith("\r") else line
			if line == "" or line.startswith("#"):
				continue
			apply_op(doc, line, "{}: write.ops line {}".format(case["name"], n + 1))
		got = doc.to_canonical()
		if got != case["expected_write"]:
			fails.append("{}: writer output differs from expected-write.shcl".format(case["name"]))
		if shcl.Document.parse(got).to_canonical() != got:
			fails.append("{}: written output is not a fmt fixpoint".format(case["name"]))

	# Bad-op dimension: each write-bad.ops line, applied alone to the case
	# input, must be rejected (bad value, bad datetime, or unusable path) and
	# leave the document unchanged.
	for case in cases:
		if case["write_bad_ops"] is None:
			continue
		for n, line in enumerate(case["write_bad_ops"].split("\n")):
			line = line[:-1] if line.endswith("\r") else line
			if line == "" or line.startswith("#"):
				continue
			doc = shcl.Document.parse(case["input"])
			before = doc.to_canonical()
			if try_apply_op(doc, line) is None:
				fails.append("{}: write-bad.ops line {} was accepted: {}".format(case["name"], n + 1, line))
			if doc.to_canonical() != before:
				fails.append("{}: write-bad.ops line {} changed the document: {}".format(case["name"], n + 1, line))

	# Layered-load dimension: fold the layer files (lowest first) and input.shcl
	# (highest file layer) via the library merge, apply the path=value overrides
	# as the top layer, and match the golden merged canonical.
	for case in cases:
		if case["expected_merged"] is None:
			continue
		texts = list(case["layers"]) + [case["input"]]
		doc = shcl.Document.parse(texts[0])
		for t in texts[1:]:
			doc.merge(shcl.Document.parse(t))
		for line in case["merge_sets"].split("\n"):
			if line == "" or line.startswith("#"):
				continue
			eq = line.find("=")
			if eq < 0:
				raise SystemExit("{}: bad merge.sets line: {}".format(case["name"], line))
			doc.set_string(line[:eq], line[eq + 1:])
		got = doc.to_canonical()
		if got != case["expected_merged"]:
			fails.append("{}: merged output differs from expected-merged.shcl".format(case["name"]))
		if shcl.Document.parse(got).to_canonical() != got:
			fails.append("{}: merged output is not a fmt fixpoint".format(case["name"]))

	# Generation dimension: generate on the schema must reproduce the golden
	# starter config, and that output must itself load cleanly.
	for case in cases:
		if case["init_schema"] is None:
			continue
		text, ifaults = shcl.generate(shcl.Document.parse(case["init_schema"]))
		if ifaults:
			fails.append("{}: init schema has faults".format(case["name"]))
			continue
		if text != case["expected_init"]:
			fails.append("{}: init output differs from expected-init.shcl".format(case["name"]))
		if any(d.severity == shcl.Severity.Error for d in shcl.Document.parse(text).diagnostics()):
			fails.append("{}: generated starter does not load cleanly".format(case["name"]))

	# Diagnostics: count, line, severity, and stable code per case - the same
	# shape `check` prints to stdout at Standard (its cross-binding contract).
	for case in cases:
		diags = shcl.Document.parse(case["input"]).diagnostics()
		got = ""
		errors = 0
		for d in diags:
			got += "line {}: {}: {}\n".format(d.line, d.severity.name, d.code)
			if d.severity == shcl.Severity.Error:
				errors += 1
		if errors > 0:
			got += "failed: {} diagnostic(s), {} error(s)\n".format(len(diags), errors)
		else:
			got += "ok ({} diagnostic(s))\n".format(len(diags))
		if got != case["expected_diags"]:
			fails.append("{}: diagnostics differ from expected-diags.txt".format(case["name"]))

	# Schema dimension: golden = the exact `check --schema` stdout at Standard
	# (doc parse diags, then validation diags, then the summary). A schema that
	# does not load cleanly is a single V099, mirroring the CLI.
	for case in cases:
		if case["schema"] is None:
			continue
		doc = shcl.Document.parse(case["input"])
		diags = list(doc.diagnostics())
		sdoc = shcl.Document.parse(case["schema"])
		if any(sd.severity == shcl.Severity.Error for sd in sdoc.diagnostics()):
			diags.append(shcl.Diagnostic(0, shcl.Severity.Error, "schema failed to load", "V099"))
		else:
			diags.extend(doc.validate(sdoc))
		got = ""
		errors = 0
		for d in diags:
			got += "line {}: {}: {}\n".format(d.line, d.severity.name, d.code)
			if d.severity == shcl.Severity.Error:
				errors += 1
		if errors > 0:
			got += "failed: {} diagnostic(s), {} error(s)\n".format(len(diags), errors)
		else:
			got += "ok ({} diagnostic(s))\n".format(len(diags))
		if got != case["expected_validate"]:
			fails.append("{}: validation output differs from expected-validate.txt".format(case["name"]))

	# The canonical formatter must match expected.shcl and be a fixpoint.
	for case in cases:
		got = shcl.Document.parse(case["input"]).to_canonical()
		if got != case["expected"]:
			fails.append("{}: canonical output differs from expected.shcl".format(case["name"]))
		again = shcl.Document.parse(got).to_canonical()
		if again != got:
			fails.append("{}: formatter is not idempotent".format(case["name"]))

	# Typed reads must match expected value + status.
	for case in cases:
		for n, line in enumerate(case["reads"].split("\n")):
			if n == 0 or not line.strip():
				continue
			cols = line.split("\t")
			if len(cols) < 4:
				fails.append("{}: reads.tsv line {} too short".format(case["name"], n + 1))
				continue
			query, kind, expected, status = cols[0], cols[1], cols[2], cols[3]
			level = parse_level(cols[4] if len(cols) > 4 else None)
			at = "{}: reads.tsv line {} ({} {})".format(case["name"], n + 1, query, kind)

			if kind == "load":
				try:
					shcl.Document.parse_with(case["input"], level)
					ok = True
				except shcl.LoadError:
					ok = False
				want = {"ok": True, "fail": False}.get(expected)
				if want is None:
					fails.append("{}: bad load expectation '{}'".format(at, expected))
				elif ok != want:
					fails.append("{}: load outcome (got {}, want {})".format(at, ok, want))
				continue

			try:
				doc = shcl.Document.parse_with(case["input"], level)
			except shcl.LoadError as e:
				fails.append("{}: load failed but reads.tsv has reads there: {}".format(at, e))
				continue

			if kind == "count":
				if str(doc.count(query)) != expected:
					fails.append("{}: count got {} want {}".format(at, doc.count(query), expected))
				continue
			if kind == "instances":
				got = "|".join(doc.instances(query))
				if got != expected:
					fails.append("{}: instances got {!r} want {!r}".format(at, got, expected))
				continue

			got_value, got_status, got_slots = scalar_read(doc, kind, query)
			if got_status.name != status:
				fails.append("{}: status got {} want {}".format(at, got_status.name, status))
			if expected != "-" and got_value != expected:
				fails.append("{}: value got {!r} want {!r}".format(at, got_value, expected))
			# Optional 6th column: per-slot statuses, |-joined (needs col 5 set).
			if len(cols) > 5:
				got = "|".join(st.name for st in got_slots)
				if got != cols[5]:
					fails.append("{}: slots got {!r} want {!r}".format(at, got, cols[5]))

	if fails:
		for f in fails:
			sys.stderr.write("FAIL " + f + "\n")
		sys.stderr.write("conformance: {} failure(s)\n".format(len(fails)))
		return 1
	print("conformance: {} case(s) pass".format(len(cases)))
	return 0


if __name__ == "__main__":
	sys.exit(main())
