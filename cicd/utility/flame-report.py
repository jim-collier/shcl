#!/usr/bin/env python3

##	Purpose: Summarize the newest shcl profiler flamegraph - self-time hot
##		spots, inclusive call buckets, and the caller chain of the top leaves -
##		by parsing the pprof/inferno SVG (its fg:w attribute = raw sample counts).
##		Runs two ways: plain (print the report, meant to run every cicd run) and
##		--check (print only when the newest flamegraph is newer than the one last
##		recorded in a local marker, then record it - meant for session startup so
##		a look is a no-op until there is something new to read).
##	History: At bottom of script.

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT


import argparse
import bisect
import html
import os
import re
import sys

STEP      = 16              # flamegraph row height in the SVG, px (a child sits at parent_y - STEP)
SELF_TOP  = 22             	# self-time leaders to list
INCL_TOP  = 14             	# inclusive-time buckets to list
CHAIN     = 4              	# leaves whose caller chain we walk up to the root
SEEN_FILE = ".flame-seen"  	# marker basename, kept in the profiling dir - outside the git repo

NAME_RE   = re.compile(r"flame_(\d{8}-\d{6})_\w+\.svg$")
FRAME_RE  = re.compile(r"<title>(.*?)</title><rect ([^>]*?)/>", re.S)
ATTR_RES  = {key: re.compile(re.escape(key) + r'="([\d.]+)"') for key in ("y", "fg:x", "fg:w")}
SAMPLES_RE = re.compile(r"\s*\(\d[\d,]* samples.*$")


def fSkip(msg):
	##	2 = environmental skip (no dir / unparseable) - non-fatal, matches the
	##	cicd profiler stage which treats such things as a warning, not a failure.
	sys.stderr.write(f"flame-report: {msg}\n")
	sys.exit(2)


def fNewest(pdir):
	##	Sort on the timestamp, NOT the role suffix: GFS rotation retags the role
	##	(frequent -> latest -> hour/day/...) as time passes, but the timestamp in
	##	the name is stable.
	best = None
	for name in os.listdir(pdir):
		m = NAME_RE.match(name)
		if m and (best is None or m.group(1) > best[0]):
			best = (m.group(1), name)
	return best


def fAttr(attrs, key):
	vm = ATTR_RES[key].search(attrs)
	return float(vm.group(1)) if vm else None


def fParse(path):
	with open(path, encoding="utf-8") as fh:
		text = fh.read()
	m = re.search(r'total_samples="(\d+)"', text)
	total = int(m.group(1)) if m else 0
	frames = []                                      # each: (name, x, y, w) in raw samples
	for fm in FRAME_RE.finditer(text):
		attrs = fm.group(2)
		y, x, w = fAttr(attrs, "y"), fAttr(attrs, "fg:x"), fAttr(attrs, "fg:w")
		if None in (y, x, w):
			continue
		name = SAMPLES_RE.sub("", html.unescape(fm.group(1)))
		frames.append((name, x, y, w))
	if not total or not frames:
		fSkip(f"could not parse a flamegraph out of {path}")
	return total, frames


def fAnalyze(total, frames, top):
	##	Rows keyed by y and sorted by x, with the x list beside each, so a
	##	frame's parent or children are a bisect and a short walk rather than a
	##	scan of the whole row for every frame.
	rows = {}
	for fr in frames:
		rows.setdefault(fr[2], []).append(fr)
	rowXs = {}
	for y, row in rows.items():
		row.sort(key=lambda fr: fr[1])
		rowXs[y] = [fr[1] for fr in row]
	eps = 1e-6

	def kids(fr):
		_, x, y, w = fr
		row = rows.get(y - STEP)
		if not row:
			return []
		out = []
		for c in row[bisect.bisect_left(rowXs[y - STEP], x - eps):]:
			if c[1] + c[3] > x + w + eps:
				break
			out.append(c)
		return out

	def parent(fr):
		_, x, y, w = fr
		row = rows.get(y + STEP)
		if not row:
			return None
		i = bisect.bisect_right(rowXs[y + STEP], x + eps) - 1
		if i >= 0 and row[i][1] + row[i][3] >= x + w - eps:
			return row[i]
		return None

	selfOf = {fr: fr[3] - sum(c[3] for c in kids(fr)) for fr in frames}

	selfBy, inclBy, byName = {}, {}, {}
	parse = emit = reads = other = 0.0
	for fr in frames:
		name = fr[0]
		byName.setdefault(name, []).append(fr)
		inclBy[name] = inclBy.get(name, 0.0) + fr[3]
		s = selfOf[fr]
		selfBy[name] = selfBy.get(name, 0.0) + s
		if s <= 0:
			continue
		anc, cur = [], fr                            # ancestor names (self up to root)
		while cur:
			anc.append(cur[0])
			cur = parent(cur)
		##	Buckets keyed to shcl's hot subsystems (the cicd workload is `fmt`, so
		##	parse + emit dominate; reads shows up if the workload ever adds gets).
		if any("Parser::parse" in a or "Document::parse" in a for a in anc):
			parse += s
		elif any("to_canonical" in a or "emit_node" in a for a in anc):
			emit += s
		elif any("scan_path" in a or "::read_" in a for a in anc):
			reads += s
		else:
			other += s

	def pct(v):
		return f"{v / total * 100:5.1f}%"

	print("attribution (self-time):")
	print(f"  parse (tokenize/merge/diags) .: {pct(parse)}")
	print(f"  emit (canonical formatter) ...: {pct(emit)}")
	print(f"  reads (lookup/coercion) ......: {pct(reads)}")
	print(f"  other ........................: {pct(other)}")
	print()

	print("top self-time (where CPU actually burns):")
	for name, v in sorted(selfBy.items(), key=lambda kv: -kv[1])[:top]:
		if v < 1:
			break
		print(f"  {pct(v)} {int(v):4d}  {name}")
	print()

	print("top inclusive (call buckets):")
	for name, v in sorted(inclBy.items(), key=lambda kv: -kv[1])[:INCL_TOP]:
		print(f"  {pct(v)} {int(v):4d}  {name}")
	print()

	print(f"caller chains of the top {CHAIN} leaves:")
	for name, v in sorted(selfBy.items(), key=lambda kv: -kv[1])[:CHAIN]:
		fr = max(byName[name], key=selfOf.__getitem__)
		print(f"  {name}  ({pct(v)} self)")
		cur, depth = parent(fr), 0
		while cur and depth < 12:
			print(f"      {cur[0]}")
			if cur[0] == "all":
				break
			cur, depth = parent(cur), depth + 1


def main():
	here = os.path.dirname(os.path.abspath(__file__))
	default_dir = os.path.normpath(os.path.join(here, "..", "artifacts", "profiling"))

	ap = argparse.ArgumentParser(description="Summarize the newest shcl profiler flamegraph.")
	ap.add_argument("--dir", default=default_dir, help="profiling directory (default: %(default)s)")
	ap.add_argument("--file", help="analyze this SVG instead of the newest in --dir")
	ap.add_argument("--top", type=int, default=SELF_TOP, help="self-time leaders to list")
	ap.add_argument("--check", action="store_true",
	                help="startup gate: print only if newer than the local marker, then record it")
	ap.add_argument("--force", action="store_true", help="with --check, report even if already seen")
	ap.add_argument("--no-mark", action="store_true", help="with --check, do not update the marker")
	a = ap.parse_args()

	if a.file:
		path = a.file
		if not os.path.isfile(path):
			fSkip(f"no such file: {path}")
		name = os.path.basename(path)
		m = NAME_RE.match(name)
		ts = m.group(1) if m else ""
	else:
		if not os.path.isdir(a.dir):
			fSkip(f"no profiling dir: {a.dir}")
		nb = fNewest(a.dir)
		if not nb:
			fSkip(f"no flamegraphs in {a.dir}")
		ts, name = nb
		path = os.path.join(a.dir, name)

	marker = os.path.join(a.dir, SEEN_FILE)
	if a.check and not a.force:
		seen = ""
		try:
			with open(marker) as fh:
				seen = fh.read().strip()
		except OSError:
			pass
		if ts and seen and ts <= seen:
			print(f"SEEN {name}  (nothing newer than {seen})")
			return

	total, frames = fParse(path)
	print(f"{'NEW' if a.check else 'FLAME'} {name}  ({ts or 'n/a'}, {total} samples)")
	print()
	fAnalyze(total, frames, a.top)

	if a.check and not a.no_mark and ts:
		try:
			with open(marker, "w") as fh:
				fh.write(ts + "\n")
		except OSError as e:
			sys.stderr.write(f"flame-report: could not write marker: {e}\n")


if __name__ == "__main__":
	main()


##	History:
##		- 20260712: Created from the SilkTerm sibling; attribution buckets redone for shcl (parse / emit / reads).
##		- 20260829: Regexes compiled once; rows indexed and bisected instead of scanned per frame.
