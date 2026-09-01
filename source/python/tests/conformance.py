#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

# Conformance-corpus runner for the Python binding. Same corpus every shipped
# binding must pass; column meanings live in project/conformance/README.md. Plain
# stdlib (no pytest) so cicd runs it with a bare python3. Exit nonzero on any miss.

import math
import os
import stat
import sys
import tracemalloc
import types
from pathlib import Path
from typing import Any

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
	raise SystemExit(f"unknown level '{s}' in reads.tsv")


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
		raise SystemExit(f"no corpus cases found under {CORPUS}")
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
	raise SystemExit(f"unknown type '{kind}'")


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
	# float() after the grammar gate is safe. Overflow (1e400) yields inf,
	# which the reader refuses, so it is a bad value like the CLI says.
	if not _float_grammar_ok(s):
		raise ValueError(f"bad float: {s}")
	x = float(s)
	if not math.isfinite(x):
		raise ValueError(f"bad float: {s}")
	return x


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
			return f"unknown op: {op}"
	except ValueError as e:
		return str(e)
	if not wrote:
		return f"cannot write {path}"
	return None


def apply_op(doc, line, at):
	# Good-path wrapper: the op must apply.
	err = try_apply_op(doc, line)
	if err is not None:
		raise SystemExit(f"{at}: {err}")


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
			apply_op(doc, line, f"{case['name']}: write.ops line {n + 1}")
		got = doc.to_canonical()
		if got != case["expected_write"]:
			fails.append(f"{case['name']}: writer output differs from expected-write.shcl")
		if shcl.Document.parse(got).to_canonical() != got:
			fails.append(f"{case['name']}: written output is not a fmt fixpoint")

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
				fails.append(f"{case['name']}: write-bad.ops line {n + 1} was accepted: {line}")
			if doc.to_canonical() != before:
				fails.append(f"{case['name']}: write-bad.ops line {n + 1} changed the document: {line}")

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
				raise SystemExit(f"{case['name']}: bad merge.sets line: {line}")
			doc.set_string(line[:eq], line[eq + 1:])
		got = doc.to_canonical()
		if got != case["expected_merged"]:
			fails.append(f"{case['name']}: merged output differs from expected-merged.shcl")
		if shcl.Document.parse(got).to_canonical() != got:
			fails.append(f"{case['name']}: merged output is not a fmt fixpoint")

	# Generation dimension: generate on the schema must reproduce the golden
	# starter config, and that output must itself load cleanly.
	for case in cases:
		if case["init_schema"] is None:
			continue
		text, ifaults = shcl.generate(shcl.Document.parse(case["init_schema"]))
		if ifaults:
			fails.append(f"{case['name']}: init schema has faults")
			continue
		if text != case["expected_init"]:
			fails.append(f"{case['name']}: init output differs from expected-init.shcl")
		# The footer is the only difference the flag makes: everything before
		# it is byte-for-byte what the default run produced.
		bare, _ = shcl.generate(shcl.Document.parse(case["init_schema"]), True)
		if not bare or not text.startswith(bare):
			fails.append(f"{case['name']}: --no-banner output is not a prefix of the default")
		elif "This config file format is SHCL." not in text[len(bare):]:
			fails.append(f"{case['name']}: default init output is missing the format footer")
		gdoc = shcl.Document.parse(text)
		if any(d.severity == shcl.Severity.Error for d in gdoc.diagnostics()):
			fails.append(f"{case['name']}: generated starter does not load cleanly")
		# And it must satisfy the very schema that produced it - case 026's
		# golden once failed its own schema (repeat lower bound and a
		# materialized wildcard were ignored).
		sdoc = shcl.Document.parse(case["init_schema"])
		if any(d.severity == shcl.Severity.Error for d in gdoc.validate(sdoc)):
			fails.append(f"{case['name']}: generated starter fails its own schema")

	# Diagnostics: count, line, severity, and stable code per case - the same
	# shape `check` prints to stdout at Standard (its cross-binding contract).
	for case in cases:
		diags = shcl.Document.parse(case["input"]).diagnostics()
		got = ""
		errors = 0
		for d in diags:
			got += f"line {d.line}: {d.severity.name}: {d.code}\n"
			if d.severity == shcl.Severity.Error:
				errors += 1
		if errors > 0:
			got += f"failed: {len(diags)} diagnostic(s), {errors} error(s)\n"
		else:
			got += f"ok ({len(diags)} diagnostic(s))\n"
		if got != case["expected_diags"]:
			fails.append(f"{case['name']}: diagnostics differ from expected-diags.txt")

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
			shcl.suppress_declared_repeats(sdoc, diags)
			shcl.suppress_declared_reopens(sdoc, diags)
		got = ""
		errors = 0
		for d in diags:
			got += f"line {d.line}: {d.severity.name}: {d.code}\n"
			if d.severity == shcl.Severity.Error:
				errors += 1
		if errors > 0:
			got += f"failed: {len(diags)} diagnostic(s), {errors} error(s)\n"
		else:
			got += f"ok ({len(diags)} diagnostic(s))\n"
		if got != case["expected_validate"]:
			fails.append(f"{case['name']}: validation output differs from expected-validate.txt")

	# The canonical formatter must match expected.shcl and be a fixpoint.
	for case in cases:
		got = shcl.Document.parse(case["input"]).to_canonical()
		if got != case["expected"]:
			fails.append(f"{case['name']}: canonical output differs from expected.shcl")
		again = shcl.Document.parse(got).to_canonical()
		if again != got:
			fails.append(f"{case['name']}: formatter is not idempotent")

	# Typed reads must match expected value + status.
	for case in cases:
		for n, line in enumerate(case["reads"].split("\n")):
			if n == 0 or not line.strip():
				continue
			cols = line.split("\t")
			if len(cols) < 4:
				fails.append(f"{case['name']}: reads.tsv line {n + 1} too short")
				continue
			query, kind, expected, status = cols[0], cols[1], cols[2], cols[3]
			level = parse_level(cols[4] if len(cols) > 4 else None)
			at = f"{case['name']}: reads.tsv line {n + 1} ({query} {kind})"

			if kind == "load":
				try:
					shcl.Document.parse_with(case["input"], level)
					ok = True
				except shcl.LoadError:
					ok = False
				want = {"ok": True, "fail": False}.get(expected)
				if want is None:
					fails.append(f"{at}: bad load expectation '{expected}'")
				elif ok != want:
					fails.append(f"{at}: load outcome (got {ok}, want {want})")
				continue

			try:
				doc = shcl.Document.parse_with(case["input"], level)
			except shcl.LoadError as e:
				fails.append(f"{at}: load failed but reads.tsv has reads there: {e}")
				continue

			if kind == "count":
				if str(doc.count(query)) != expected:
					fails.append(f"{at}: count got {doc.count(query)} want {expected}")
				continue
			if kind == "instances":
				got = "|".join(doc.instances(query))
				if got != expected:
					fails.append(f"{at}: instances got {got!r} want {expected!r}")
				continue
			if kind == "children":
				got = "|".join(doc.children(query))
				if got != expected:
					fails.append(f"{at}: children got {got!r} want {expected!r}")
				continue
			if kind == "paths":
				got = "|".join(doc.paths())
				if got != expected:
					fails.append(f"{at}: paths got {got!r} want {expected!r}")
				continue

			got_value, got_status, got_slots = scalar_read(doc, kind, query)
			if got_status.name != status:
				fails.append(f"{at}: status got {got_status.name} want {status}")
			if expected != "-" and got_value != expected:
				fails.append(f"{at}: value got {got_value!r} want {expected!r}")
			# Optional 6th column: per-slot statuses, |-joined (needs col 5 set).
			if len(cols) > 5:
				got = "|".join(st.name for st in got_slots)
				if got != cols[5]:
					fails.append(f"{at}: slots got {got!r} want {cols[5]!r}")

	if fails:
		for f in fails:
			sys.stderr.write("FAIL " + f + "\n")
		sys.stderr.write(f"conformance: {len(fails)} failure(s)\n")
		return 1
	# paths(): file order, deduplicated, non-bare segments quoted so every
	# path resolves. Same fixture is pinned in every runner.
	pdoc = shcl.Document.parse('a: 1\na.b: 2\n"q n": 3\nx:\n\tb: 4\nx.b: 5\n')
	if pdoc.paths() != ["a", "a.b", '"q n"', "x", "x.b"]:
		raise SystemExit(f"paths() fixture mismatch: {pdoc.paths()}")
	for p in pdoc.paths():
		if pdoc.count(p) < 1:
			raise SystemExit("emitted path does not resolve: " + p)
	# quote_segment: same spelling both directions, injection-safe.
	if (shcl.quote_segment("port"), shcl.quote_segment("q n"), shcl.quote_segment("a.b")) != ("port", '"q n"', '"a.b"'):
		raise SystemExit("quote_segment spelling drift")
	qr = pdoc.read_int(shcl.quote_segment("q n"))
	if (qr.value, qr.status) != (3, shcl.Status.Good):
		raise SystemExit("quoted segment read failed")
	# One combined diagnostics list (parse first, then validation) and an
	# error predicate, so recover-and-continue can't read as success by
	# accident. Same fixture in every runner.
	otext = ": nope\nport: x\n"
	oschema = "field: port\n\ttype: int\n"
	odoc = shcl.Document.load_and_validate(otext, oschema, shcl.Strictness.Standard)
	ocodes = [d.code for d in odoc.diagnostics()]
	if ocodes != ["E014", "V003"]:
		raise SystemExit(f"load_and_validate codes got {ocodes}")
	if odoc.error_count() != 2:
		raise SystemExit(f"error_count got {odoc.error_count()}")
	if odoc.read_string("port").value != "x":  # doc still usable
		raise SystemExit("load_and_validate doc not readable")
	# Strict never raises here; the diagnostics are the answer.
	ostrict = shcl.Document.load_and_validate(otext, oschema, shcl.Strictness.Strict)
	if ostrict.error_count() < 2:
		raise SystemExit(f"strict error_count got {ostrict.error_count()}")
	# An empty schema declares nothing and validates nothing.
	oplain = shcl.Document.load_and_validate("a: 1\n", "", shcl.Strictness.Standard)
	if (oplain.error_count(), len(oplain.diagnostics())) != (0, 0):
		raise SystemExit("empty-schema load_and_validate not clean")
	# write_reason: the reason behind a setter's bare False. Same fixture in
	# every runner.
	wdoc = shcl.Document.parse("a:\n\tb: 1\n")
	for wpath, wwant in (
		("a.b", shcl.WriteReason.Writable),
		("a.new[Boston].x", shcl.WriteReason.Writable),   # creatable
		("", shcl.WriteReason.BadPath),
		("a..b", shcl.WriteReason.BadPath),
		("a.b: 2", shcl.WriteReason.ValueInPath),
		("a[*].b", shcl.WriteReason.Wildcard),
		("a[#5].b", shcl.WriteReason.NoSuchIndex),
		("nope[#0].b", shcl.WriteReason.NoSuchIndex),
		(".".join(["d"] * 513), shcl.WriteReason.TooDeep),
		# A literal line break in a SELECTOR: the binding would emit across two
		# lines and reparse as neither, and the value emitter never escapes one.
		# In a NAME it is writable - names emit through the name escaper, which
		# spells a line break \n, so the escaped and literal spellings are one
		# path now. Not corpus-pinnable - an ops line cannot carry a raw newline.
		('a["p\nq"].b', shcl.WriteReason.BadPath),
		('"x\ny".b', shcl.WriteReason.Writable),
		('"x\\ny".b', shcl.WriteReason.Writable),
	):
		wgot = wdoc.write_reason(wpath)
		if wgot is not wwant:
			raise SystemExit(f"write_reason({wpath!r}) got {wgot} want {wwant}")
	# The probe never creates: the doc is unchanged after all of the above.
	if wdoc.count("a") != 1:
		raise SystemExit("write_reason probe created an instance")
	if wdoc.paths() != ["a", "a.b"]:
		raise SystemExit(f"write_reason probe changed paths: {wdoc.paths()}")
	# Each setter is the inverse of its read, so a value with no spelling the
	# reader accepts fails the write and leaves the document alone. Same
	# fixture in every runner.
	sdoc = shcl.Document.parse("z: 0\n")
	for v in (math.inf, -math.inf, math.nan):
		if sdoc.set_float("f", v) or sdoc.set_float_default("f", v) or sdoc.set_float_array("f", [1.0, v]):
			raise SystemExit(f"float {v} was written")
	if not sdoc.set_float("f", 2.5) or sdoc.get_float("f") != 2.5:
		raise SystemExit("a finite float was refused")
	DT = shcl.ShclDateTime
	bad_dts = [
		DT(),                                                  # nothing written
		DT(date=(2026, 13, 1)),                                # month 13
		DT(date=(2026, 2, 30)),                                # February 30
		DT(date=(-1, 1, 1)),                                   # negative year
		DT(time=(24, 0, None)),                                # hour 24
		DT(time=(1, 2, None), frac="5"),                       # fraction with no seconds
		DT(time=(1, 2, 3), frac=""),                           # empty fraction
		DT(time=(1, 2, 3), frac="12345678901"),                # 11 digits
		DT(time=(1, 2, None), zone=("offset", 9999)),          # +166:39
		DT(date=(2026, 1, 1), zone=("utc", None)),             # zone on a date alone
	]
	for dt in bad_dts:
		if sdoc.set_datetime("d", dt) or sdoc.set_datetime_default("d", dt) or sdoc.set_datetime_array("d", [DT(date=(2026, 1, 1)), dt]):
			raise SystemExit(f"datetime {dt} was written")
	ok_dt = DT(date=(2026, 1, 2), time=(3, 4, 5), frac="60", zone=("offset", -90))
	if not sdoc.set_datetime("d", ok_dt) or str(sdoc.get_datetime("d")) != str(ok_dt):
		raise SystemExit("a valid datetime was refused or read back differently")
	if sdoc.to_canonical() != 'z: 0\n\nf: 2.5\n\nd: "2026-01-02T03:04:05.60-01:30"\n':
		raise SystemExit(f"document after the refusals: {sdoc.to_canonical()!r}")
	# A raw body is the only content kept untrimmed, so it is the only place a
	# trailing CR survives the load - and one written back becomes CRLF, which
	# reads as neither. The whole trailing run comes off instead; a CR inside a
	# line is content and stays. Same fixture in every runner: a golden would be
	# rewritten by any platform's line-ending translation.
	rdoc = shcl.Document.parse("r:\n\t~~~\n\tone\r\r\n\ta\rb\n\t~~~\n")
	if rdoc.read_raw("r").value != "one\na\rb":
		raise SystemExit(f"raw content: {rdoc.read_raw('r').value!r}")
	rcanon = rdoc.to_canonical()
	if shcl.Document.parse(rcanon).to_canonical() != rcanon:
		raise SystemExit("raw block with CR is not a formatter fixpoint")
	# line/quoted on the read result, line(path), children(path). Same
	# fixture in every runner (C pins the same answers on shcl_quoted and
	# shcl_line; its read structs stay value+status).
	ldoc = shcl.Document.parse('a: @null\nb: "@null"\ncode:\n\thook: 1\n\thook: 2\n\tdone: 3\n')
	if ldoc.read_string("a").quoted:
		raise SystemExit("unquoted read reports quoted")
	if not ldoc.read_string("b").quoted:
		raise SystemExit("quoted read not flagged")
	if ldoc.read_string("code").quoted:
		raise SystemExit("a block reports quoted")
	if ldoc.read_string("missing").quoted:
		raise SystemExit("missing path reports quoted")
	if ldoc.read_string("b").line != 2:
		raise SystemExit(f"read line got {ldoc.read_string('b').line}")
	if ldoc.line("code.done") != 6:
		raise SystemExit(f"line() got {ldoc.line('code.done')}")
	if ldoc.line("code") != 3:
		raise SystemExit(f"line() on merged instance got {ldoc.line('code')}")
	if ldoc.line("missing") != 0:
		raise SystemExit(f"line() on missing path got {ldoc.line('missing')}")
	# lines(): the plural - a repeated field cites every binding, wildcard
	# slots keep their index (0 = unresolved), a miss is the empty list.
	if ldoc.line("code.hook") != 0:  # Multiple - the singular's gap
		raise SystemExit(f"line() on repeated field got {ldoc.line('code.hook')}")
	if ldoc.lines("code.hook") != [4, 5]:
		raise SystemExit(f"lines() got {ldoc.lines('code.hook')}")
	if ldoc.lines("code.done") != [6]:
		raise SystemExit(f"lines() single got {ldoc.lines('code.done')}")
	if ldoc.lines("a") != [1]:
		raise SystemExit(f"lines() top-level got {ldoc.lines('a')}")
	if ldoc.lines("code[*].done") != [6]:
		raise SystemExit(f"lines() wildcard got {ldoc.lines('code[*].done')}")
	if ldoc.lines("code[*].nope") != [0]:
		raise SystemExit(f"lines() unresolved slot got {ldoc.lines('code[*].nope')}")
	if ldoc.lines("missing"):
		raise SystemExit("lines() on missing path not empty")
	if ldoc.children("code") != ["hook", "hook", "done"]:
		raise SystemExit(f"children() got {ldoc.children('code')}")
	if ldoc.children("") != ["a", "b", "code"]:
		raise SystemExit(f"top-level children() got {ldoc.children('')}")
	if ldoc.children("missing"):
		raise SystemExit("children() on missing path not empty")
	# authored_name(): the author's spelling, unfolded; merged instances keep
	# the first binding's; unresolved or Multiple is empty; writer-built
	# keeps the setter path's spelling.
	sdoc2 = shcl.Document.parse("SYMBOLS: 3\nCode:\n\tx: 1\ncode:\n\ty: 2\n")
	if sdoc2.authored_name("symbols") != "SYMBOLS":
		raise SystemExit(f"authored_name(symbols) got {sdoc2.authored_name('symbols')}")
	if sdoc2.authored_name("code") != "Code":
		raise SystemExit(f"authored_name(code) got {sdoc2.authored_name('code')}")
	if sdoc2.authored_name("missing") != "":
		raise SystemExit("authored_name(missing) not empty")
	if not sdoc2.set_int("NewTop.n", 1):
		raise SystemExit("set_int NewTop.n failed")
	if sdoc2.authored_name("newtop") != "NewTop":
		raise SystemExit(f"authored_name(newtop) got {sdoc2.authored_name('newtop')}")
	# Escapes ARE resolved on a name, so both spellings of the path find the same
	# node - while authored_name still hands back the source spelling, which is
	# the one thing it is for. Same fixture in every runner.
	sdoc3 = shcl.Document.parse('"Ab\\tCd": 2\n')
	esc_name = sdoc3.authored_name('"ab\\tcd"')
	if esc_name != "Ab\\tCd":
		raise SystemExit(f"authored_name escaped got {esc_name!r}")
	lit_name = sdoc3.authored_name('"ab\tcd"')
	if lit_name != "Ab\\tCd":
		raise SystemExit(f"authored_name via the literal spelling got {lit_name!r}")
	if sdoc3.read_int('"ab\tcd"').value != 2:
		raise SystemExit("read via the literal spelling failed")
	# Canonical output folds the case, as it always has, and escapes the tab.
	if sdoc3.to_canonical() != '"ab\\tcd": 2\n':
		raise SystemExit(f"canonical name spelling got {sdoc3.to_canonical()!r}")
	# parse_limited: the caps exist because a document amplifies to many times
	# its byte size in memory, so read_file's byte cap alone cannot bound a
	# load. Same fixture in every runner.
	ltext = "a: 1\nb: 2\nc: 3\nd: 4\n"
	ldoc = shcl.Document.parse_limited(ltext, shcl.Strictness.Standard, 2, 0)
	lcaps = [g for g in ldoc.diagnostics() if g.code == "E020"]
	if len(lcaps) != 1 or lcaps[0].line != 4:
		raise SystemExit(f"node cap: {[(g.line, g.code) for g in ldoc.diagnostics()]}")
	if ldoc.lost_count() != 1 or ldoc.get_int("a") != 1 or ldoc.exists("d"):
		raise SystemExit("node cap: remainder must be lost and unparsed")
	ldoc = shcl.Document.parse_limited("a: 1\nb: 2\nc: 3", shcl.Strictness.Standard, 2, 0)
	if sum(1 for g in ldoc.diagnostics() if g.code == "E020") != 1 or ldoc.lost_count() != 0:
		raise SystemExit("node cap crossed on the last line must still report")
	ldoc = shcl.Document.parse_limited("x.y.z: 1\n", shcl.Strictness.Standard, 1, 0)
	if sum(1 for g in ldoc.diagnostics() if g.code == "E020") != 1 or ldoc.get_int("x.y.z") != 1:
		raise SystemExit("node cap overshoot line must stay in the document")
	ldoc = shcl.Document.parse_limited(ltext, shcl.Strictness.Standard, 0, 0)
	if ldoc.diagnostics():
		raise SystemExit("uncapped parse_limited must match parse_with")
	ldoc = shcl.Document.parse_limited("arr: 1, 2, 3\nok: 5\n", shcl.Strictness.Standard, 0, 2)
	lcaps = [g for g in ldoc.diagnostics() if g.code == "E021"]
	if len(lcaps) != 1 or lcaps[0].line != 1 or ldoc.exists("arr") or ldoc.get_int("ok") != 5 or ldoc.lost_count() != 1:
		raise SystemExit("element cap: the whole line is refused, the rest untouched")
	ldoc = shcl.Document.parse_limited("arr:\n\t* 1\n\t* 2\n\t* 3\n", shcl.Strictness.Standard, 0, 2)
	if sum(1 for g in ldoc.diagnostics() if g.code == "E021") != 1 or ldoc.get_int_array("arr") != [1, 2]:
		raise SystemExit("element cap: a stacked array keeps what fit")
	# The count the cap judges is the count the array reads back as, spelling
	# by spelling: quoted and escaped commas, empty and blank slots, Unicode
	# blanks, a quote that never closes. Refused at one under, kept at exact.
	counts = [
		("1, 2, 3", 3), ('"a, b", c', 2), ("a\\, b, c", 2), ("a,,b", 2),
		("a, , b", 2), (" a ", 1), ("\"\", ''", 2), ("'a\", b'", 1),
		('"open, b', 1), ("\\", 1), ("x,\u3000", 1), ("x, \u00a0y", 2), (", , ,", 0),
	]
	for spelling, n in counts:
		text = f"v: {spelling}\n"
		ldoc = shcl.Document.parse_limited(text, shcl.Strictness.Standard, 0, max(n, 1))
		if any(g.code == "E021" for g in ldoc.diagnostics()):
			raise SystemExit(f"{spelling!r} at cap {n}: E021")
		r = ldoc.read_string_array("v")
		if n == 0:
			if not ldoc.exists("v") or r.status == shcl.Status.Good:
				raise SystemExit(f"{spelling!r}: want empty, got {r.status}")
		elif r.status != shcl.Status.Good or len(r.value) != n:
			raise SystemExit(f"{spelling!r}: want {n} elements, got {r.value} {r.status}")
		if n >= 2:
			ldoc = shcl.Document.parse_limited(text, shcl.Strictness.Standard, 0, n - 1)
			if ldoc.lost_count() != 1:
				raise SystemExit(f"{spelling!r} at cap {n - 1}: not refused")
	# A refused line reports the cap alone: the quote check runs after it, so
	# it never splits a value the cap already turned away.
	ldoc = shcl.Document.parse_limited('v: a, "open, b\n', shcl.Strictness.Standard, 0, 1)
	if [g.code for g in ldoc.diagnostics()] != ["E021"]:
		raise SystemExit("refused line must report the cap alone")
	# What a capped parse holds is the text and its lines. The refused line
	# used to be built in full first (38x the text), so the cap saved nothing.
	btext = "arr: " + "1, " * 200000 + "\nok: 5\n"
	tracemalloc.start()
	ldoc = shcl.Document.parse_limited(btext, shcl.Strictness.Standard, 0, 8)
	lpeak = tracemalloc.get_traced_memory()[1]
	tracemalloc.stop()
	if ldoc.lost_count() != 1 or ldoc.get_int("ok") != 5:
		raise SystemExit("capped parse: wrong result")
	if lpeak > len(btext) * 8:
		raise SystemExit(f"a capped parse held the array it refused: {lpeak} bytes for {len(btext)} of text")
	try:
		shcl.Document.parse_limited(ltext, shcl.Strictness.Strict, 2, 0)
		raise SystemExit("capped strict load must fail")
	except shcl.LoadError as ex:
		if ex.document is None or ex.document.get_int("a") != 1:
			raise SystemExit("failed strict doc must stay readable") from None
	# load_file/save_file: the status separates absent / unreadable / parsed
	# with errors / clean, and a save round-trips through the atomic write.
	# Same fixture in every runner.
	import tempfile
	with tempfile.TemporaryDirectory() as td:
		fpath = os.path.join(td, "t.shcl")
		_, fst = shcl.Document.load_file(fpath)
		if fst != shcl.FileStatus.NotFound:
			raise SystemExit(f"load_file missing got {fst}")
		_, fst = shcl.Document.load_file(td)  # a directory is not readable
		if fst != shcl.FileStatus.Unreadable:
			raise SystemExit(f"load_file directory got {fst}")
		# Python-only, so not in the shared fixture: a NUL in the path raises
		# ValueError out of the path calls where the other bindings report. Both
		# halves promise a status or a message, never a throw.
		_, fst = shcl.Document.load_file("a\0b")
		if fst != shcl.FileStatus.Unreadable:
			raise SystemExit(f"load_file NUL path got {fst}")
		try:
			shcl.Document.parse("a: 1").save_file("a\0b")
			raise SystemExit("save_file NUL path reported success")
		except shcl.SaveFailed:
			pass
		# Also python-only: an int outside the 64-bit range the other three
		# bindings use writes text every reader calls bad-type, so the setter
		# refuses instead. The edges themselves still write.
		rdoc = shcl.Document.parse("a: 1")
		if rdoc.set_int("b", 1 << 63) or rdoc.set_int("b", -(1 << 63) - 1):
			raise SystemExit("set_int accepted an out-of-range value")
		if rdoc.set_int_array("c", [1, 1 << 64]):
			raise SystemExit("set_int_array accepted an out-of-range value")
		if not rdoc.set_int("d", (1 << 63) - 1) or not rdoc.set_int("e", -(1 << 63)):
			raise SystemExit("set_int refused an in-range edge")
		if rdoc.to_canonical() != "a: 1\n\nd: 9223372036854775807\n\ne: -9223372036854775808\n":
			raise SystemExit(f"set_int range check left the wrong document: {rdoc.to_canonical()!r}")
		# Also python-only: the frame budget is small, so a document at the
		# documented depth cap has to survive every walk from an already-deep
		# caller. Merge and clone were the two still recursing.
		deep = "\n".join("\t" * i + f"l{i}:" for i in range(511)) + "\n" + "\t" * 511 + "v: 1\n"

		def _at_depth(k, fn):
			if k:
				return _at_depth(k - 1, fn)
			return fn()

		def _merge_deep():
			base = shcl.Document.parse(deep)
			base.merge(shcl.Document.parse(deep))
			return base.to_canonical()

		if _at_depth(900, _merge_deep) != _merge_deep():
			raise SystemExit("deep merge from a deep caller diverged")
		# Wildcard resolution, validation through mounts and the unknown-field
		# sweep behind it, and generation through mounts each recursed one
		# frame per level, so from a caller 600 frames deep a document or
		# schema at the cap raised RecursionError. Each from that depth.
		ddoc = shcl.Document.parse(deep)
		stars = ".".join(["*"] * 512)
		if _at_depth(600, lambda: ddoc.count(stars)) != 1:
			raise SystemExit("deep wildcard resolve from a deep caller")
		vschema = shcl.Document.parse("field: l0\n\tinherits: f\nfragment: f\n\tfield: *\n\t\tinherits: f\n")
		if _at_depth(600, lambda: ddoc.validate(vschema)) != []:
			raise SystemExit("deep validate through mounts from a deep caller")
		wschema = shcl.Document.parse(f"field: {stars}\n")
		if _at_depth(600, lambda: ddoc.validate(wschema)) != []:
			raise SystemExit("deep wildcard validate from a deep caller")
		# 512 distinct fragments in one chain: a re-entered name cuts at once,
		# so only a chain that long reaches the depth cut.
		gs = ["field: a\n\tinherits: f0\n"]
		for k in range(512):
			gs.append(f"fragment: f{k}\n\tfield: b\n" + (f"\t\tinherits: f{k + 1}\n" if k < 511 else ""))
		gschema = shcl.Document.parse("".join(gs))
		gtext, gfaults = _at_depth(600, lambda: shcl.generate(gschema))
		if gfaults or gtext != shcl.generate(gschema)[0] or gtext.count("\n#") < 512:
			raise SystemExit("deep generate through mounts from a deep caller")
		# Bad encoding is unreadable too: the parser assumes well-formed text, so a
		# binary file loading clean would read back mangled and a later save would
		# write the mangled version over the original.
		with open(fpath, "wb") as fbin:
			fbin.write(b"a: 1\nb: \xff\xfe bad\n")
		fdoc, fst = shcl.Document.load_file(fpath)
		if fst != shcl.FileStatus.Unreadable or fdoc.to_canonical() != "":
			raise SystemExit(f"load_file bad encoding got {fst} {fdoc.to_canonical()!r}")
		# newline="" on the seeds below: text mode would translate \n on windows,
		# so the fixture would be feeding different bytes there than anywhere else.
		with open(fpath, "w", encoding="utf-8", newline="") as fh:
			fh.write("a: 1\n: broken\n")
		fdoc, fst = shcl.Document.load_file(fpath)
		if fst != shcl.FileStatus.HadErrors:
			raise SystemExit(f"load_file broken got {fst}")
		if fdoc.get_int("a", default=0) != 1:
			raise SystemExit("load_file broken read failed")
		with open(fpath, "w", encoding="utf-8", newline="") as fh:
			fh.write("a: 1\nb: x\n")
		fdoc, fst = shcl.Document.load_file(fpath)
		if fst != shcl.FileStatus.Clean:
			raise SystemExit(f"load_file clean got {fst}")
		# read_file is the load's read half on its own: the exact bytes, or
		# the status. The cap counts bytes, and a file exactly at it passes.
		# Same fixture in every runner.
		if shcl.read_file(fpath, 0) != ("a: 1\nb: x\n", shcl.FileStatus.Clean):
			raise SystemExit("read_file")
		if shcl.read_file(fpath, 10) != ("a: 1\nb: x\n", shcl.FileStatus.Clean):
			raise SystemExit("read_file at the cap")
		if shcl.read_file(fpath, 9) != (None, shcl.FileStatus.Unreadable):
			raise SystemExit("read_file past the cap")
		# A cap spelled as the type maximum used to overflow the over-cap probe
		# and read nothing. Same fixture in every runner.
		if shcl.read_file(fpath, sys.maxsize) != ("a: 1\nb: x\n", shcl.FileStatus.Clean):
			raise SystemExit("read_file at the largest cap")
		if shcl.read_file(os.path.join(td, "none.shcl"), 0) != (None, shcl.FileStatus.NotFound):
			raise SystemExit("read_file missing")
		if not fdoc.set_int("c", 3):
			raise SystemExit("set_int c failed")
		fdoc.save_file(fpath)
		fback, fst = shcl.Document.load_file(fpath)
		if fst != shcl.FileStatus.Clean or fback.to_canonical() != fdoc.to_canonical():
			raise SystemExit("save round-trip mismatch")
		# Creating a file and overwriting one are two different code paths in
		# the write - the create picks its own mode, the overwrite copies the
		# target's, and the publish step differs by platform (windows goes
		# through ReplaceFile, with a rename fallback). Both run everywhere: an
		# overwrite used to throw outright on windows here, which no POSIX-only
		# fixture could ever have caught. Same fixture in every runner.
		fresh = os.path.join(td, "fresh.shcl")
		ndoc = shcl.Document.parse("a: 1\n")
		ndoc.save_file(fresh)
		back, bst = shcl.Document.load_file(fresh)
		if bst != shcl.FileStatus.Clean or back.to_canonical() != "a: 1\n":
			raise SystemExit("new file did not round-trip")
		ndoc.save_file(fresh)
		back, bst = shcl.Document.load_file(fresh)
		if bst != shcl.FileStatus.Clean or back.to_canonical() != "a: 1\n":
			raise SystemExit("overwritten file did not round-trip")

		# A new file lands where an ordinary create lands - 0666 narrowed by the
		# umask - and an existing one keeps the mode it had. Neither is visible
		# on stdout, so no corpus case can see either, and neither is a windows
		# concept, so the mode half is POSIX-only.
		if os.name == "posix":
			probe = os.path.join(td, "probe")
			Path(probe).touch()
			mode_want = os.stat(probe).st_mode & 0o777
			born = os.path.join(td, "born.shcl")
			ndoc.save_file(born)
			mode = os.stat(born).st_mode & 0o777
			if mode != mode_want:
				raise SystemExit(f"new file mode {mode:o}, want {mode_want:o}")
			os.chmod(born, 0o640)
			ndoc.save_file(born)
			mode = os.stat(born).st_mode & 0o777
			if mode != 0o640:
				raise SystemExit(f"existing file mode {mode:o}, want 640")
			# setuid and setgid come over too: applying the mode before the
			# data lets the kernel clear them on the write.
			os.chmod(born, 0o6750)
			if os.stat(born).st_mode & 0o7777 == 0o6750:
				ndoc.save_file(born)
				if os.stat(born).st_mode & 0o7777 != 0o6750:
					raise SystemExit("save dropped a set-id bit")
			# A link to a file that is not there yet is written through like any
			# other link: the file appears where the link points and the link
			# stays a link. Same fixture in every POSIX runner.
			os.mkdir(os.path.join(td, "real"))
			link = os.path.join(td, "c.shcl")
			os.symlink("real/c.shcl", link)
			shcl.Document.parse("a: 1\n").save_file(link)
			if not os.path.islink(link):
				raise SystemExit("save replaced the dangling link")
			if _read(os.path.join(td, "real", "c.shcl")) != "a: 1\n":
				raise SystemExit("save did not create the file behind the link")
		if os.name != "nt":
			# Two links pointing at each other resolve to nothing, so the save
			# fails and says why. It must not "fix" the cycle by dropping a
			# regular file over one of the links. Same fixture in every POSIX
			# runner.
			cyc_a = os.path.join(td, "cyc-a.shcl")
			cyc_b = os.path.join(td, "cyc-b.shcl")
			os.symlink("cyc-b.shcl", cyc_a)
			os.symlink("cyc-a.shcl", cyc_b)
			try:
				shcl.Document.parse("a: 1\n").save_file(cyc_a)
				raise SystemExit("a symlink cycle saved without an error")
			except shcl.SaveFailed:
				pass
			for link in (cyc_a, cyc_b):
				if not os.path.islink(link):
					raise SystemExit("a symlink cycle was replaced by a regular file")
		if os.name == "nt":
			# A read-only target is rewritten, as it is on POSIX, and comes back
			# read-only; no temp file is left behind. Same fixture in every runner.
			ro = os.path.join(td, "ro.shcl")
			with open(ro, "w", encoding="utf-8", newline="") as fh:
				fh.write("a: 1\n")
			os.chmod(ro, stat.S_IREAD)
			shcl.Document.parse("a: 2\n").save_file(ro)
			if _read(ro) != "a: 2\n":
				raise SystemExit("read-only file not rewritten")
			if os.stat(ro).st_mode & stat.S_IWRITE:
				raise SystemExit("rewritten file did not come back read-only")
			if [n for n in os.listdir(td) if ".tmp" in n]:
				raise SystemExit("read-only rewrite left a temp file")
			os.chmod(ro, stat.S_IREAD | stat.S_IWRITE)
		# A document holding a lone surrogate has no UTF-8 spelling; the save
		# fails like any other failed write, and leaves no temp file behind.
		surdoc = shcl.Document.parse("a: 1\n")
		if not surdoc.set_string("s", "\udcff"):
			raise SystemExit("set_string refused a surrogate")
		try:
			surdoc.save_file(fpath)
			raise SystemExit("save_file wrote a lone surrogate")
		except shcl.SaveFailed:
			pass
		if [n for n in os.listdir(td) if ".tmp" in n]:
			raise SystemExit("failed save left a temp file")
		# A path is a str or a PathLike, never a descriptor: read_file(0) used
		# to read stdin and close it.
		bad_path: Any = 0
		for call in (
			lambda: shcl.read_file(bad_path),
			lambda: shcl.Document.load_file(bad_path),
			lambda: shcl.write_file_atomic(bad_path, "a: 1\n"),
		):
			try:
				call()
				raise SystemExit("an int was taken as a path")
			except TypeError:
				pass
		os.fstat(0)  # raises if the read closed stdin
		# Content-malformed lines are retained as trivia (lost_count 0, the
		# line survives a save); position-dependent drops count as lost and
		# make save_file refuse until the caller opts into save_file_lossy.
		# Same fixture in every runner.
		kept = shcl.Document.parse("a: 1\nsquare-miles 300\nb: 2\n")
		if kept.lost_count() != 0:
			raise SystemExit(f"kept lost_count got {kept.lost_count()}")
		if "square-miles 300\n" not in kept.to_canonical():
			raise SystemExit("retained line missing from canonical output")
		lostdoc = shcl.Document.parse("a:\n\tb: 1\n  c: 2\n")  # indent matches no level
		if lostdoc.lost_count() != 1:
			raise SystemExit(f"lost lost_count got {lostdoc.lost_count()}")
		kept.save_file(fpath)
		kback, _ = shcl.Document.load_file(fpath)
		if "square-miles 300\n" not in kback.to_canonical():
			raise SystemExit("retained line lost through save round-trip")
		try:
			lostdoc.save_file(fpath)
			raise SystemExit("save_file did not refuse a lossy save")
		except shcl.SaveRefused:
			pass
		lostdoc.save_file_lossy(fpath)
		# A refusal and a failed write are separate classes, not two spellings of
		# one message, and the gate answers before any i/o - so an unwritable path
		# still reports the refusal. Same fixture in every runner.
		bad = os.path.join(td, "nope", "t.shcl")
		try:
			kept.save_file(bad)
			raise SystemExit("save_file to an unwritable path reported success")
		except shcl.SaveRefused:
			raise SystemExit("a failed write reported as a refusal") from None
		except shcl.SaveFailed:
			pass
		try:
			lostdoc.save_file(bad)
			raise SystemExit("save_file did not refuse an unwritable path")
		except shcl.SaveRefused as e:
			if e.lost != 1:
				raise SystemExit(f"SaveRefused lost got {e.lost}") from None
	# A failed strict load hands back the document and names the first
	# failures in the message - the diagnostics are the point.
	try:
		shcl.Document.parse_with("ok: 1\n: nope\n", shcl.Strictness.Strict)
		raise SystemExit("strict load unexpectedly passed")
	except shcl.LoadError as le:
		if le.document is None or le.document.read_int("ok").value != 1:
			raise SystemExit("LoadError does not carry a usable document") from None
		if "; line " not in str(le):
			raise SystemExit("LoadError message lacks diagnostics: " + str(le)) from None
	# raw: the verbatim value span from the source line - not the display
	# join, which rewrites `{2,3}` to `{2, 3}`. Same fixture in every runner
	# whose read result exposes raw (the C read structs deliberately do not).
	rdoc = shcl.Document.parse('regex: ^\\d{2,3}$\nlist: a,  "b c"\n')
	if rdoc.read_string("regex").raw != "^\\d{2,3}$":
		raise SystemExit(f"raw fixture mismatch: {rdoc.read_string('regex').raw!r}")
	if rdoc.read_string_array("list").raw != 'a,  "b c"':
		raise SystemExit(f"raw fixture mismatch: {rdoc.read_string_array('list').raw!r}")
	# A written value has no source spelling; raw falls back to display. The
	# selector's escaped spelling must land on the existing instance.
	rdoc2 = shcl.Document.parse("who: 'q\"uote'\n")
	if not rdoc2.set_int('who["q\\"uote"].n', 5):
		raise SystemExit("escaped selector write failed")
	if rdoc2.count("who") != 1:
		raise SystemExit("escaped selector created a second instance")
	rr = rdoc2.read_int('who[\'q"uote\'].n')
	if (rr.value, rr.status) != (5, shcl.Status.Good):
		raise SystemExit("escaped selector read failed")
	if rr.raw != "5":
		raise SystemExit(f"written raw mismatch: {rr.raw!r}")
	# The get-tier value survives only on Good; Empty/BadType/NotFound all fall
	# back to the call-site default, so a real zero can't be faked. `_or` is the
	# cross-binding spelling for it, so a routine ported between two bindings
	# cannot keep the call name while changing which tier it lands on. Same
	# fixture in every runner.
	cdoc = shcl.Document.parse("a: 42\nb: not-a-number\ne:\narr: 1, 2, 3\nblk:\n\t```html\n\thi\n\t```\n")
	if cdoc.get_int_or("a", 9) != 42:
		raise SystemExit(f"get_int_or Good got {cdoc.get_int_or('a', 9)}")
	for p in ("b", "e", "missing"):
		if cdoc.get_int_or(p, 9) != 9:
			raise SystemExit(f"get_int_or({p!r}) did not fall back")
	if cdoc.get_int_array_or("arr", [7]) != [1, 2, 3]:
		raise SystemExit(f"get_int_array_or Good got {cdoc.get_int_array_or('arr', [7])}")
	if cdoc.get_int_array_or("missing", [7]) != [7]:
		raise SystemExit("get_int_array_or missing did not fall back")
	if cdoc.get_string_or("missing", "fb") != "fb":
		raise SystemExit("get_string_or missing did not fall back")
	# The raw block's info-string was the one typed read with no convenience
	# tier, so it alone forced a caller down to the status tier.
	if cdoc.get_raw_info("blk") != "html":
		raise SystemExit("get_raw_info did not read the info-string")
	if cdoc.get_raw_info_or("blk", "fb") != "html":
		raise SystemExit("get_raw_info_or Good did not read through")
	if cdoc.get_raw_info_or("missing", "fb") != "fb":
		raise SystemExit("get_raw_info_or missing did not fall back")
	# ok() and the convenience tier deliberately disagree on an explicitly
	# emptied field: one asks whether the author spoke for it, the other whether
	# there is a usable value.
	if not cdoc.read_int("e").ok():
		raise SystemExit("an emptied field is not ok()")
	if cdoc.read_int("missing").ok():
		raise SystemExit("a missing field is ok()")
	# The body's shared indent survives a reload (the closing fence's indent
	# is what comes off), the info-string is stored as a fence line reads it
	# back, and an info with a line break or an unquoted `#` has no spelling
	# and fails the write. Same fixture in every runner.
	rawdoc = shcl.Document.new()
	if not rawdoc.set_raw("q", "  a\n  b", " sql "):
		raise SystemExit("set_raw failed")
	rawback = shcl.Document.parse(rawdoc.to_canonical())
	if rawback.get_raw("q") != "  a\n  b":
		raise SystemExit(f"set_raw indent got {rawback.get_raw('q')!r}")
	if rawback.read_raw_info("q").value != "sql":
		raise SystemExit("set_raw info not trimmed")
	if rawdoc.set_raw("q", "x", "a\nb") or rawdoc.set_raw("q", "x", "a\rb"):
		raise SystemExit("set_raw accepted an info with a line break")
	if rawdoc.set_raw("q", "x", "a # b"):
		raise SystemExit("set_raw accepted an info with an unquoted #")
	if not rawdoc.set_raw("q", "  a\n  b", '"a # b"'):
		raise SystemExit("set_raw refused a quoted #")
	rawback = shcl.Document.parse(rawdoc.to_canonical())
	if rawback.get_raw("q") != "  a\n  b":
		raise SystemExit("refused set_raw changed the document")
	if rawback.read_raw_info("q").value != '"a # b"':
		raise SystemExit(f"set_raw quoted info got {rawback.read_raw_info('q').value!r}")
	# A body line ending in CR has no fence spelling: the load takes the whole
	# trailing CR run off every line, so it is refused rather than lost. A CR
	# mid-line is content and still round-trips.
	if rawdoc.set_raw("q", "a\r\nb", "") or rawdoc.set_raw("q", "\r", ""):
		raise SystemExit("set_raw accepted a body with a line-ending CR")
	if not rawdoc.set_raw("q", "a\rb", ""):
		raise SystemExit("set_raw refused a body with a mid-line CR")
	rawback = shcl.Document.parse(rawdoc.to_canonical())
	if rawback.get_raw("q") != "a\rb":
		raise SystemExit(f"set_raw mid-line CR got {rawback.get_raw('q')!r}")
	# Typed setters take exactly their type and raise a TypeError naming the
	# setter for anything else, rather than writing text the reader of that
	# type would call bad-type (set_int of 3.5 wrote `3.5`). bool is an int
	# in Python, so the int and float gates carve it out; an int is a float.
	tdoc = shcl.Document.parse("a: 1\n")
	for setter, args in (
		("set_int", ("a", 3.5)),
		("set_int", ("a", True)),
		("set_int", ("a", "3")),
		("set_float", ("a", True)),
		("set_float", ("a", "1.5")),
		("set_bool", ("a", 1)),
		("set_string", ("a", 5)),
		("set_datetime", ("a", "2026-01-01")),
		("set_int_array", ("a", [1, 2.5])),
		("set_float_array", ("a", [1.5, False])),
		("set_bool_array", ("a", [True, 0])),
		("set_string_array", ("a", ["x", None])),
		("set_datetime_array", ("a", ["2026-01-01"])),
		("set_int_default", ("a", 1.0)),
		("set_float_default", ("a", "1")),
		("set_bool_default", ("a", "true")),
		("set_string_default", ("a", 1)),
		("set_datetime_default", ("a", 1)),
		("set_int_array_default", ("a", [True])),
		("set_float_array_default", ("a", ["1"])),
		("set_bool_array_default", ("a", [1])),
		("set_string_array_default", ("a", [1])),
		("set_datetime_array_default", ("a", [1])),
	):
		try:
			getattr(tdoc, setter)(*args)
			raise SystemExit(f"{setter} accepted {args[1]!r}")
		except TypeError as e:
			if not str(e).startswith(setter + ":"):
				raise SystemExit(f"{setter} TypeError does not name it: {e}") from None
	if tdoc.to_canonical() != "a: 1\n":
		raise SystemExit("a refused setter changed the document")
	if not tdoc.set_float("f", 3) or tdoc.read_float("f").value != 3.0:
		raise SystemExit("set_float refused an int")
	if not tdoc.set_float_array_default("g", [1, 2.5]) or tdoc.read_float_array("g").value != [1.0, 2.5]:
		raise SystemExit("set_float_array_default refused an int element")
	# Python-only: an int is written as the float it converts to, the f64 a
	# caller of the other bindings would hold - not its exact digits. One past
	# the float range has no f64 but inf, which the reader refuses, so it
	# fails the write like any other infinity.
	if not tdoc.set_float("h", 9007199254740993) or tdoc.read_string("h").value != "9007199254740992":
		raise SystemExit(f"set_float of a wide int wrote {tdoc.read_string('h').value!r}")
	if tdoc.set_float_array("i", [10 ** 400, -(10 ** 400)]) or tdoc.exists("i"):
		raise SystemExit("set_float_array past the float range bound a value")

	# Three hot-path shortcuts this binding carries because it is the slow one.
	# Each is asserted structurally, by what the code does rather than by a
	# clock: a wall-time threshold on a constant-factor win either flakes or
	# never fires. Their combined effect was parse-plus-emit 1.05 s -> 0.87 s on
	# a 1.7 MB document, output byte-identical.
	class _CountingList(list):
		iters = 0

		def __iter__(self):
			_CountingList.iters += 1
			return list.__iter__(self)

	pdoc = shcl.Document.parse("one: a\ntwo: a, b\n")
	for pnode in pdoc.arena:
		if pnode.value.kind == "cell":
			pnode.value.els = _CountingList(pnode.value.els)
	for want_len, want_iters in ((1, 0), (2, 2)):
		_CountingList.iters = 0
		for pnode in pdoc.arena:
			if pnode.value.kind == "cell" and len(pnode.value.els) == want_len:
				pnode.value.display()
				shcl._merge_key(pnode.name, pnode.value)
		if _CountingList.iters != want_iters:
			raise SystemExit(
				f"display/merge-key walked a {want_len}-element cell {_CountingList.iters} time(s), want {want_iters}"
			)

	# The source-attach guard settles the common line by identity, so neither
	# key is built. Anything above zero means it went back to comparing a value
	# with itself the long way.
	key_calls = [0]
	real_value_key = shcl._value_key
	try:
		shcl._value_key = lambda v: (key_calls.__setitem__(0, key_calls[0] + 1), real_value_key(v))[1]
		shcl.Document.parse("".join(f"k{i}: v{i}\n" for i in range(200)))
	finally:
		shcl._value_key = real_value_key
	if key_calls[0]:
		raise SystemExit(f"the source-attach guard built {key_calls[0]} value key(s) on 200 plain lines")

	# The path scanner's two helpers are module level. Defined inside it they
	# would be rebuilt, with a fresh cell each, once per document line.
	inner = [c.co_name for c in shcl._scan_path_ex.__code__.co_consts if isinstance(c, types.CodeType)]
	if inner:
		raise SystemExit(f"the path scanner rebuilds {inner} on every call")

	print(f"conformance: {len(cases)} case(s) pass")
	return 0


if __name__ == "__main__":
	sys.exit(main())
