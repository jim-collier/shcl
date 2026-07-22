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
			"write_ops": None,
			"expected_write": None,
		}
		ops = os.path.join(d, "write.ops")
		if os.path.exists(ops):
			case["write_ops"] = _read(ops)
			case["expected_write"] = _read(os.path.join(d, "expected-write.shcl"))
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
		raise SystemExit("bad datetime: {}".format(s))
	return dt


def apply_op(doc, line, at):
	f = line.split("\t")

	def g(i):
		return f[i] if i < len(f) else ""

	path, v = g(1), g(2)
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
		doc.set_raw(path, _unescape_ops(g(3)), v)
	elif op == "raw-default":
		doc.set_raw_default(path, _unescape_ops(g(3)), v)
	elif op == "empty":
		doc.set_empty(path)
	elif op == "comment":
		doc.set_comment(path, v)
	elif op == "remove":
		doc.remove(path)
	else:
		raise SystemExit("{}: unknown op '{}'".format(at, op))


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
