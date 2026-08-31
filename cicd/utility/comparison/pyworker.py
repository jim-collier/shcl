#!/usr/bin/env python3

"""Python half of the format comparison.

Speaks the same key=value worker protocol the Rust half does, so the orchestrator
does not care which language a measurement came from: one process per
measurement, best of N runs, and peak resident memory read from the kernel's own
high-water mark.

Its job is to answer the question the Rust tier cannot: how much of SHCL's cost
is the format and how much is one implementation of it. So the libraries here are
the ones a Python program would really import - the standard library first, and
whatever the ecosystem reached for where the standard library has no answer.

Most of what Python reaches for is a C extension wearing a Python name - json,
ElementTree and PyYAML's CSafeLoader all are - while this project's own binding
is pure Python by design. So the row worth reading here is tomllib, which is also
pure Python: that pair is the only like-for-like comparison in the tier, and the
notes on each entry say which side it is on.

Anything not installed is reported as unavailable rather than failing the run;
the orchestrator lists what it skipped.
"""

##	Copyright © 2026 Bubbles (ID: XଌฅრX۳ᛟԃლፀƅꓩหδლც)
##	Licensed under The MIT License (MIT). Full text at:
##		https://mit-license.org/
##	SPDX-License-Identifier: MIT

from __future__ import annotations

import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any, Optional

REPO_PY = Path(__file__).absolute().parents[3] / "source" / "python"

# What a loader hands back: (version, parse, emit) - emit None when the library
# cannot write. Optional[] rather than | None: this alias is evaluated at import.
Loader = tuple[str, Callable[[str], Any], Optional[Callable[[Any], str]]]


def vmhwm() -> int:
	"""Kernel peak resident set for this process, in bytes."""
	try:
		with open("/proc/self/status", encoding="utf-8") as f:
			for line in f:
				if line.startswith("VmHWM:"):
					return int(line.split()[1]) * 1024
	except OSError:
		pass
	return 0


#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
# One loader per entry. Each returns (version, parse, emit) - emit None when the
# library cannot write - and raises ImportError when it is not installed.
#••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

def load_shcl() -> Loader:
	# Every call used to push the same entry again, and the listing calls every
	# loader, so the search path grew a duplicate each time.
	if str(REPO_PY) not in sys.path:
		sys.path.insert(0, str(REPO_PY))
	import shcl
	return "working tree", shcl.Document.parse, lambda d: d.to_canonical()


def load_json() -> Loader:
	import json
	# ensure_ascii off, or every non-ASCII character comes back as an escape and
	# the emitted document stops being the one that was read.
	return getattr(json, "__version__", "stdlib"), json.loads, lambda o: json.dumps(o, indent=2, ensure_ascii=False)


def load_yaml() -> Loader:
	import yaml
	# libyaml through CSafeLoader where it is built, which is what anyone who has
	# noticed PyYAML's speed uses; the pure-Python loader is the fallback.
	loader = getattr(yaml, "CSafeLoader", yaml.SafeLoader)
	dumper = getattr(yaml, "CSafeDumper", yaml.SafeDumper)
	ver = yaml.__version__ + ("" if hasattr(yaml, "CSafeLoader") else " (no libyaml)")
	# sort_keys off for the same reason JSON gets preserve_order in the Rust tier.
	return (ver,
		lambda s: yaml.load(s, Loader=loader),
		lambda o: yaml.dump(o, Dumper=dumper, allow_unicode=True, default_flow_style=False, sort_keys=False))


def load_toml() -> Loader:
	import tomllib
	try:
		import tomli_w
		return "stdlib + tomli_w " + getattr(tomli_w, "__version__", "?"), tomllib.loads, tomli_w.dumps
	except ImportError:
		return "stdlib", tomllib.loads, None   # tomllib reads only, by design


def load_toml_edit() -> Loader:
	import tomlkit
	return getattr(tomlkit, "__version__", "?"), tomlkit.parse, tomlkit.dumps


def load_xml() -> Loader:
	import xml.etree.ElementTree as ET
	return "stdlib", ET.fromstring, lambda e: ET.tostring(e, encoding="unicode")


def load_xml_lxml() -> Loader:
	from lxml import etree
	# huge_tree lifts libxml2's built-in size ceilings, which a document of the
	# size this tool generates walks straight into - the parse fails outright
	# otherwise. Turning them off is the fair setting rather than a thumb on the
	# scale: they guard against hostile input, and this input is ours.
	parser = etree.XMLParser(huge_tree=True)
	return (etree.__version__,
		(lambda s: etree.fromstring(s.encode("utf-8"), parser)),
		(lambda e: etree.tostring(e, encoding="unicode")))


ENTRIES = {
	"shcl":      (load_shcl,      "shcl", "shcl (this repo)",      "layout+comments",
		"pure python - no C anywhere, which is the row to read tomllib against"),
	"json":      (load_json,      "json", "json (stdlib)",         "data",
		"C accelerated - the stdlib module is a thin shell over _json"),
	"yaml":      (load_yaml,      "yaml", "PyYAML",                "data",
		"C accelerated where libyaml is built, which CSafeLoader uses"),
	"toml":      (load_toml,      "toml", "tomllib (stdlib)",      "data",
		"pure python, and read-only by design; tomli_w supplies a writer when installed"),
	"toml-edit": (load_toml_edit, "toml", "tomlkit",               "layout+comments",
		"pure python; the answer to toml_edit - keeps the file as written"),
	"xml":       (load_xml,       "xml",  "xml.etree.ElementTree", "data",
		"C accelerated - the stdlib module is a shell over _elementtree/expat"),
	"xml-lxml":  (load_xml_lxml,  "xml",  "lxml",                  "data",
		"C - a binding to libxml2, with huge_tree on so its size ceilings do not refuse the document"),
}


def emit_list() -> None:
	"""key|format|library|retains|version|note for every entry that can be imported."""
	for key, (loader, fmt, lib, retains, note) in ENTRIES.items():
		try:
			version, _, _ = loader()
		# Not just ImportError: a loader that fails any other way is one entry
		# unavailable, not the whole listing gone.
		except Exception as e:
			print(f"unavailable|{key}|{type(e).__name__}: {e}")
			continue
		print(f"available|{key}|{fmt}|{lib}|{retains}|{version}|{note}")


def run(key: str, path: str, iters: int) -> None:
	loader = ENTRIES[key][0]
	try:
		_, parse, emit = loader()
	except ImportError as e:
		print(f"skipped={e}")
		return
	src = Path(path).read_text(encoding="utf-8")

	base = vmhwm()
	try:
		doc = parse(src)
	except Exception as e:                                    # any parse failure is a result
		print(f"failed={type(e).__name__}: {e}".replace("\n", " "))
		return
	rss = vmhwm()
	del doc

	best = None
	for _ in range(iters):
		t = time.perf_counter()
		doc = parse(src)
		secs = time.perf_counter() - t
		if best is None or secs < best:
			best = secs
		del doc

	doc = parse(src)
	emit_best = None
	out = ""
	if emit is not None:
		for _ in range(iters):
			t = time.perf_counter()
			out = emit(doc)
			secs = time.perf_counter() - t
			if emit_best is None or secs < emit_best:
				emit_best = secs

	print(f"parse-secs={best:.6f}")
	if emit_best is not None:
		print(f"emit-secs={emit_best:.6f}")
	print(f"emit-bytes={len(out.encode('utf-8'))}")
	print(f"roundtrip={'true' if out and out == src else 'false'}")
	print("scalars=0")
	print(f"rss-bytes={rss}")
	print(f"base-rss-bytes={base}")


def main() -> int:
	args = sys.argv[1:]
	if args and args[0] == "--list":
		emit_list()
		return 0
	if len(args) != 3 or args[0] not in ENTRIES:
		print("usage: pyworker.py --list | KEY FILE ITERS", file=sys.stderr)
		return 2
	try:
		iters = int(args[2])
	except ValueError:
		iters = 0
	# Zero timed rounds leaves the best time unset, which used to raise while
	# formatting it - two lines below a usage line that says what ITERS is.
	if iters < 1:
		print("usage: pyworker.py --list | KEY FILE ITERS (ITERS is a positive integer)", file=sys.stderr)
		return 2
	run(args[0], args[1], iters)
	return 0


if __name__ == "__main__":
	sys.exit(main())
