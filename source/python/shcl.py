# SPDX-License-Identifier: MIT
# Copyright © 2026 Jim Collier

"""SHCL binding for Python: parser, accessor, writer/formatter.

Single file on purpose - the drop-in story is "copy this file into your tree".
Behaviour tracks the Rust reference (source/rust/src/lib.rs) byte for byte; the
conformance corpus in project/conformance/ pins every behaviour here, and the
cicd cross-binding check compares this against the reference on every run.
"""

import math
from decimal import Decimal
from enum import Enum

# ---------------------------------------------------------------------------
# Public surface
# ---------------------------------------------------------------------------


class Strictness(Enum):
	Loose = 1
	Standard = 2
	Strict = 3

	@staticmethod
	def from_arg(s):
		"""Accepts the CLI spellings: loose|standard|strict or 1|2|3."""
		return {
			"loose": Strictness.Loose, "1": Strictness.Loose,
			"standard": Strictness.Standard, "2": Strictness.Standard,
			"strict": Strictness.Strict, "3": Strictness.Strict,
		}.get(_ascii_lower(s))


class Severity(Enum):
	# .name renders "Error"/"Hint" - that spelling is the `check` output contract.
	Error = 1
	Hint = 2


class Status(Enum):
	# Values are the CLI exit codes, so status_code is just Status.value.
	Good = 0
	Empty = 2
	NotFound = 3
	BadType = 4
	Multiple = 5


class Diagnostic:
	__slots__ = ("line", "severity", "message")

	def __init__(self, line, severity, message):
		self.line = line          # 1-based
		self.severity = severity
		self.message = message


class Read:
	"""Value plus status plus the original raw text (when the path resolved)."""
	__slots__ = ("value", "status", "raw")

	def __init__(self, value, status, raw):
		self.value = value
		self.status = status
		self.raw = raw

	def ok(self):
		return self.status in (Status.Good, Status.Empty)


class LoadError(Exception):
	def __init__(self, diagnostics):
		self.diagnostics = diagnostics
		n = sum(1 for d in diagnostics if d.severity == Severity.Error)
		super().__init__("strict load failed: {} error diagnostic(s)".format(n))


class ShclDateTime:
	"""Local (floating) date/time unless a zone suffix was present. Fields mirror
	what was written: a date-only value has no time, and vice versa."""
	__slots__ = ("date", "time", "frac", "zone")

	def __init__(self, date=None, time=None, frac=None, zone=None):
		self.date = date          # None | (year, month, day)
		self.time = time          # None | (hour, minute, seconds-if-written)
		self.frac = frac          # None | fractional-second digits as typed
		self.zone = zone          # None | ("utc", None) | ("offset", minutes)

	def __str__(self):
		out = []
		if self.date is not None:
			y, m, d = self.date
			out.append("{:04d}-{:02d}-{:02d}".format(y, m, d))
			if self.time is not None:
				out.append("T")
		if self.time is not None:
			h, mi, s = self.time
			out.append("{:02d}:{:02d}".format(h, mi))
			if s is not None:
				out.append(":{:02d}".format(s))
			if self.frac is not None:
				out.append("." + self.frac)
		if self.zone is not None:
			if self.zone[0] == "utc":
				out.append("Z")
			else:
				off = self.zone[1]
				sign = "-" if off < 0 else "+"
				a = abs(off)
				out.append("{}{:02d}:{:02d}".format(sign, a // 60, a % 60))
		return "".join(out)


def format_float(v):
	"""Float -> string, matching the reference: positional, shortest round-trip,
	never scientific. inf/NaN spelled as the reference spells them."""
	if v != v:
		return "NaN"
	if v == math.inf:
		return "inf"
	if v == -math.inf:
		return "-inf"
	return format(Decimal(repr(v)).normalize(), "f")


# ---------------------------------------------------------------------------
# In-memory model
# ---------------------------------------------------------------------------
# One rule covers everything: a node is (field-name, value, children); nodes
# merge when (name, value) matches; empty values merge into the wrapper node.


class _Element:
	__slots__ = ("text", "quoted")   # text: quote-stripped, escapes NOT applied

	def __init__(self, text, quoted):
		self.text = text
		self.quoted = quoted


class _Value:
	# kind: "empty" | "cell" (els) | "raw" (content/info/fence_char/fence_len)
	__slots__ = ("kind", "els", "content", "info", "fence_char", "fence_len")

	def __init__(self, kind):
		self.kind = kind
		self.els = None
		self.content = None
		self.info = None
		self.fence_char = None
		self.fence_len = None

	def is_empty(self):
		return self.kind == "empty"

	def key(self):
		"""Merge key: nodes with equal (name, key) collapse into one."""
		if self.kind == "empty":
			return "e"
		if self.kind == "cell":
			parts = ["c:"]
			for e in self.els:
				parts.append("\x00")
				parts.append(e.text)
			return "".join(parts)
		return "r:" + self.content

	def display(self):
		"""Human/display form; also what selectors match against (case-sensitive)."""
		if self.kind == "empty":
			return ""
		if self.kind == "cell":
			return ", ".join(e.text for e in self.els)
		return self.content


def _empty():
	return _Value("empty")


def _cell(els):
	v = _Value("cell")
	v.els = els
	return v


def _raw(content, info, fence_char, fence_len):
	v = _Value("raw")
	v.content = content
	v.info = info
	v.fence_char = fence_char
	v.fence_len = fence_len
	return v


class _Node:
	__slots__ = ("name", "value", "children", "parent", "line", "star_list")

	def __init__(self, name, value, parent, line):
		self.name = name          # ASCII-folded to lower; non-ASCII never folds
		self.value = value
		self.children = []
		self.parent = parent
		self.line = line
		self.star_list = False    # value built from stacked "* " lines


ROOT = 0


# ---------------------------------------------------------------------------
# Lexical helpers
# ---------------------------------------------------------------------------

# White_Space set, matching Rust char::is_whitespace exactly (so trims agree on
# exotic input - e.g. U+001C-1F are NOT whitespace here, unlike Python's default).
_WS = (
	"\t\n\x0b\x0c\r\x20\x85\xa0 "
	"           "
	"    　"
)
_WS_SET = set(_WS)

_ASCII_UPPER_MAP = {ord(c): ord(c) + 32 for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"}


def _ascii_lower(s):
	# Folds A-Z only; non-ASCII passes through untouched (matches to_ascii_lowercase).
	return s.translate(_ASCII_UPPER_MAP)


def _trim(s):
	return s.strip(_WS)


def _trim_end(s):
	return s.rstrip(_WS)


def _split_ws(s):
	# Like Rust split_whitespace: split on White_Space runs, no empty tokens.
	out = []
	cur = []
	for c in s:
		if c in _WS_SET:
			if cur:
				out.append("".join(cur))
				cur = []
		else:
			cur.append(c)
	if cur:
		out.append("".join(cur))
	return out


def _is_ascii_digit(c):
	return "0" <= c <= "9"


def _fold_name(s):
	return _ascii_lower(s)


def _is_bare_name_char(c):
	return (c.isascii() and c.isalnum()) or c == "-" or c == "_"


def _strip_comment(s):
	"""Strip an unquoted trailing comment. A `\\` shields the next char throughout."""
	in_quote = None
	i = 0
	n = len(s)
	while i < n:
		c = s[i]
		if c == "\\":
			i += 2
			continue
		if in_quote is not None:
			if c == in_quote:
				in_quote = None
		elif c == '"' or c == "'":
			in_quote = c
		elif c == "#":
			return s[:i]
		i += 1
	return s


def _split_unquoted_commas(s):
	"""Split on unquoted commas; `\\` shields the next char."""
	parts = []
	in_quote = None
	start = 0
	i = 0
	n = len(s)
	while i < n:
		c = s[i]
		if c == "\\":
			i += 2
			continue
		if in_quote is not None:
			if c == in_quote:
				in_quote = None
		elif c == '"' or c == "'":
			in_quote = c
		elif c == ",":
			parts.append(s[start:i])
			start = i + 1
		i += 1
	parts.append(s[start:])
	return parts


def _normalize_dangling_backslash(t):
	"""A dangling trailing backslash would swallow the following separator on
	re-emit; store the doubled spelling instead (identical on string read)."""
	run = 0
	for c in reversed(t):
		if c == "\\":
			run += 1
		else:
			break
	if run % 2 == 1:
		t += "\\"
	return t


def _parse_element(piece):
	"""Trim, then strip one matching outer quote pair if present. Unquoted empty
	slots return None (dropped, never an error)."""
	t = _trim(piece)
	if not t:
		return None
	first = t[0]
	if (first == '"' or first == "'") and len(t) >= 2 and t[-1] == first:
		# The closing quote must not itself be escaped (`"a\"` is not closed).
		esc = False
		for c in t[1:-1]:
			esc = (c == "\\") and not esc
		if not esc:
			return _Element(t[1:-1], True)
	return _Element(_normalize_dangling_backslash(t), False)


def _parse_cell(text):
	els = []
	for piece in _split_unquoted_commas(text):
		e = _parse_element(piece)
		if e is not None:
			els.append(e)
	return _cell(els) if els else _empty()


def _apply_escapes(s):
	"""Escape processing (string reads): \\t \\n \\\\ \\" \\'; unknown escapes stay literal."""
	out = []
	it = iter(s)
	for c in it:
		if c != "\\":
			out.append(c)
			continue
		nxt = next(it, None)
		if nxt == "t":
			out.append("\t")
		elif nxt == "n":
			out.append("\n")
		elif nxt == "\\":
			out.append("\\")
		elif nxt == '"':
			out.append('"')
		elif nxt == "'":
			out.append("'")
		elif nxt is None:
			out.append("\\")
		else:
			out.append("\\")
			out.append(nxt)
	return "".join(out)


def _fence_open(rest):
	"""Opening fence: a run of >=3 backticks or tildes, then an optional info-string."""
	if not rest:
		return None
	first = rest[0]
	if first != "`" and first != "~":
		return None
	run = 0
	for c in rest:
		if c == first:
			run += 1
		else:
			break
	if run < 3:
		return None
	return (first, run, _trim(rest[run:]))


def _is_fence_close(line, ch, min_len):
	t = _trim(line)
	return len(t) >= min_len and len(t) > 0 and all(c == ch for c in t)


# ---------------------------------------------------------------------------
# Path scanner (shared by file lines and accessor queries)
# ---------------------------------------------------------------------------
# Selector is a tuple: ("val", str) | ("idx", int) | ("wild", None).


class _Segment:
	__slots__ = ("name", "selector")   # name folded; selector None or tuple

	def __init__(self, name, selector):
		self.name = name
		self.selector = selector


class _PathError(Exception):
	pass


def _parse_uint(s):
	# Rust usize::from_str: an optional leading '+', then ASCII digits, no underscores.
	if not s:
		return None
	body = s[1:] if s[0] == "+" else s
	if not body or not all(_is_ascii_digit(c) for c in body):
		return None
	n = int(body)
	if n > 2 ** 64 - 1:
		return None
	return n


def _scan_path(inp):
	"""Scan `a . b : [sel] . c : value`. Whitespace around dots/colons/brackets is
	insignificant. A colon is a selector colon only when the next non-ws char is
	`[`; otherwise it separates the value. Raises _PathError on genuinely ambiguous
	input, which the caller skips with a diagnostic. Returns (segments, value_text)
	where value_text is None (no colon) or the trimmed text after the colon."""
	chars = inp
	n = len(chars)

	def skip_ws(p):
		while p < n and (chars[p] == " " or chars[p] == "\t"):
			p += 1
		return p

	def read_quoted(p):
		q = chars[p]
		p += 1
		out = []
		while True:
			if p >= n:
				raise _PathError("unterminated quote")
			c = chars[p]
			if c == "\\" and p + 1 < n:
				out.append(c)
				out.append(chars[p + 1])
				p += 2
				continue
			p += 1
			if c == q:
				return "".join(out), p
			out.append(c)

	pos = 0
	segments = []
	while True:
		pos = skip_ws(pos)
		if pos >= n:
			raise _PathError("empty path")
		# Field name: quoted or bare.
		if chars[pos] == '"' or chars[pos] == "'":
			name, pos = read_quoted(pos)
		else:
			start = pos
			while pos < n and _is_bare_name_char(chars[pos]):
				pos += 1
			if pos == start:
				raise _PathError("expected field name, found '{}'".format(chars[pos]))
			name = chars[start:pos]
		selector = None
		pos = skip_ws(pos)
		# Optional selector, with its optional sugar colon (colon counts as
		# selector sugar only when the next non-ws char is an open bracket).
		bracket_at = None
		if pos < n and chars[pos] == "[":
			bracket_at = pos
		elif pos < n and chars[pos] == ":":
			q = skip_ws(pos + 1)
			if q < n and chars[q] == "[":
				bracket_at = q
		if bracket_at is not None:
			pos = skip_ws(bracket_at + 1)
			if pos < n and (chars[pos] == '"' or chars[pos] == "'"):
				v, pos = read_quoted(pos)
				selector = ("val", v)   # quotes force a value match, even numeric
			else:
				start = pos
				while pos < n and chars[pos] != "]":
					pos += 1
				body = _trim(chars[start:pos])
				if body == "*":
					selector = ("wild", None)
				elif body.startswith("#") and _parse_uint(body[1:]) is not None:
					selector = ("idx", _parse_uint(body[1:]))
				elif _parse_uint(body) is not None:
					selector = ("idx", _parse_uint(body))
				elif body == "":
					raise _PathError("empty selector")
				else:
					selector = ("val", _normalize_dangling_backslash(body))
			pos = skip_ws(pos)
			if pos >= n or chars[pos] != "]":
				raise _PathError("unterminated selector")
			pos = skip_ws(pos + 1)
		segments.append(_Segment(_fold_name(name), selector))
		if pos >= n:
			return segments, None
		c = chars[pos]
		if c == ".":
			pos += 1
		elif c == ":":
			pos += 1
			return segments, _trim(chars[pos:])
		else:
			raise _PathError("unexpected '{}' after field".format(c))


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------


class _Parser:
	def __init__(self):
		self.arena = [_Node("", _empty(), 0, 0)]
		self.diags = []
		# (indent string, node) for each open level; [0] is the virtual root.
		self.stack = [("", ROOT)]

	def _err(self, line, msg):
		self.diags.append(Diagnostic(line, Severity.Error, msg))

	def _select_or_create(self, parent, name, value, line):
		"""Find (or create by merge rule) the child of `parent` with this (name, value)."""
		key = value.key()
		for c in self.arena[parent].children:
			if self.arena[c].name == name and self.arena[c].value.key() == key:
				return c
		idx = len(self.arena)
		node = _Node(name, value, parent, line)
		self.arena.append(node)
		self.arena[parent].children.append(idx)
		return idx

	def _resolve_parent(self, indent):
		"""Resolve which open level this indent belongs to. Child only when the
		current top's indent is a proper prefix; otherwise the indent must equal
		an open level exactly (dedent), else it is a recoverable error."""
		top_indent, top_node = self.stack[-1]
		if len(indent) > len(top_indent) and indent.startswith(top_indent):
			return top_node
		for i in range(len(self.stack) - 1, -1, -1):
			if self.stack[i][0] == indent:
				# Sibling of stack[i]: its parent is the entry below it.
				parent = ROOT if i == 0 else self.stack[i - 1][1]
				self.stack = self.stack[:max(i, 1)]
				if i == 0:
					self.stack = self.stack[:1]
				return parent
		return None

	def _attach_path(self, parent, segs, value, line):
		"""Walk path segments under `parent`, select-or-creating; returns the node
		for the last segment carrying `value`. None aborts the line (diagnosed)."""
		cur = parent
		last = len(segs) - 1
		for i, seg in enumerate(segs):
			is_last = i == last
			sel = seg.selector
			if sel is not None and sel[0] == "val":
				disc = _cell([_Element(sel[1], False)])
				cur = self._select_or_create(cur, seg.name, disc, line)
				if is_last and not value.is_empty():
					# `a.b[X]: v` - the discriminator is the value; a second
					# value has nowhere unambiguous to go.
					self._err(line, "value after selector on '{}' ignored".format(seg.name))
			elif sel is not None and sel[0] == "idx":
				matches = [c for c in self.arena[cur].children if self.arena[c].name == seg.name]
				k = sel[1]
				if k < len(matches):
					cur = matches[k]
				else:
					self._err(line, "no instance {} of '{}'".format(k, seg.name))
					return None
			elif sel is not None and sel[0] == "wild":
				self._err(line, "wildcard selector is query-only")
				return None
			elif not is_last:
				cur = self._select_or_create(cur, seg.name, _empty(), line)
			else:
				cur = self._select_or_create(cur, seg.name, value, line)
		return cur

	def _consume_raw(self, lines, i, open_line, ch, length, info):
		"""Consume raw-block content after an opening fence. Returns (value, next
		line index). Content keeps relative indentation; the common leading run
		is stripped."""
		content = []
		closed = False
		while i < len(lines):
			if _is_fence_close(lines[i], ch, length):
				closed = True
				i += 1
				break
			content.append(lines[i])
			i += 1
		if not closed:
			self._err(open_line, "unterminated raw block")
		# Strip the common leading whitespace (the visual nesting); keep the rest.
		common = None
		for l in content:
			if not _trim(l):
				continue
			lead = []
			for c in l:
				if c == " " or c == "\t":
					lead.append(c)
				else:
					break
			lead = "".join(lead)
			if common is None:
				common = lead
			else:
				p = []
				for a, b in zip(common, lead):
					if a == b:
						p.append(a)
					else:
						break
				common = "".join(p)
		if common is None:
			common = ""
		stripped = []
		for l in content:
			if not _trim(l):
				stripped.append("")
			elif l.startswith(common):
				stripped.append(l[len(common):])
			else:
				stripped.append(l)
		return _raw("\n".join(stripped), info, ch, length), i

	def _bind_block(self, parent, value, line):
		"""A bare fence line is a value line for its parent field: fills an empty
		value, else creates a new instance of that field (the repeated-leaf rule)."""
		if parent == ROOT:
			self._err(line, "raw block with no parent field")
			return
		if self.arena[parent].value.is_empty():
			self.arena[parent].value = value
		else:
			name = self.arena[parent].name
			grandparent = self.arena[parent].parent
			self._select_or_create(grandparent, name, value, line)

	def _add_star_element(self, parent, body, line):
		"""One stacked-list element (`* scalar`) appends to the parent's array."""
		if parent == ROOT:
			self._err(line, "list element with no parent field")
			return
		trimmed = _trim(body)
		if not trimmed:
			self._err(line, "empty list element")
			return
		# One scalar per line; a bare comma is an error, not a second element.
		if len(_split_unquoted_commas(trimmed)) > 1:
			self._err(line, "bare comma in list element (one element per line)")
			return
		el = _parse_element(trimmed)
		if el is None:
			self._err(line, "empty list element")
			return
		node = self.arena[parent]
		if node.value.kind == "empty":
			node.value = _cell([el])
			node.star_list = True
		elif node.value.kind == "cell" and node.star_list:
			node.value.els.append(el)
		else:
			self._err(line, "field already has a value; list element ignored")

	def _emit_repeated_leaf_hints(self):
		"""Legal input that looks like a common mistake: a field repeating as a bare
		scalar leaf. Mandatory hint per spec (never fails a load)."""
		hints = []
		for parent in range(len(self.arena)):
			# Group by name in first-appearance order: hint order must be
			# deterministic or the cross-binding check can't compare `check` output.
			by_name = []
			for c in self.arena[parent].children:
				name = self.arena[c].name
				found = None
				for entry in by_name:
					if entry[0] == name:
						found = entry
						break
				if found is not None:
					found[1].append(c)
				else:
					by_name.append((name, [c]))
			for name, group in by_name:
				if len(group) < 2:
					continue
				all_scalar_leaves = all(
					not self.arena[c].children
					and self.arena[c].value.kind == "cell"
					and not self.arena[c].star_list
					for c in group
				)
				if all_scalar_leaves:
					line = max(self.arena[c].line for c in group)
					joined = ", ".join(self.arena[c].value.display() for c in group)
					hints.append((line, "'{}' repeats as a bare leaf - did you mean '{}: {}'?".format(name, name, joined)))
		for line, message in hints:
			self.diags.append(Diagnostic(line, Severity.Hint, message))

	def parse(self, text, strictness):
		# UTF-8 BOM strip, then split keeping raw lines (CR stripped per line).
		if text.startswith("﻿"):
			text = text[1:]
		lines = [l[:-1] if l.endswith("\r") else l for l in text.split("\n")]
		i = 0
		nlines = len(lines)
		while i < nlines:
			lineno = i + 1
			line = _trim_end(lines[i])
			j = 0
			while j < len(line) and (line[j] == " " or line[j] == "\t"):
				j += 1
			indent = line[:j]
			rest = line[j:]
			if not rest or rest.startswith("#"):
				i += 1
				continue
			# Child-indent fence: a value line for its parent field.
			fo = _fence_open(rest)
			if fo is not None:
				ch, length, info = fo
				parent = self._resolve_parent(indent)
				if parent is None:
					self._err(lineno, "indentation matches no open level")
					i += 1
					continue
				value, nxt = self._consume_raw(lines, i + 1, lineno, ch, length, info)
				self._bind_block(parent, value, lineno)
				i = nxt
				continue
			# Stacked-list element: colon-less by construction ('*' can't begin a name).
			if rest.startswith("*"):
				after = rest[1:]
				if after.startswith(" ") or after.startswith("\t"):
					parent = self._resolve_parent(indent)
					if parent is None:
						self._err(lineno, "indentation matches no open level")
						i += 1
						continue
					body = _strip_comment(after)
					self._add_star_element(parent, body, lineno)
					i += 1
					continue
				self._err(lineno, "malformed line: '*' must be followed by a space")
				i += 1
				continue
			# Field line.
			content = _trim_end(_strip_comment(rest))
			if not content:
				i += 1   # the line was only a comment
				continue
			parent = self._resolve_parent(indent)
			if parent is None:
				self._err(lineno, "indentation matches no open level")
				i += 1
				continue
			try:
				segments, value_text = _scan_path(content)
			except _PathError as e:
				self._err(lineno, "malformed line skipped: {}".format(e.args[0]))
				i += 1
				continue
			nxt = i + 1
			if value_text is None:
				# A clean path with no colon is the one defined repair:
				# the obvious intent is that path with an empty value.
				self._err(lineno, "missing colon; repaired as an empty value")
				value = _empty()
			elif value_text == "":
				value = _empty()
			else:
				fo = _fence_open(value_text)
				if fo is not None:
					# Same-line fence spelling.
					ch, length, info = fo
					value, nxt = self._consume_raw(lines, i + 1, lineno, ch, length, info)
				else:
					value = _parse_cell(value_text)
			node = self._attach_path(parent, segments, value, lineno)
			if node is not None:
				self.stack.append((indent, node))
			i = nxt
		self._emit_repeated_leaf_hints()
		return Document(self.arena, self.diags, strictness)


# ---------------------------------------------------------------------------
# Document: load, diagnostics, formatter
# ---------------------------------------------------------------------------


class Document:
	"""A parsed SHCL document: the tree, its diagnostics, and its strictness level."""
	__slots__ = ("arena", "diags", "_strictness")

	def __init__(self, arena, diags, strictness):
		self.arena = arena
		self.diags = diags
		self._strictness = strictness

	@staticmethod
	def parse(text):
		"""Parse at Standard strictness. Never fails: bad lines are skipped and
		diagnosed, good values stay readable."""
		return _Parser().parse(text, Strictness.Standard)

	@staticmethod
	def parse_with(text, strictness):
		"""Parse at a chosen strictness. Only Strict can fail (any error diagnostic)."""
		doc = _Parser().parse(text, strictness)
		if strictness == Strictness.Strict and any(d.severity == Severity.Error for d in doc.diags):
			raise LoadError(doc.diags)
		return doc

	def diagnostics(self):
		return self.diags

	def strictness(self):
		return self._strictness

	# ----- formatter -----

	def to_canonical(self):
		"""Canonical form: block layout, tabs, insertion order, minimal quoting,
		redundancy collapsed, comments dropped. Scalar text is never rewritten."""
		out = []
		for c in self.arena[ROOT].children:
			self._emit_node(c, 0, out)
		return "".join(out)

	def _emit_node(self, idx, depth, out):
		node = self.arena[idx]
		out.append("\t" * depth)
		out.append(_emit_name(node.name))
		out.append(":")
		v = node.value
		if v.kind == "empty":
			out.append("\n")
		elif v.kind == "cell":
			out.append(" ")
			out.append(", ".join(_emit_element(e) for e in v.els))
			out.append("\n")
		else:
			# Child-indent spelling is canonical: bare name line, fenced block one
			# level deeper, verbatim content. Exception: if an earlier same-name
			# sibling is empty, the bare `name:` header would merge into it on
			# reparse and the fence would fill that instance instead - so use the
			# same-line spelling there.
			parent = node.parent
			siblings = self.arena[parent].children
			me = siblings.index(idx)
			would_merge = any(
				self.arena[c].name == node.name and self.arena[c].value.is_empty()
				for c in siblings[:me]
			)
			out.append(" " if would_merge else "\n")
			pad = "\t" * (depth + 1)
			fence = v.fence_char * v.fence_len
			if not would_merge:
				out.append(pad)
			out.append(fence)
			if v.info:
				# An info-string starting with the fence char would extend the run
				# on reparse; a space keeps the fence length intact.
				if v.info[0] == v.fence_char:
					out.append(" ")
				out.append(v.info)
			out.append("\n")
			if v.content:
				for l in v.content.split("\n"):
					if l:
						out.append(pad)
					out.append(l)
					out.append("\n")
			out.append(pad)
			out.append(fence)
			out.append("\n")
		for c in node.children:
			self._emit_node(c, depth + 1, out)

	# ----- accessor: path resolution -----

	def _children_named(self, parent, name):
		return [c for c in self.arena[parent].children if self.arena[c].name == name]

	def _resolve_from(self, start, segs):
		# Returns ("none",) | ("one", idx) | ("many", [idx]) | ("slots", [idx|None]).
		cur = list(start)
		for i, seg in enumerate(segs):
			nxt = []
			for node in cur:
				nxt.extend(self._children_named(node, seg.name))
			sel = seg.selector
			if sel is None:
				cur = nxt
			elif sel[0] == "val":
				v = sel[1]
				cur = [c for c in nxt if self.arena[c].value.display() == v]
			elif sel[0] == "idx":
				k = sel[1]
				cur = [nxt[k]] if k < len(nxt) else []
			else:
				# Wildcard: remaining path resolves per-instance; slots stay aligned.
				rest = segs[i + 1:]
				slots = []
				for inst in nxt:
					if not rest:
						slots.append(inst)
					else:
						r = self._resolve_from([inst], rest)
						slots.append(r[1] if r[0] == "one" else None)
				return ("slots", slots)
		if len(cur) == 0:
			return ("none",)
		if len(cur) == 1:
			return ("one", cur[0])
		return ("many", cur)

	def _resolve(self, path):
		# Returns a _resolve_from result, or ("err", Status).
		try:
			segments, value_text = _scan_path(path)
		except _PathError:
			return ("err", Status.NotFound)
		if value_text is not None:
			return ("err", Status.NotFound)   # a query has no value part
		return self._resolve_from([ROOT], segments)

	def count(self, path):
		"""Instance count at a path (0 when nothing matches)."""
		r = self._resolve(path)
		tag = r[0]
		if tag == "one":
			return 1
		if tag == "many" or tag == "slots":
			return len(r[1])
		return 0

	def instances(self, path):
		"""Instance values at a path, in file order."""
		r = self._resolve(path)
		tag = r[0]
		if tag == "one":
			nodes = [r[1]]
		elif tag == "many":
			nodes = r[1]
		elif tag == "slots":
			nodes = [n for n in r[1] if n is not None]
		else:
			nodes = []
		return [self.arena[n].value.display() for n in nodes]

	# ----- accessor: typed reads -----

	def _value_at(self, path):
		# Returns ("ok", value) or ("err", Status).
		r = self._resolve(path)
		tag = r[0]
		if tag == "err":
			return ("err", r[1])
		if tag == "none":
			return ("err", Status.NotFound)
		if tag == "many" or tag == "slots":
			return ("err", Status.Multiple)
		return ("ok", self.arena[r[1]].value)

	def _scalar_element(self, v):
		# Returns ("ok", element) or ("err", Status).
		if v.kind == "empty":
			return ("err", Status.Empty)
		if v.kind == "raw":
			return ("err", Status.BadType)
		if len(v.els) == 1:
			return ("ok", v.els[0])
		return ("err", Status.BadType)   # an array is not one scalar

	def _read_scalar(self, path, coerce, default):
		va = self._value_at(path)
		if va[0] == "err":
			return Read(default, va[1], None)
		value = va[1]
		raw = value.display()
		se = self._scalar_element(value)
		if se[0] == "err":
			return Read(default, se[1], raw)
		v = coerce(se[1])
		if v is None:
			return Read(default, Status.BadType, raw)
		return Read(v, Status.Good, raw)

	def read_int(self, path):
		lvl = self._strictness
		return self._read_scalar(path, lambda e: _parse_int_text(e, lvl), 0)

	def read_float(self, path):
		lvl = self._strictness
		return self._read_scalar(path, lambda e: _parse_float_text(e, lvl), 0.0)

	def read_bool(self, path):
		lvl = self._strictness
		return self._read_scalar(path, lambda e: _parse_bool_text(e.text, lvl), False)

	def read_datetime(self, path):
		return self._read_scalar(path, lambda e: parse_datetime(e.text), ShclDateTime())

	def read_string(self, path):
		"""Any value reads as a string: a raw block yields its content, an array its
		canonical inline text. Escapes are applied."""
		va = self._value_at(path)
		if va[0] == "err":
			return Read("", va[1], None)
		value = va[1]
		raw = value.display()
		if value.kind == "empty":
			return Read("", Status.Empty, raw)
		if value.kind == "raw":
			return Read(value.content, Status.Good, raw)
		if len(value.els) == 1:
			return Read(_apply_escapes(value.els[0].text), Status.Good, raw)
		return Read(value.display(), Status.Good, raw)

	def read_raw(self, path):
		"""Raw-block content (verbatim). Non-block values are BadType."""
		va = self._value_at(path)
		if va[0] == "err":
			return Read("", va[1], None)
		value = va[1]
		raw = value.display()
		if value.kind == "raw":
			return Read(value.content, Status.Good, raw)
		if value.kind == "empty":
			return Read("", Status.Empty, raw)
		return Read("", Status.BadType, raw)

	def read_raw_info(self, path):
		"""The advisory info-string of a raw block ("" when absent)."""
		va = self._value_at(path)
		if va[0] == "err":
			return Read("", va[1], None)
		value = va[1]
		if value.kind == "raw":
			return Read(value.info, Status.Good, value.display())
		return Read("", Status.BadType, value.display())

	def _read_array(self, path, coerce, default):
		r = self._resolve(path)
		tag = r[0]
		if tag == "err":
			return Read([], r[1], None)
		if tag == "slots":
			slots = r[1]
			out = []
			status = Status.Good
			for slot in slots:
				if slot is None:
					out.append(default)
					continue
				se = self._scalar_element(self.arena[slot].value)
				if se[0] == "err":
					out.append(default)
					continue
				v = coerce(se[1])
				if v is None:
					out.append(default)
					status = Status.BadType
				else:
					out.append(v)
			if not slots:
				status = Status.Empty
			return Read(out, status, None)
		if tag == "none":
			return Read([], Status.NotFound, None)
		if tag == "many":
			return Read([], Status.Multiple, None)
		# one
		value = self.arena[r[1]].value
		raw = value.display()
		if value.kind == "empty":
			return Read([], Status.Empty, raw)
		if value.kind == "raw":
			return Read([], Status.BadType, raw)
		out = []
		status = Status.Good
		for el in value.els:
			v = coerce(el)
			if v is None:
				out.append(default)
				status = Status.BadType
			else:
				out.append(v)
		return Read(out, status, raw)

	def read_int_array(self, path):
		lvl = self._strictness
		return self._read_array(path, lambda e: _parse_int_text(e, lvl), 0)

	def read_float_array(self, path):
		lvl = self._strictness
		return self._read_array(path, lambda e: _parse_float_text(e, lvl), 0.0)

	def read_bool_array(self, path):
		lvl = self._strictness
		return self._read_array(path, lambda e: _parse_bool_text(e.text, lvl), False)

	def read_datetime_array(self, path):
		return self._read_array(path, lambda e: parse_datetime(e.text), ShclDateTime())

	def read_string_array(self, path):
		return self._read_array(path, lambda e: _apply_escapes(e.text), "")

	# Full tier, Result form: value on Good; the sentinel Status raised otherwise.

	def get_int(self, path):
		r = self.read_int(path)
		if r.status == Status.Good:
			return r.value
		raise _StatusError(r.status)

	def get_float(self, path):
		r = self.read_float(path)
		if r.status == Status.Good:
			return r.value
		raise _StatusError(r.status)

	def get_bool(self, path):
		r = self.read_bool(path)
		if r.status == Status.Good:
			return r.value
		raise _StatusError(r.status)

	def get_string(self, path):
		r = self.read_string(path)
		if r.status == Status.Good:
			return r.value
		raise _StatusError(r.status)

	def get_raw(self, path):
		r = self.read_raw(path)
		if r.status == Status.Good:
			return r.value
		raise _StatusError(r.status)

	def get_datetime(self, path):
		r = self.read_datetime(path)
		if r.status == Status.Good:
			return r.value
		raise _StatusError(r.status)


class _StatusError(Exception):
	def __init__(self, status):
		self.status = status
		super().__init__(status.name)


def _emit_name(name):
	if name and all(_is_bare_name_char(c) for c in name):
		return name
	return _quote_text(name)


_RESERVED = set(" \t,:#\"'[]")


def _emit_element(e):
	"""Minimal quoting: bare unless a reserved character (or lookalike hazard) forces it."""
	t = e.text
	needs = (not t) or any(c in _RESERVED for c in t) or (_fence_open(t) is not None)
	return _quote_text(t) if needs else t


def _bare_quote_counts(t):
	"""Count quote chars that are NOT already escaped; escaped ones stay untouched
	or every round-trip would re-escape them."""
	dq = sq = 0
	it = iter(t)
	for c in it:
		if c == "\\":
			next(it, None)
		elif c == '"':
			dq += 1
		elif c == "'":
			sq += 1
	return dq, sq


def _quote_text(t):
	dq, sq = _bare_quote_counts(t)
	if dq == 0:
		return '"' + t + '"'
	if sq == 0:
		return "'" + t + "'"
	# Both quote kinds appear bare: escape the doubles, wrap in doubles.
	out = ['"']
	it = iter(t)
	for c in it:
		if c == "\\":
			out.append(c)
			nxt = next(it, None)
			if nxt is not None:
				out.append(nxt)
		elif c == '"':
			out.append('\\"')
		else:
			out.append(c)
	out.append('"')
	return "".join(out)


# ---------------------------------------------------------------------------
# Coercion ("intelligent but safe"; Loose re-admits a closed list of tricks)
# ---------------------------------------------------------------------------

_CURRENCY = set("$¢£¤¥₩₪₫€₭₮₱₲₴₹₺₼₽₾₿")

_I64_MIN = -(2 ** 63)
_I64_MAX = 2 ** 63 - 1


def _strip_currency(t):
	if t and t[0] in _CURRENCY:
		return t[1:]
	return t


def _parse_i64(t):
	# Rust t.parse::<i64>(): optional +/- then ASCII digits, range-checked.
	body = t[1:] if t[:1] in ("+", "-") else t
	if not body or not all(_is_ascii_digit(c) for c in body):
		return None
	n = int(t)
	if n < _I64_MIN or n > _I64_MAX:
		return None
	return n


def _rust_round(f):
	# Nearest integer, ties away from zero (matches Rust f64::round). Returns int.
	fl = math.floor(f)
	diff = f - fl
	if diff < 0.5:
		return fl
	if diff > 0.5:
		return fl + 1
	return fl + 1 if f > 0 else fl


def _parse_int_text(e, level):
	t = _trim(e.text)
	if level == Strictness.Loose:
		t = _strip_currency(t)
	# Plain decimal.
	body = t[1:] if t[:1] in ("+", "-") else t
	if body and all(_is_ascii_digit(c) for c in body):
		return _parse_i64(t)
	# Hex.
	if t[:1] == "-":
		neg = True
		hexs = t[1:]
	else:
		neg = False
		hexs = t[1:] if t[:1] == "+" else t
	if hexs[:2] in ("0x", "0X"):
		h = hexs[2:]
		if h and all(c in "0123456789abcdefABCDEF" for c in h):
			val = int(h, 16)
			if val > _I64_MAX:
				return None
			return -val if neg else val
	# Thousands separators, only inside quotes (bare commas are reserved).
	if e.quoted and "," in t:
		sign_body = t[1:] if t[:1] in ("+", "-") else t
		groups = sign_body.split(",")
		well_formed = (
			len(groups) > 1
			and groups[0] != ""
			and len(groups[0]) <= 3
			and all(_is_ascii_digit(c) for c in groups[0])
			and all(len(g) == 3 and all(_is_ascii_digit(c) for c in g) for g in groups[1:])
		)
		if well_formed:
			return _parse_i64(t.replace(",", ""))
	# Loose: a float (including %) rounds, half away from zero.
	if level == Strictness.Loose:
		f = _parse_float_text(e, level)
		if f is not None and not math.isnan(f) and not math.isinf(f):
			r = _rust_round(f)
			if -(2 ** 63) <= r <= 2 ** 63:
				if r > _I64_MAX:
					r = _I64_MAX
				elif r < _I64_MIN:
					r = _I64_MIN
				return r
	return None


def _float_shape_ok(t):
	body = t[1:] if t[:1] in ("+", "-") else t
	if not body:
		return False
	epos = -1
	for i, c in enumerate(body):
		if c == "e" or c == "E":
			epos = i
			break
	if epos >= 0:
		mantissa = body[:epos]
		exp = body[epos + 1:]
	else:
		mantissa = body
		exp = None
	if exp is not None:
		xb = exp[1:] if exp[:1] in ("+", "-") else exp
		if not xb or not all(_is_ascii_digit(c) for c in xb):
			return False
	dot = mantissa.find(".")
	if dot >= 0:
		int_part = mantissa[:dot]
		frac_part = mantissa[dot + 1:]
	else:
		int_part = mantissa
		frac_part = ""
	if int_part == "" and frac_part == "":
		return False
	return all(_is_ascii_digit(c) for c in int_part) and all(_is_ascii_digit(c) for c in frac_part)


def _parse_float_text(e, level):
	t = _trim(e.text)
	percent = False
	if level == Strictness.Loose:
		t = _strip_currency(t)
		if t.endswith("%"):
			t = _trim_end(t[:-1])
			percent = True
	if _float_shape_ok(t):
		try:
			v = float(t)
		except ValueError:
			return None
	else:
		# An integer is a valid float on read (incl. hex and quoted thousands).
		iv = _parse_int_text_no_loose(_Element(t, e.quoted))
		if iv is None:
			return None
		v = float(iv)
	return v / 100.0 if percent else v


def _parse_int_text_no_loose(e):
	# Integer forms only (no Loose float fallback) - used by the float path.
	return _parse_int_text(e, Strictness.Standard)


def _parse_bool_text(t, level):
	s = _ascii_lower(_trim(t))
	if s == "true":
		return True
	if s == "false":
		return False
	if level == Strictness.Strict:
		return None
	if s in ("yes", "on", "1"):
		return True
	if s in ("no", "off", "0"):
		return False
	if level == Strictness.Loose:
		if s in ("t", "y", "enable", "enabled"):
			return True
		if s in ("f", "n", "disable", "disabled"):
			return False
	return None


# ---------------------------------------------------------------------------
# Date/time (closed whitelist; shape match, then calendar validation)
# ---------------------------------------------------------------------------

_MONTHS = {
	"jan": 1, "feb": 2, "mar": 3, "apr": 4, "may": 5, "jun": 6,
	"jul": 7, "aug": 8, "sep": 9, "oct": 10, "nov": 11, "dec": 12,
	"january": 1, "february": 2, "march": 3, "april": 4, "june": 6,
	"july": 7, "august": 8, "september": 9, "october": 10,
	"november": 11, "december": 12,
}


def _month_from_name(s):
	return _MONTHS.get(_ascii_lower(s))


def _days_in_month(y, m):
	if m in (1, 3, 5, 7, 8, 10, 12):
		return 31
	if m in (4, 6, 9, 11):
		return 30
	if m == 2:
		return 29 if (y % 4 == 0 and y % 100 != 0) or y % 400 == 0 else 28
	return 0


def _valid_date(y, m, d):
	return 1 <= m <= 12 and d >= 1 and d <= _days_in_month(y, m)


def _parse_u32(s):
	# Rust u32 parse: optional leading '+', ASCII digits, range-checked.
	if not s:
		return None
	body = s[1:] if s[0] == "+" else s
	if not body or not all(_is_ascii_digit(c) for c in body):
		return None
	n = int(body)
	if n > 2 ** 32 - 1:
		return None
	return n


def _parse_year4(s):
	if len(s) == 4 and all(_is_ascii_digit(c) for c in s):
		return int(s)
	return None


def _parse_num2(s):
	if (len(s) == 1 or len(s) == 2) and all(_is_ascii_digit(c) for c in s):
		return int(s)
	return None


def _parse_date_part(s):
	s = _trim(s)
	# Compact 8-digit YYYYMMDD.
	if len(s) == 8 and all(_is_ascii_digit(c) for c in s):
		y = int(s[:4])
		m = int(s[4:6])
		d = int(s[6:8])
		return (y, m, d) if _valid_date(y, m, d) else None
	# Space-separated named-month forms; a comma may follow the day in "Mon DD, YYYY".
	toks = _split_ws(s)
	if len(toks) == 3:
		m = _month_from_name(toks[0])
		if m is not None:
			day_tok = toks[1][:-1] if toks[1].endswith(",") else toks[1]
			d = _parse_u32(day_tok)
			y = _parse_year4(toks[2])
			if d is None or y is None:
				return None
			return (y, m, d) if _valid_date(y, m, d) else None
		m = _month_from_name(toks[1])
		if m is not None:
			d = _parse_u32(toks[0])
			y = _parse_year4(toks[2])
			if d is None or y is None:
				return None
			return (y, m, d) if _valid_date(y, m, d) else None
		return None
	if len(toks) != 1:
		return None
	# Delimited forms: one of - / . used uniformly.
	delim = None
	for c in s:
		if c == "-" or c == "/" or c == ".":
			delim = c
			break
	if delim is None:
		return None
	parts = s.split(delim)
	if len(parts) != 3 or any(p == "" for p in parts):
		return None
	# The delimiter must be uniform: no other delimiter chars anywhere.
	if sum(1 for c in s if c == "-" or c == "/" or c == ".") != 2:
		return None
	if len(parts[0]) == 4 and all(_is_ascii_digit(c) for c in parts[0]):
		y = int(parts[0])
		m = _parse_num2(parts[1])
		d = _parse_num2(parts[2])
		if m is None or d is None:
			return None
		return (y, m, d) if _valid_date(y, m, d) else None
	m = _month_from_name(parts[0])
	if m is not None:
		d = _parse_num2(parts[1])
		y = _parse_year4(parts[2])
		if d is None or y is None:
			return None
		return (y, m, d) if _valid_date(y, m, d) else None
	m = _month_from_name(parts[1])
	if m is not None:
		d = _parse_num2(parts[0])
		y = _parse_year4(parts[2])
		if d is None or y is None:
			return None
		return (y, m, d) if _valid_date(y, m, d) else None
	return None   # everything else (MM/DD/YYYY, 2-digit years, epoch) is rejected


def _parse_time_part(s):
	"""Time with optional meridiem, fraction, zone: `H:MM[:SS[.f+]][ AM|PM][Z|+HH:MM]`.
	Returns ((h, mi, sec-if-written), frac-or-None, zone-or-None) or None."""
	t = _trim(s)
	# Zone suffix first (only valid after a time).
	zone = None
	if t and (t[-1] == "Z" or t[-1] == "z"):
		zone = ("utc", None)
		t = _trim_end(t[:-1])
	elif len(t) >= 6:
		tail = t[-6:]
		sign = tail[0]
		if (sign == "+" or sign == "-") and _is_ascii_digit(tail[1]) and _is_ascii_digit(tail[2]) \
				and tail[3] == ":" and _is_ascii_digit(tail[4]) and _is_ascii_digit(tail[5]):
			hh = int(tail[1:3])
			mm = int(tail[4:6])
			if hh <= 23 and mm <= 59:
				off = hh * 60 + mm
				if sign == "-":
					off = -off
				zone = ("offset", off)
				t = _trim_end(t[:-6])
	# Meridiem: dotted a.m. is rejected (the '.' fails the digit checks below).
	meridiem = None   # True = PM
	lower = _ascii_lower(t)
	if lower.endswith("am"):
		meridiem = False
		t = t[:len(_trim_end(lower[:-2]))]
	elif lower.endswith("pm"):
		meridiem = True
		t = t[:len(_trim_end(lower[:-2]))]
	t = _trim_end(t)
	# Fraction: only after seconds, '.' delimiter, 1-9 digits.
	dot = t.find(".")
	if dot >= 0:
		hms = t[:dot]
		f = t[dot + 1:]
		if f == "" or len(f) > 9 or not all(_is_ascii_digit(c) for c in f):
			return None
		frac = f
	else:
		hms = t
		frac = None
	parts = hms.split(":")
	if len(parts) < 2 or len(parts) > 3:
		return None
	if frac is not None and len(parts) != 3:
		return None   # fraction can only follow HH:MM:SS
	h_raw = _parse_num2(parts[0])
	if h_raw is None:
		return None
	if len(parts[1]) != 2:
		return None
	mi = _parse_num2(parts[1])
	if mi is None:
		return None
	if len(parts) == 3:
		if len(parts[2]) != 2:
			return None
		sec = _parse_num2(parts[2])
		if sec is None:
			return None
	else:
		sec = None
	if mi > 59 or (sec is not None and sec > 59):
		return None
	if meridiem is None:
		if h_raw > 23:
			return None
		h = h_raw
	else:
		if not (1 <= h_raw <= 12):
			return None
		if not meridiem and h_raw == 12:
			h = 0
		elif not meridiem:
			h = h_raw
		elif h_raw == 12:
			h = 12
		else:
			h = h_raw + 12
	return ((h, mi, sec), frac, zone)


def parse_datetime(text):
	"""Whole-value date/time parse per the whitelist. None = BadType."""
	t = _trim(text)
	if not t:
		return None
	colon = t.find(":")
	if colon != -1:
		# Scan back over the 1-2 hour digits to find where the time starts.
		k = colon
		while k > 0 and _is_ascii_digit(t[k - 1]) and colon - k < 2:
			k -= 1
		if k == colon:
			return None   # ':' with no hour digits before it
		if k == 0:
			# Time-only value.
			tp = _parse_time_part(t)
			if tp is None:
				return None
			(h, mi, s), frac, zone = tp
			return ShclDateTime(date=None, time=(h, mi, s), frac=frac, zone=zone)
		# Combined: one separator char between date and time.
		sep = t[k - 1]
		if sep not in ("T", "t", " ", "_", "-", "/", "."):
			return None
		date = _parse_date_part(t[:k - 1])
		if date is None:
			return None
		tp = _parse_time_part(t[k:])
		if tp is None:
			return None
		(h, mi, s), frac, zone = tp
		return ShclDateTime(date=date, time=(h, mi, s), frac=frac, zone=zone)
	# Date-only.
	date = _parse_date_part(t)
	if date is None:
		return None
	return ShclDateTime(date=date, time=None, frac=None, zone=None)
