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
		cases.append({
			"name": name,
			"input": _read(inp),
			"expected": _read(os.path.join(d, "expected.shcl")),
			"reads": _read(os.path.join(d, "reads.tsv")),
		})
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


def main():
	fails = []
	cases = load_cases()

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
