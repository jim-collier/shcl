# SPDX-License-Identifier: MIT
# Copyright © 2026 Jim Collier

"""SHCL binding for Python: parser, accessor, writer/formatter.

Single file on purpose - the drop-in story is "copy this file into your tree".
Behaviour tracks the Rust reference (source/rust/src/lib.rs) byte for byte; the
conformance corpus in project/conformance/ pins every behaviour here, and the
cicd cross-binding check compares this against the reference on every run.
Structure deliberately mirrors the reference over Python idiom, so a fix there
ports here by mechanical diff (parity over idiom - see style-guide.md).
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


class WriteReason(Enum):
	"""Why a write would fail (write_reason()): the distinctions behind a
	setter's bare False. Writable = the path passes the writer's validation;
	the rest name the five ways it cannot."""
	Writable = 0
	BadPath = 1       # empty path, or the scanner rejected it
	ValueInPath = 2   # the path carries a `: value` part; writes take values separately
	Wildcard = 3      # wildcard selectors are query-only
	NoSuchIndex = 4   # a `[#k]` instance that does not (and can never) exist
	TooDeep = 5       # deeper than the nesting cap; the writer never creates past it


class Diagnostic:
	__slots__ = ("line", "severity", "message", "code")

	def __init__(self, line, severity, message, code):
		self.line = line          # 1-based
		self.severity = severity
		self.message = message
		self.code = code          # stable machine code (E001.., H001..); the contract


# The one place prose couples to a code, so the wording stays free everywhere else.
def _diag_code(msg):
	for prefix, code in (
		("field mixed with list elements", "E001"),
		("value after selector on ", "E002"),
		("no instance ", "E003"),
		("wildcard selector is query-only", "E004"),
		("unterminated raw block", "E005"),
		("raw block with no parent field", "E006"),
		("list element with no parent field", "E007"),
		("list element mixed with field children", "E008"),
		("empty list element", "E009"),
		("bare comma in list element", "E010"),
		("field already has a value", "E011"),
		("indentation matches no open level", "E012"),
		("malformed line skipped", "E014"),
		("malformed line: ", "E013"),
		("missing colon", "E015"),
		("nesting deeper than", "E016"),
		("unterminated quote in value", "E017"),
		("merged with ", "H002"),
		("unknown field ", "V001"),
		("required path missing", "V002"),
		("wrong type at ", "V003"),
		("value not allowed at ", "V004"),
		("value below min at ", "V005"),
		("value above max at ", "V006"),
		("instance count out of bounds at ", "V007"),
		("unknown schema key ", "V090"),
		("unknown schema type ", "V091"),
		("bad schema constraint ", "V092"),
		("bad schema path", "V093"),
		("bad schema fragment", "V094"),
		("unknown schema fragment ", "V095"),
		("schema failed to load", "V099"),
	):
		if msg.startswith(prefix):
			return code
	return "E000"


class Read:
	"""Value plus status plus the original raw text (when the path resolved).
	Array reads also carry one status per slot (element, or wildcard instance)
	in .slots; .status is then the worst slot. Scalar reads leave .slots empty.
	.line is the 1-based source line of the resolved binding (0 when the path
	did not resolve to one node, or the node was writer-built), so a consumer
	check the schema cannot express can still cite the line. .quoted is True
	when the read's single scalar element was quoted in the source - the escape
	hatch that lets a downstream language reserve @null while "@null" stays a
	plain string. Arrays, raw blocks, and empties leave it False."""
	__slots__ = ("value", "status", "raw", "slots", "line", "quoted")

	def __init__(self, value, status, raw, slots=None):
		self.value = value
		self.status = status
		self.raw = raw
		self.slots = slots if slots is not None else []
		self.line = 0
		self.quoted = False

	def _at(self, line, quoted):
		self.line = line
		self.quoted = quoted
		return self

	def ok(self):
		return self.status in (Status.Good, Status.Empty)


class LoadError(Exception):
	"""A failed strict load. Carries the full diagnostics list AND the document
	the parse produced anyway - recover-and-continue means the diagnostics are
	the point, and the tree is what a Standard load would have kept."""
	def __init__(self, diagnostics, document=None):
		self.diagnostics = diagnostics
		self.document = document
		# Name the first few failures right in the message; the bare count made
		# callers dig for information the error was already holding.
		errs = [d for d in diagnostics if d.severity == Severity.Error]
		msg = "strict load failed: {} error diagnostic(s)".format(len(errs))
		for d in errs[:3]:
			msg += "; line {}: {} {}".format(d.line, d.code, d.message)
		if len(errs) > 3:
			msg += "; +{} more".format(len(errs) - 3)
		super().__init__(msg)


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


class _Lead:
	"""One whole-line comment held as trivia, plus whether a blank line preceded
	it - so a blank between comment-only regions survives the round-trip
	(blank runs collapse to one, same as nodes)."""
	__slots__ = ("text", "blank_before")

	def __init__(self, text, blank_before):
		self.text = text
		self.blank_before = blank_before


class _Pend:
	"""A pending whole-line comment during parse: text, source indent (used only
	to decide whether it hangs on a deeper block), and the blank it consumed."""
	__slots__ = ("text", "indent", "blank_before")

	def __init__(self, text, indent, blank_before):
		self.text = text
		self.indent = indent
		self.blank_before = blank_before


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
			# Length-prefix each element so the joined key is injective: a bare NUL
			# separator lets `[a, b]` collide with the single element "a\0b" (NUL is
			# legal in a quoted string), silently merging them.
			parts = ["c:"]
			for e in self.els:
				parts.append(str(len(e.text)))
				parts.append(":")
				parts.append(e.text)
			return "".join(parts)
		# Info-string is part of identity (a `sql` and a `python` block are
		# different values even with equal bodies); fence style is not. Info is
		# length-prefixed for the same injectivity reason as cell elements.
		return "r:" + str(len(self.info)) + ":" + self.info + self.content

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


def _cell_of(text):
	return _cell([_Element(text, False)])


def _array_cell(texts):
	# Inline-array value; the empty array is an empty value (reads back Empty).
	if not texts:
		return _empty()
	return _cell([_Element(t, False) for t in texts])


def _encode_string(s):
	"""Inverse of a scalar string read (_apply_escapes): only backslash, newline,
	and tab need encoding; _emit_element wraps quote/reserved chars itself and
	reparse strips that wrapping."""
	out = []
	for c in s:
		if c == "\\":
			out.append("\\\\")
		elif c == "\n":
			out.append("\\n")
		elif c == "\t":
			out.append("\\t")
		else:
			out.append(c)
	return "".join(out)


def _choose_fence(content):
	"""Pick a backtick fence long enough that no content line closes it early."""
	maxrun = 0
	for line in content.split("\n"):
		t = _trim(line)
		if t and all(ch == "`" for ch in t):
			maxrun = max(maxrun, len(t))
	return ("`", max(3, maxrun + 1))


class _Node:
	__slots__ = (
		"name", "value", "children", "parent", "line", "star_list", "star_mixed",
		"leading", "trailing", "after", "blank_before", "src",
	)

	def __init__(self, name, value, parent, line):
		self.name = name          # ASCII-folded to lower; non-ASCII never folds
		self.value = value
		self.children = []
		self.parent = parent
		self.line = line
		self.star_list = False    # value built from stacked "* " lines
		self.star_mixed = False   # mix of "* " and field children already diagnosed
		# Comment trivia, verbatim from `#` to end of line. Never part of identity
		# or reads; merged instances concatenate leading, first trailing wins
		# (later ones demote to leading - a canonical line has room for one).
		self.leading = []
		self.trailing = ""        # empty = none
		# Whole-line comments that followed this node's subtree at a deeper
		# indent than the next binding - they belong to this block, not the next
		# node, so a run trailing a block's last child stays put instead of
		# re-attaching dedented. Emitted after the subtree at this node's depth.
		self.after = []
		# Blank-line grouping is the other half of hand-authored layout: set
		# when a blank line preceded this node's binding line (runs collapse).
		self.blank_before = False
		# Verbatim value text from the source line (after the colon, comment
		# stripped, trimmed) - what a read's `raw` hands back. None when the
		# value was synthesized (writer, stacked list, fence), where raw falls
		# back to the display form.
		self.src = None


ROOT = 0


def _fold_node_into(arena, survivor, loser):
	"""Merge a later instance into an earlier one under the in-file merge rule:
	children and trivia move over, first trailing wins (a second demotes to a
	leading line), first spelling stays. The caller drops the loser from the
	parent's child list; it keeps its arena slot, unreferenced."""
	# Lists are references: after each move, point the loser at a fresh empty
	# list so the two nodes never share one.
	kids = arena[loser].children
	arena[loser].children = []
	for k in kids:
		arena[k].parent = survivor
	arena[survivor].children.extend(kids)
	arena[survivor].leading.extend(arena[loser].leading)
	arena[loser].leading = []
	trail = arena[loser].trailing
	arena[loser].trailing = ""
	if trail:
		if not arena[survivor].trailing:
			arena[survivor].trailing = trail
		else:
			arena[survivor].leading.append(_Lead(trail, False))
	arena[survivor].after.extend(arena[loser].after)
	arena[loser].after = []


# Maximum nesting depth (levels below the document root), enforced at load and
# by the Writer. Deeper lines are skipped with an E016 error. The cap is what
# keeps the recursive tree walks (emit, merge, clone) safely inside every
# binding's stack, so a hostile or machine-generated document can make a load
# fail but never crash the consumer.
MAX_DEPTH = 512


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


def _split_comment(s):
	"""Split off an unquoted trailing comment: (content, comment from `#` on,
	"" = none). A `\\` shields the next char throughout. Comments are kept as
	trivia."""
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
			return s[:i], s[i:]
		i += 1
	return s, ""


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


def _unterminated_quote(text):
	"""True when some piece starts with a quote that never closes (missing or
	escaped). Such a piece stays literal - and the quote-aware comment strip has
	already swallowed any trailing # comment into it - so the parser calls it
	out instead of letting the typo look deliberate. Mid-text apostrophes
	(it's fine) are legal prose and stay silent."""
	for piece in _split_unquoted_commas(text):
		t = _trim(piece)
		if not t:
			continue
		first = t[0]
		if first != '"' and first != "'":
			continue
		closed = False
		if len(t) >= 2 and t[-1] == first:
			esc = False
			for c in t[1:-1]:
				esc = (c == "\\") and not esc
			closed = not esc
		if not closed:
			return True
	return False


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


def _disp_key(v):
	"""The predicate a `[value]` selector matches with: display form with escapes
	applied on both sides, so `["q\\"uote"]` finds `'q"uote'` - a logical-string
	match, not spelling against spelling."""
	return _apply_escapes(v.display())


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
	# name folded; selector None or tuple; star = bare `*` name wildcard
	# (quoted "*" stays a literal name).
	__slots__ = ("name", "selector", "star")

	def __init__(self, name, selector, star=False):
		self.name = name
		self.selector = selector
		self.star = star


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
	return _scan_path_ex(inp, False)


def _scan_lookup(inp):
	"""Query spelling of _scan_path: also accepts a bare `*` segment (the name
	wildcard - any child name). Document lines never take it; only lookups
	(reads, the writer probe, schema paths) do."""
	return _scan_path_ex(inp, True)


def _scan_path_ex(inp, stars):
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
		# Field name: quoted, bare, or (lookups only) the `*` name wildcard.
		star = False
		if chars[pos] == '"' or chars[pos] == "'":
			name, pos = read_quoted(pos)
		elif stars and chars[pos] == "*":
			pos += 1
			star = True
			name = "*"
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
		if star and selector is not None:
			raise _PathError("selector on a name wildcard")
		segments.append(_Segment(_fold_name(name), selector, star))
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
		# Per-node (name, value-key) -> first matching child, parallel to arena.
		# Pure lookup accelerator for _select_or_create; children keeps the order.
		self.child_map = [{}]
		# Per-node (name, display) -> first matching child: the `[value]` selector
		# accelerator (its predicate is display(), a different and non-injective
		# key from child_map's). Same first-wins discipline, same mutation sites.
		self.disp_map = [{}]
		# Whole-line comments waiting for the next line that binds a node. The
		# source indent is kept only to decide after-attachment (a comment
		# deeper than the next binding hangs on the block it sits in).
		self.pending = []
		self.saw_blank = False  # a blank line waits to become the next bound node's blank_before
		# An open stacked list defers its merge-key remap (rebuilding the key per
		# element is O(list^2) time); (node, key, display) at deferral start,
		# flushed before any map lookup and at end of parse.
		self.star_open = None

	def _err(self, line, msg):
		self.diags.append(Diagnostic(line, Severity.Error, msg, _diag_code(msg)))

	def _select_or_create(self, parent, name, value, line):
		"""Find (or create by merge rule) the child of `parent` with this (name, value)."""
		self._star_flush()
		map_key = (name, value.key())
		found = self.child_map[parent].get(map_key)
		if found is not None:
			return found
		idx = len(self.arena)
		node = _Node(name, value, parent, line)
		self.arena.append(node)
		self.arena[parent].children.append(idx)
		self.child_map.append({})
		self.child_map[parent][map_key] = idx
		self.disp_map.append({})
		self.disp_map[parent].setdefault((name, _disp_key(node.value)), idx)
		return idx

	def _star_flush(self):
		"""Apply an open stacked list's deferred remap. Runs before any map lookup
		(and at end of parse), so both maps are always fresh when queried."""
		if self.star_open is not None:
			node, key, disp = self.star_open
			self.star_open = None
			self._remap_child(node, key, disp)

	def _remap_child(self, node, old_key, old_disp):
		"""A node's value mutated in place (empty field filled, star element added):
		move its map entry from the old key to the new one. First-wins on both
		sides so lookups keep matching the earliest sibling, like the scan did."""
		parent = self.arena[node].parent
		name = self.arena[node].name
		cmap = self.child_map[parent]
		if cmap.get((name, old_key)) == node:
			del cmap[(name, old_key)]
		cmap.setdefault((name, self.arena[node].value.key()), node)
		dmap = self.disp_map[parent]
		if dmap.get((name, old_disp)) == node:
			del dmap[(name, old_disp)]
		dmap.setdefault((name, _disp_key(self.arena[node].value)), node)

	def _fold_late_dups(self):
		"""A value that mutates after its sibling group was keyed - an empty field
		filled by a fence, a stacked list closed - can land on a key an earlier
		sibling already holds, which the keyed lookup can no longer catch. Fold
		those pairs so the tree matches a reparse of its own canonical text.
		Depth-first, since folding can carry duplicates down a level."""
		# Explicit stack: parse-side walks stay iterative so depth can't blow
		# Python's recursion limit.
		stack = [ROOT]
		while stack:
			parent = stack.pop()
			kids = self.arena[parent].children
			first = {}
			keep = []
			for c in kids:
				key = (self.arena[c].name, self.arena[c].value.key())
				survivor = first.get(key)
				if survivor is not None:
					_fold_node_into(self.arena, survivor, c)
				else:
					first[key] = c
					keep.append(c)
			stack.extend(keep)
			self.arena[parent].children = keep

	def _attach_trivia(self, node, trailing):
		"""Hand pending leading comments (and this line's trailing one) to a node.
		First trailing wins; a later one demotes to leading so nothing is lost."""
		n = self.arena[node]
		if self.pending:
			for p in self.pending:
				n.leading.append(_Lead(p.text, p.blank_before))
			self.pending = []
		if trailing:
			if not n.trailing:
				n.trailing = trailing
			else:
				n.leading.append(_Lead(trailing, False))

	def _hang_deeper_pending(self, new_indent):
		"""Comments written deeper than the incoming line belong to the block
		they sit in, not to the next binding: hang each on the deepest open
		level whose indent prefixes the comment's, so a run trailing a block's
		last child stays with that block instead of re-attaching dedented at
		the next node. Runs before the incoming line resolves (and at end of
		parse with the empty indent, so indented tail comments keep their block)."""
		if not self.pending:
			return
		taken = self.pending
		self.pending = []
		for p in taken:
			if len(p.indent) > len(new_indent):
				target = None
				for ind, node in reversed(self.stack):
					if node != ROOT and ind and len(ind) > len(new_indent) and p.indent.startswith(ind):
						target = node
						break
				if target is not None:
					self.arena[target].after.append(_Lead(p.text, p.blank_before))
					continue
			self.pending.append(p)

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
		self._star_flush()
		# Field child under a stacked list: diagnose the mix once, keep the field.
		pnode = self.arena[parent]
		if pnode.star_list and not pnode.star_mixed:
			pnode.star_mixed = True
			self._err(line, "field mixed with list elements")
		# Nesting cap: parent depth plus the segments this line adds. Checked
		# before any node is created so a rejected line leaves nothing behind.
		parent_depth = 0
		up = parent
		while up != ROOT:
			parent_depth += 1
			up = self.arena[up].parent
		if parent_depth + len(segs) > MAX_DEPTH:
			self._err(line, "nesting deeper than {} levels; line skipped".format(MAX_DEPTH))
			return None
		cur = parent
		last = len(segs) - 1
		for i, seg in enumerate(segs):
			is_last = i == last
			sel = seg.selector
			if sel is not None and sel[0] == "val":
				# Same escape-applied display predicate resolution uses, so a
				# selector also selects an array-valued instance instead of
				# creating a spurious second one - via the disp_map accelerator
				# (the inline spelling was quadratic in siblings without it).
				# Create only when nothing matches.
				found = self.disp_map[cur].get((seg.name, _apply_escapes(sel[1])))
				if found is not None:
					cur = found
				else:
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
				before = len(self.arena)
				cur = self._select_or_create(cur, seg.name, value, line)
				# Two separately-written bindings just combined: legal (the
				# merge rule), but only the parser can see it happened, so
				# say so. Adjacent re-mentions (still the newest binding at
				# this scope) and selector/path-intermediate merges stay
				# silent - those are the deliberate redundant-path idiom.
				if (cur < before
						and self.arena[cur].line != line
						and self.arena[self.arena[cur].parent].children[-1] != cur):
					at = self.arena[cur].line
					self.diags.append(Diagnostic(
						line, Severity.Hint,
						"merged with '{}' at line {} (same name and value combine)".format(seg.name, at),
						"H002"))
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
		for ln in content:
			if not _trim(ln):
				continue
			lead = []
			for c in ln:
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
		for ln in content:
			if not _trim(ln):
				stripped.append("")
			elif ln.startswith(common):
				stripped.append(ln[len(common):])
			else:
				stripped.append(ln)
		return _raw("\n".join(stripped), info, ch, length), i

	def _bind_block(self, parent, value, line):
		"""A bare fence line is a value line for its parent field: fills an empty
		value, else creates a new instance of that field (the repeated-leaf rule).
		Returns the node the block landed on (None = no parent, diagnosed)."""
		if parent == ROOT:
			self._err(line, "raw block with no parent field")
			return None
		if self.arena[parent].value.is_empty():
			old_key = self.arena[parent].value.key()
			old_disp = _disp_key(self.arena[parent].value)
			self.arena[parent].value = value
			self._remap_child(parent, old_key, old_disp)
			return parent
		name = self.arena[parent].name
		grandparent = self.arena[parent].parent
		return self._select_or_create(grandparent, name, value, line)

	def _add_star_element(self, parent, body, line):
		"""One stacked-list element (`* scalar`) appends to the parent's array."""
		if parent == ROOT:
			self._err(line, "list element with no parent field")
			return
		# Uniform-or-nothing (spec): a mix with field children is not a block array.
		if self.arena[parent].children:
			self._err(line, "list element mixed with field children; ignored")
			return
		trimmed = _trim(body)
		if not trimmed:
			self._err(line, "empty list element")
			return
		# One scalar per line; a bare comma is an error, not a second element.
		if len(_split_unquoted_commas(trimmed)) > 1:
			self._err(line, "bare comma in list element (one element per line)")
			return
		if _unterminated_quote(trimmed):
			self._err(line, "unterminated quote in value")
		el = _parse_element(trimmed)
		if el is None:
			self._err(line, "empty list element")
			return
		node = self.arena[parent]
		if node.value.kind == "empty":
			old_key = node.value.key()
			old_disp = _disp_key(node.value)
			node.value = _cell([el])
			node.star_list = True
			# First element: remap now (Empty -> cell changes both keys), then
			# open the deferral window with the current keys. Rebuilding the
			# keys per appended element was O(list^2) time; the maps only need
			# to be fresh when queried, and every query flushes first.
			self._remap_child(parent, old_key, old_disp)
			k = node.value.key()
			d = _disp_key(node.value)
			self.star_open = (parent, k, d)
		elif node.value.kind == "cell" and node.star_list:
			if self.star_open is None or self.star_open[0] != parent:
				self._star_flush()
				old_key = node.value.key()
				old_disp = _disp_key(node.value)
				self.star_open = (parent, old_key, old_disp)
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
			group_of = {}
			for c in self.arena[parent].children:
				name = self.arena[c].name
				g = group_of.get(name)
				if g is not None:
					by_name[g][1].append(c)
				else:
					group_of[name] = len(by_name)
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
			self.diags.append(Diagnostic(line, Severity.Hint, message, "H001"))

	def parse(self, text, strictness):
		# UTF-8 BOM strip, then split keeping raw lines (CR stripped per line).
		if text.startswith("﻿"):
			text = text[1:]
		lines = [ln[:-1] if ln.endswith("\r") else ln for ln in text.split("\n")]
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
			if not rest:
				self.saw_blank = True
				i += 1
				continue
			# Whole-line comment: hold it for the next line that binds a node.
			# It consumes a pending blank into its own flag, so a blank between
			# comment-only regions survives the round-trip.
			if rest.startswith("#"):
				self.pending.append(_Pend(rest, indent, self.saw_blank))
				self.saw_blank = False
				i += 1
				continue
			# Any other line consumes the pending blank; only a field line that
			# binds turns it into grouping.
			had_blank = self.saw_blank
			self.saw_blank = False
			# A binding line claims the pending comments - but deeper-written
			# ones hang on their own block first.
			self._hang_deeper_pending(indent)
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
				node = self._bind_block(parent, value, lineno)
				if node is not None:
					self._attach_trivia(node, "")
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
					body, comment = _split_comment(after)
					# Elements have no node of their own; trivia rides the field.
					if parent != ROOT:
						self._attach_trivia(parent, comment)
					self._add_star_element(parent, body, lineno)
					i += 1
					continue
				self._err(lineno, "malformed line: '*' must be followed by a space")
				i += 1
				continue
			# Field line.
			before, comment = _split_comment(rest)
			content = _trim_end(before)
			if not content:
				# Only a comment survived (e.g. an escaped lead-in); keep it.
				if comment:
					self.pending.append(_Pend(comment, indent, had_blank))
				i += 1
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
			# The verbatim value span, kept for reads' `raw` (only the plain
			# scalar/inline-array case has a one-line source spelling).
			src_text = None
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
					if _unterminated_quote(value_text):
						self._err(lineno, "unterminated quote in value")
					src_text = value_text
					value = _parse_cell(value_text)
			# Record only when the bound node holds exactly this line's value
			# (a merge into an equal-valued node keeps the first line's span;
			# a value dropped after a last-segment selector records nothing).
			vkey = value.key() if src_text is not None else None
			node = self._attach_path(parent, segments, value, lineno)
			if node is not None:
				if src_text is not None and self.arena[node].src is None and self.arena[node].value.key() == vkey:
					self.arena[node].src = src_text
				if had_blank:
					self.arena[node].blank_before = True
				self._attach_trivia(node, comment)
				self.stack.append((indent, node))
			i = nxt
		self._star_flush()
		self._fold_late_dups()
		self._emit_repeated_leaf_hints()
		# Indented tail comments keep their block; only top-level ones orphan.
		self._hang_deeper_pending("")
		orphans = [_Lead(p.text, p.blank_before) for p in self.pending]
		self.pending = []
		return Document(self.arena, self.diags, strictness, orphans)


# ---------------------------------------------------------------------------
# Document: load, diagnostics, formatter
# ---------------------------------------------------------------------------

_NO_DEFAULT = object()  # get_* sentinel: no call-site default -> must-exist (raises)


class Document:
	"""A parsed SHCL document: the tree, its diagnostics, and its strictness level."""
	__slots__ = ("arena", "diags", "_strictness", "orphans")

	def __init__(self, arena, diags, strictness, orphans=None):
		self.arena = arena
		self.diags = diags
		self._strictness = strictness
		self.orphans = orphans if orphans is not None else []

	@staticmethod
	def parse(text):
		"""Parse at Standard strictness. Never fails: bad lines are skipped and
		diagnosed, good values stay readable."""
		return _Parser().parse(text, Strictness.Standard)

	@staticmethod
	def parse_with(text, strictness):
		"""Parse at a chosen strictness. Only Strict can fail (any error
		diagnostic); the raised LoadError still carries the parsed document
		alongside the diagnostics."""
		doc = _Parser().parse(text, strictness)
		if strictness == Strictness.Strict and any(d.severity == Severity.Error for d in doc.diags):
			raise LoadError(doc.diags, doc)
		return doc

	def diagnostics(self):
		return self.diags

	def error_count(self):
		"""How many error-severity diagnostics the document carries - the "did
		this file have errors?" predicate, so recover-and-continue can't read
		as success by accident. Counts whatever diagnostics() holds (after
		load_and_validate, that includes validation errors)."""
		return sum(1 for d in self.diags if d.severity == Severity.Error)

	@staticmethod
	def load_and_validate(text, schema_text, strictness):
		"""One-shot load-and-validate: parse at a strictness, validate against a
		schema, and hand back the document carrying ONE combined diagnostics
		list (parse first, then validation - the order `check --schema`
		prints), so half the errors can't vanish because a caller forgot one
		of the two lists. Never fails: a strict-failing document comes back as
		the document plus its diagnostics (error_count() answers "did it
		fail"). An empty schema text skips validation entirely. H001 hints the
		schema disavows (a declared repeat upper bound above 1) are dropped."""
		doc = _Parser().parse(text, strictness)
		if _trim(schema_text):
			schema = Document.parse(schema_text)
			vdiags = doc.validate(schema)
			doc.diags.extend(vdiags)
			suppress_declared_repeats(schema, doc.diags)
		return doc

	def strictness(self):
		return self._strictness

	# ----- formatter -----

	def to_canonical(self):
		"""Canonical form: block layout, tabs, insertion order, minimal quoting,
		redundancy collapsed, comments re-emitted as attached trivia. Scalar
		text is never rewritten."""
		# Explicit stack, children pushed in reverse: the reference handles depths
		# far past Python's recursion limit, so emit must not recurse.
		out = []
		stack = []
		self._emit_children(self.arena[ROOT].children, 0, stack)
		while stack:
			idx, depth, would_merge = stack.pop()
			if would_merge is None:
				# Post-children marker: comments that hung on this block after
				# its last child re-emit at the block's own depth.
				pad = "\t" * depth
				for c in self.arena[idx].after:
					if c.blank_before and out:
						out.append("\n")
					out.append(pad)
					out.append(c.text)
					out.append("\n")
				continue
			self._emit_node(idx, depth, would_merge, out)
			if self.arena[idx].after:
				# The marker sits under the children, so it pops after them.
				stack.append((idx, depth, None))
			self._emit_children(self.arena[idx].children, depth + 1, stack)
		# Comments that never found a following line re-emit at the end.
		for c in self.orphans:
			if c.blank_before and out:
				out.append("\n")
			out.append(c.text)
			out.append("\n")
		return "".join(out)

	def _emit_children(self, kids, depth, stack):
		"""Emit a sibling run. The parent walk already knows whether an earlier
		same-name sibling is empty (the raw same-line-fence hazard), so one
		seen-empties set here replaces a per-child rescan of the whole run.
		Iterative shape: the flags are computed per run and ride the stack."""
		entries = []
		empties = set()
		for c in kids:
			n = self.arena[c]
			wm = n.value.kind == "raw" and n.name in empties
			if n.value.is_empty():
				empties.add(n.name)
			entries.append((c, depth, wm))
		stack.extend(reversed(entries))

	def _emit_node(self, idx, depth, would_merge, out):
		node = self.arena[idx]
		pad = "\t" * depth
		v = node.value
		# Same-line fence spelling can't carry an inline comment (an unbalanced
		# quote in the info-string could hide the `#` on reparse), so its
		# trailing comment joins the leading lines instead; the flag comes from
		# the parent's walk. Each blank rides its own comment (or the binding
		# line), never as the first output line.
		for c in node.leading:
			if c.blank_before and out:
				out.append("\n")
			out.append(pad)
			out.append(c.text)
			out.append("\n")
		if node.blank_before and out:
			out.append("\n")
		if would_merge and node.trailing:
			out.append(pad)
			out.append(node.trailing)
			out.append("\n")
		out.append(pad)
		out.append(_emit_name(node.name))
		out.append(":")
		if v.kind == "empty":
			if node.trailing:
				out.append("  ")
				out.append(node.trailing)
			out.append("\n")
		elif v.kind == "cell":
			out.append(" ")
			out.append(", ".join(_emit_element(e) for e in v.els))
			if node.trailing:
				out.append("  ")
				out.append(node.trailing)
			out.append("\n")
		else:
			# Child-indent spelling is canonical: bare name line, fenced block one
			# level deeper, verbatim content. Exception: if an earlier same-name
			# sibling is empty, the bare `name:` header would merge into it on
			# reparse and the fence would fill that instance instead - so use the
			# same-line spelling there.
			if would_merge:
				out.append(" ")
			else:
				if node.trailing:
					out.append("  ")
					out.append(node.trailing)
				out.append("\n")
			body_pad = "\t" * (depth + 1)
			fence = v.fence_char * v.fence_len
			if not would_merge:
				out.append(body_pad)
			out.append(fence)
			if v.info:
				# An info-string starting with the fence char would extend the run
				# on reparse; a space keeps the fence length intact.
				if v.info[0] == v.fence_char:
					out.append(" ")
				out.append(v.info)
			out.append("\n")
			if v.content:
				for ln in v.content.split("\n"):
					if ln:
						out.append(body_pad)
					out.append(ln)
					out.append("\n")
			out.append(body_pad)
			out.append(fence)
			out.append("\n")

	# ----- accessor: path resolution -----

	def _children_named(self, parent, name):
		return [c for c in self.arena[parent].children if self.arena[c].name == name]

	def _resolve_from(self, start, segs):
		# Returns ("none",) | ("one", idx) | ("many", [idx]) | ("slots", [entry]).
		# A slots entry is a node idx, or the Status saying why the sub-path did
		# not land on one node (NotFound missing, Multiple ambiguous).
		cur = list(start)
		for i, seg in enumerate(segs):
			nxt = []
			for node in cur:
				if seg.star:
					nxt.extend(self.arena[node].children)
				else:
					nxt.extend(self._children_named(node, seg.name))
			if seg.star:
				# Name wildcard: same per-slot split as `[*]`, over every child.
				rest = segs[i + 1:]
				slots = []
				for inst in nxt:
					if not rest:
						slots.append(inst)
					else:
						r = self._resolve_from([inst], rest)
						if r[0] == "one":
							slots.append(r[1])
						elif r[0] == "none":
							slots.append(Status.NotFound)
						else:
							slots.append(Status.Multiple)
				return ("slots", slots)
			sel = seg.selector
			if sel is None:
				cur = nxt
			elif sel[0] == "val":
				want = _apply_escapes(sel[1])
				cur = [c for c in nxt if _disp_key(self.arena[c].value) == want]
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
						if r[0] == "one":
							slots.append(r[1])
						elif r[0] == "none":
							slots.append(Status.NotFound)
						else:
							slots.append(Status.Multiple)
				return ("slots", slots)
		if len(cur) == 0:
			return ("none",)
		if len(cur) == 1:
			return ("one", cur[0])
		return ("many", cur)

	def _resolve(self, path):
		# Returns a _resolve_from result, or ("err", Status).
		try:
			segments, value_text = _scan_lookup(path)
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

	def paths(self):
		"""Every field path in the document, in file order, deduplicated - a
		query recipe for tooling. A segment that is not bare-name-safe is
		emitted quoted and escaped - the form the path scanner accepts - so
		each path is a well-formed lookup path and nothing in the document is
		hidden."""
		out = []
		seen = set()
		stack = [(c, "") for c in reversed(self.arena[ROOT].children)]
		while stack:
			node, prefix = stack.pop()
			seg = _emit_name(self.arena[node].name)
			path = seg if not prefix else prefix + "." + seg
			if path not in seen:
				seen.add(path)
				out.append(path)
			for c in reversed(self.arena[node].children):
				stack.append((c, path))
		return out

	def line(self, path):
		"""1-based source line of the binding at a path, for consumer checks the
		schema cannot express. 0 when the path does not resolve to exactly one
		node, or the node was writer-built. Merged instances cite the first
		binding's line, matching diagnostics."""
		r = self._resolve(path)
		if r[0] == "one":
			return self.arena[r[1]].line
		return 0

	def children(self, path):
		"""Child field names under a path, in file order, duplicates included -
		the "what keys are in this section?" question paths() (deduplicated,
		path-shaped) cannot answer. "" enumerates the top level. Names come
		back as stored; quote_segment() makes one splice-safe in a path."""
		if not _trim(path):
			node = ROOT
		else:
			r = self._resolve(path)
			if r[0] != "one":
				return []
			node = r[1]
		return [self.arena[c].name for c in self.arena[node].children]

	def instances(self, path):
		"""Instance values at a path, in file order. Wildcard slots that did not
		resolve stay in the list as "" so indices keep matching count()."""
		r = self._resolve(path)
		tag = r[0]
		if tag == "one":
			return [self.arena[r[1]].value.display()]
		if tag == "many":
			return [self.arena[n].value.display() for n in r[1]]
		if tag == "slots":
			return [self.arena[n].value.display() if isinstance(n, int) else ""
				for n in r[1]]
		return []

	# ----- writer: typed emit, defaults, comments, structural edits -----
	# The reverse of the Accessor. A setter builds the canonical stored text for a
	# typed value (the inverse of the matching read) and places it at a path,
	# creating intermediate nodes on the way. Reads and to_canonical walk the
	# children lists, so mutating the arena directly is enough.

	@staticmethod
	def new():
		"""A fresh document with no bindings - the start point for schema-driven
		generation. Set values, then to_canonical()."""
		return Document.parse("")

	def _new_child(self, parent, name, value):
		idx = len(self.arena)
		node = _Node(name, value, parent, 0)
		# Hand-written files separate top-level sections with a blank line;
		# writer-built ones do the same (the emitter never blanks line 1).
		node.blank_before = parent == ROOT
		self.arena.append(node)
		self.arena[parent].children.append(idx)
		return idx

	def _child_or_create(self, parent, name):
		for c in self.arena[parent].children:
			if self.arena[c].name == name:
				return c
		return self._new_child(parent, name, _empty())

	def write_reason(self, path):
		"""Why a write at this path would fail - the reason behind a setter's
		bare False, so a consumer's error message need not guess. Writable means
		the same validation _place() runs would pass; nothing is created."""
		try:
			segments, value_text = _scan_lookup(path)
		except _PathError:
			return WriteReason.BadPath
		if value_text is not None:
			return WriteReason.ValueInPath
		if not segments:
			return WriteReason.BadPath
		# Writer side of the load-time nesting cap: never create deeper.
		if len(segments) > MAX_DEPTH:
			return WriteReason.TooDeep
		# The probe walk _place() validates with: once it falls off the existing
		# tree, a later `[#k]` can never match (fresh intermediates are created
		# childless), so an index segment past that point is unresolvable.
		probe = ROOT
		for seg in segments:
			if seg.star:
				return WriteReason.Wildcard
			sel = seg.selector
			if sel is not None and sel[0] == "wild":
				return WriteReason.Wildcard
			if sel is not None and sel[0] == "idx":
				if probe is None:
					return WriteReason.NoSuchIndex
				matches = [c for c in self.arena[probe].children if self.arena[c].name == seg.name]
				if sel[1] >= len(matches):
					return WriteReason.NoSuchIndex
				probe = matches[sel[1]]
			elif sel is not None and sel[0] == "val":
				if probe is not None:
					want = _apply_escapes(sel[1])
					found = None
					for c in self.arena[probe].children:
						if self.arena[c].name == seg.name and _disp_key(self.arena[c].value) == want:
							found = c
							break
					probe = found
			else:
				if probe is not None:
					found = None
					for c in self.arena[probe].children:
						if self.arena[c].name == seg.name:
							found = c
							break
					probe = found
		return WriteReason.Writable

	def _place(self, path):
		"""Walk (creating as needed) to the node a write targets. A trailing
		name with no selector hits the first same-named instance (or a new one);
		a `[value]` selector selects the matching instance or creates it; `[#k]`
		must already exist. None = path unusable for a write (write_reason()
		says why). Validation runs first, so a doomed path leaves no
		half-created intermediates behind."""
		if self.write_reason(path) != WriteReason.Writable:
			return None
		try:
			segments, _ = _scan_lookup(path)
		except _PathError:
			return None
		cur = ROOT
		for seg in segments:
			if seg.star:
				return None   # write_reason gates this; belt only
			sel = seg.selector
			if sel is None:
				cur = self._child_or_create(cur, seg.name)
			elif sel[0] == "val":
				want = _apply_escapes(sel[1])
				found = None
				for c in self.arena[cur].children:
					if self.arena[c].name == seg.name and _disp_key(self.arena[c].value) == want:
						found = c
						break
				cur = found if found is not None else self._new_child(cur, seg.name, _cell_of(sel[1]))
			elif sel[0] == "idx":
				matches = [c for c in self.arena[cur].children if self.arena[c].name == seg.name]
				if sel[1] >= len(matches):
					return None
				cur = matches[sel[1]]
			else:
				return None   # wildcard is query-only
		return cur

	def _set_value(self, path, value):
		idx = self._place(path)
		if idx is None:
			return False
		self.arena[idx].value = value
		self.arena[idx].src = None   # written value has no source spelling
		self._collapse_dup(idx)
		return True

	def _collapse_dup(self, node):
		# A written value may now collide with a same-named sibling under the
		# in-file merge rule; fold the pair the way a reparse would (earlier
		# sibling survives, later one folds children and trivia in) so Writer
		# output stays a formatter fixpoint.
		parent = self.arena[node].parent
		name = self.arena[node].name
		key = self.arena[node].value.key()
		other = None
		for c in self.arena[parent].children:
			if c != node and self.arena[c].name == name and self.arena[c].value.key() == key:
				other = c
				break
		if other is None:
			return
		siblings = self.arena[parent].children
		if siblings.index(other) < siblings.index(node):
			survivor, loser = other, node
		else:
			survivor, loser = node, other
		_fold_node_into(self.arena, survivor, loser)
		self.arena[parent].children = [c for c in self.arena[parent].children if c != loser]

	def exists(self, path):
		"""True when the path resolves to at least one real node."""
		r = self._resolve(path)
		tag = r[0]
		if tag == "one" or tag == "many":
			return True
		if tag == "slots":
			return any(isinstance(n, int) for n in r[1])
		return False

	def remove(self, path):
		"""Delete the node(s) at a path (with their subtrees); returns how many."""
		r = self._resolve(path)
		tag = r[0]
		if tag == "one":
			targets = [r[1]]
		elif tag == "many":
			targets = list(r[1])
		elif tag == "slots":
			targets = [n for n in r[1] if isinstance(n, int)]
		else:
			targets = []
		for t in targets:
			p = self.arena[t].parent
			self.arena[p].children = [c for c in self.arena[p].children if c != t]
		return len(targets)

	def set_comment(self, path, text):
		"""Attach a leading comment line to the node at a path (creating an empty
		node if absent). A missing '#' is added; only the first line is kept."""
		idx = self._place(path)
		if idx is None:
			return False
		line = text.split("\n", 1)[0]
		if not line.startswith("#"):
			line = "# " + line
		self.arena[idx].leading.append(_Lead(line, False))
		return True

	def set_int(self, path, v):
		return self._set_value(path, _cell_of(str(v)))

	def set_float(self, path, v):
		return self._set_value(path, _cell_of(format_float(v)))

	def set_bool(self, path, v):
		return self._set_value(path, _cell_of("true" if v else "false"))

	def set_string(self, path, v):
		return self._set_value(path, _cell_of(_encode_string(v)))

	def set_datetime(self, path, v):
		return self._set_value(path, _cell_of(str(v)))

	def set_raw(self, path, content, info):
		fc, fl = _choose_fence(content)
		return self._set_value(path, _raw(content, info, fc, fl))

	def set_empty(self, path):
		return self._set_value(path, _empty())

	def set_int_array(self, path, v):
		return self._set_value(path, _array_cell([str(x) for x in v]))

	def set_float_array(self, path, v):
		return self._set_value(path, _array_cell([format_float(x) for x in v]))

	def set_bool_array(self, path, v):
		return self._set_value(path, _array_cell(["true" if x else "false" for x in v]))

	def set_string_array(self, path, v):
		return self._set_value(path, _array_cell([_encode_string(x) for x in v]))

	def set_datetime_array(self, path, v):
		return self._set_value(path, _array_cell([str(x) for x in v]))

	# Default (only-if-absent) forms - the "emit defaults" half of the Writer.
	def set_int_default(self, path, v):
		if not self.exists(path):
			return self.set_int(path, v)
		return True

	def set_float_default(self, path, v):
		if not self.exists(path):
			return self.set_float(path, v)
		return True

	def set_bool_default(self, path, v):
		if not self.exists(path):
			return self.set_bool(path, v)
		return True

	def set_string_default(self, path, v):
		if not self.exists(path):
			return self.set_string(path, v)
		return True

	def set_datetime_default(self, path, v):
		if not self.exists(path):
			return self.set_datetime(path, v)
		return True

	def set_raw_default(self, path, content, info):
		if not self.exists(path):
			return self.set_raw(path, content, info)
		return True

	def set_int_array_default(self, path, v):
		if not self.exists(path):
			return self.set_int_array(path, v)
		return True

	def set_float_array_default(self, path, v):
		if not self.exists(path):
			return self.set_float_array(path, v)
		return True

	def set_bool_array_default(self, path, v):
		if not self.exists(path):
			return self.set_bool_array(path, v)
		return True

	def set_string_array_default(self, path, v):
		if not self.exists(path):
			return self.set_string_array(path, v)
		return True

	def set_datetime_array_default(self, path, v):
		if not self.exists(path):
			return self.set_datetime_array(path, v)
		return True

	# ----- layered loading: overlay a higher-priority document -----

	def merge(self, over):
		"""Overlay `over` (a higher-priority layer) onto self (the lower one).
		Container instances merge by (name, value) exactly like the in-file rule;
		a leaf name present in `over` replaces self's same-named children at that
		scope - provided those base children are leaves too - so scalars, arrays,
		and raw blocks get real override while a bare section header merges
		instead of wiping. over-only nodes are appended. Comment trivia rides
		with each node. Load(defaults, site, user) is a left fold of this: each
		later file overlaid on the earlier ones."""
		self._overlay(ROOT, over, ROOT)
		# Layers commonly share a footer; keeping one copy of each keeps a
		# stack of files from repeating it once per layer.
		for o in over.orphans:
			if not any(e.text == o.text for e in self.orphans):
				self.orphans.append(_Lead(o.text, o.blank_before))

	# One grouping pass over each side, then a single children rebuild: the
	# old shape re-filtered the over side per distinct name and re-scanned
	# (and re-keyed) the base side per over node - three O(K^2) terms at one
	# parent, plus a full list rebuild per replaced name.
	def _adopt_trivia(self, base, over, ok):
		"""A matched instance keeps the base node, so the over side's comments have
		to move onto it or they are lost. Same rule as an in-file merge: leading
		concatenates in layer order, first trailing wins."""
		# Per-element copies: the merged document must not share list objects
		# with `over`, which the caller may still use.
		src = over.arena[ok]
		self.arena[base].leading.extend(_Lead(c.text, c.blank_before) for c in src.leading)
		if src.trailing:
			if not self.arena[base].trailing:
				self.arena[base].trailing = src.trailing
			else:
				self.arena[base].leading.append(_Lead(src.trailing, False))
		self.arena[base].after.extend(_Lead(c.text, c.blank_before) for c in src.after)

	def _overlay(self, base_parent, over, over_parent):
		over_kids = list(over.arena[over_parent].children)
		# Over side: name -> node bucket, in first-appearance order.
		order = []
		groups = {}
		for k in over_kids:
			n = over.arena[k].name
			g = groups.get(n)
			if g is None:
				order.append(n)
				g = []
				groups[n] = g
			g.append(k)
		# Base side, one pass: does the name have a container instance, and
		# which child carries each (name, key) - every key computed once.
		base_kids = list(self.arena[base_parent].children)
		has_container = {}
		by_key = {}
		for b in base_kids:
			name = self.arena[b].name
			has_container[name] = has_container.get(name, False) or bool(self.arena[b].children)
			by_key.setdefault((name, self.arena[b].value.key()), b)
		# Decide per name. A name whose over-side nodes are all leaves is an
		# override - but only when the base side of the group is leaf-shaped
		# too. Against a base container, a childless over-node is a wrapper
		# mention, not a leaf, so it falls through to the instance merge: a
		# bare section header in a higher layer never wipes the subtree below.
		# Replaced groups splice in the rebuild; everything appended (unmatched
		# instances, and replaced names base never had) keeps processing order.
		replace = {}
		appended = []
		for name in order:
			group = groups[name]
			over_leafy = all(not over.arena[k].children for k in group)
			in_base = name in has_container
			base_container = has_container.get(name, False)
			if over_leafy and not base_container:
				clones = [self._clone_subtree(over, ok, base_parent) for ok in group]
				if in_base:
					replace[name] = clones
				else:
					appended.extend(clones)
			else:
				for ok in group:
					okey = over.arena[ok].value.key()
					b = by_key.get((name, okey))
					if b is not None:
						self._adopt_trivia(b, over, ok)
						self._overlay(b, over, ok)
					else:
						appended.append(self._clone_subtree(over, ok, base_parent))
		if not replace and not appended:
			return
		# Rebuild once: each replaced group lands at its name's first original
		# position (dropped nodes stay in the arena, unreferenced - reads and
		# emit walk children from the root), appends go at the end.
		new_kids = []
		spliced = set()
		for b in base_kids:
			name = self.arena[b].name
			clones = replace.get(name)
			if clones is None:
				new_kids.append(b)
			elif name not in spliced:
				spliced.add(name)
				new_kids.extend(clones)
		new_kids.extend(appended)
		self.arena[base_parent].children = new_kids

	def _clone_subtree(self, over, oi, parent):
		"""Deep-copy `over`'s subtree at `oi` into self's arena under `parent`."""
		src = over.arena[oi]
		# Copy the value too - sharing the object (and its element list) with
		# `over` would break the promise that the clone survives its release.
		cv = _Value(src.value.kind)
		cv.els = None if src.value.els is None else [_Element(e.text, e.quoted) for e in src.value.els]
		cv.content = src.value.content
		cv.info = src.value.info
		cv.fence_char = src.value.fence_char
		cv.fence_len = src.value.fence_len
		node = _Node(src.name, cv, parent, src.line)
		node.star_list = src.star_list
		node.star_mixed = src.star_mixed
		node.leading = [_Lead(c.text, c.blank_before) for c in src.leading]
		node.trailing = src.trailing
		node.after = [_Lead(c.text, c.blank_before) for c in src.after]
		node.blank_before = src.blank_before
		node.src = src.src
		idx = len(self.arena)
		self.arena.append(node)
		for ok in list(over.arena[oi].children):
			c = self._clone_subtree(over, ok, idx)
			self.arena[idx].children.append(c)
		return idx

	# ----- accessor: typed reads -----

	def _node_at(self, path):
		# Returns ("ok", node index) or ("err", Status).
		r = self._resolve(path)
		tag = r[0]
		if tag == "err":
			return ("err", r[1])
		if tag == "none":
			return ("err", Status.NotFound)
		if tag == "many" or tag == "slots":
			return ("err", Status.Multiple)
		return ("ok", r[1])

	def _raw_of(self, n):
		"""A read's `raw`: the verbatim source value text when the value came from
		one source line, else the display form (writer-built, stacked list, raw
		block - shapes with no one-line source spelling)."""
		src = self.arena[n].src
		return src if src is not None else self.arena[n].value.display()

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
		na = self._node_at(path)
		if na[0] == "err":
			return Read(default, na[1], None)
		value = self.arena[na[1]].value
		raw = self._raw_of(na[1])
		line = self.arena[na[1]].line
		se = self._scalar_element(value)
		if se[0] == "err":
			return Read(default, se[1], raw)._at(line, False)
		v = coerce(se[1])
		if v is None:
			return Read(default, Status.BadType, raw)._at(line, se[1].quoted)
		return Read(v, Status.Good, raw)._at(line, se[1].quoted)

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
		na = self._node_at(path)
		if na[0] == "err":
			return Read("", na[1], None)
		value = self.arena[na[1]].value
		raw = self._raw_of(na[1])
		line = self.arena[na[1]].line
		if value.kind == "empty":
			return Read("", Status.Empty, raw)._at(line, False)
		if value.kind == "raw":
			return Read(value.content, Status.Good, raw)._at(line, False)
		if len(value.els) == 1:
			return Read(_apply_escapes(value.els[0].text), Status.Good, raw)._at(line, value.els[0].quoted)
		# Canonical inline form (quoting + escapes intact), so the string
		# re-parses to the same array - not the bare display join.
		return Read(", ".join(_emit_element(e) for e in value.els), Status.Good, raw)._at(line, False)

	def read_raw(self, path):
		"""Raw-block content (verbatim). Non-block values are BadType."""
		na = self._node_at(path)
		if na[0] == "err":
			return Read("", na[1], None)
		value = self.arena[na[1]].value
		raw = self._raw_of(na[1])
		line = self.arena[na[1]].line
		if value.kind == "raw":
			return Read(value.content, Status.Good, raw)._at(line, False)
		if value.kind == "empty":
			return Read("", Status.Empty, raw)._at(line, False)
		return Read("", Status.BadType, raw)._at(line, False)

	def read_raw_info(self, path):
		"""The advisory info-string of a raw block ("" when absent)."""
		na = self._node_at(path)
		if na[0] == "err":
			return Read("", na[1], None)
		raw = self._raw_of(na[1])
		line = self.arena[na[1]].line
		value = self.arena[na[1]].value
		if value.kind == "raw":
			return Read(value.info, Status.Good, raw)._at(line, False)
		return Read("", Status.BadType, raw)._at(line, False)

	def _read_array(self, path, coerce, default):
		r = self._resolve(path)
		tag = r[0]
		if tag == "err":
			return Read([], r[1], None)
		if tag == "slots":
			# Each slot reads like a scalar of the target type and records its
			# own status; the aggregate is the worst one (never silently Good).
			out = []
			sts = []
			for slot in r[1]:
				if not isinstance(slot, int):
					out.append(default)
					sts.append(slot)
					continue
				se = self._scalar_element(self.arena[slot].value)
				if se[0] == "err":
					out.append(default)
					sts.append(se[1])
					continue
				v = coerce(se[1])
				if v is None:
					out.append(default)
					sts.append(Status.BadType)
				else:
					out.append(v)
					sts.append(Status.Good)
			status = max(sts, key=lambda s: s.value) if sts else Status.Empty
			return Read(out, status, None, sts)
		if tag == "none":
			return Read([], Status.NotFound, None)
		if tag == "many":
			return Read([], Status.Multiple, None)
		# one
		value = self.arena[r[1]].value
		raw = self._raw_of(r[1])
		line = self.arena[r[1]].line
		if value.kind == "empty":
			return Read([], Status.Empty, raw)._at(line, False)
		if value.kind == "raw":
			return Read([], Status.BadType, raw)._at(line, False)
		out = []
		sts = []
		status = Status.Good
		for el in value.els:
			v = coerce(el)
			if v is None:
				out.append(default)
				sts.append(Status.BadType)
				status = Status.BadType
			else:
				out.append(v)
				sts.append(Status.Good)
		return Read(out, status, raw, sts)._at(line, False)

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

	# Convenience/get tier: value on Good, else the call-site default. Pass a
	# `default` to make the read forgiving (Default mode - the call a beginner
	# writes 90% of the time; a missing/empty/bad/ambiguous read can't sneak in
	# as a real zero). With no default it must-exist, raising the Status. Array
	# forms fall back to the whole default list; per-slot substitution is the
	# read_*_array tier or the CLI --default.

	def _get(self, r, default):
		if r.status == Status.Good:
			return r.value
		if default is _NO_DEFAULT:
			raise StatusError(r.status)
		return default

	def get_int(self, path, default=_NO_DEFAULT):
		return self._get(self.read_int(path), default)

	def get_float(self, path, default=_NO_DEFAULT):
		return self._get(self.read_float(path), default)

	def get_bool(self, path, default=_NO_DEFAULT):
		return self._get(self.read_bool(path), default)

	def get_string(self, path, default=_NO_DEFAULT):
		return self._get(self.read_string(path), default)

	def get_raw(self, path, default=_NO_DEFAULT):
		return self._get(self.read_raw(path), default)

	def get_datetime(self, path, default=_NO_DEFAULT):
		return self._get(self.read_datetime(path), default)

	def get_int_array(self, path, default=_NO_DEFAULT):
		return self._get(self.read_int_array(path), default)

	def get_float_array(self, path, default=_NO_DEFAULT):
		return self._get(self.read_float_array(path), default)

	def get_bool_array(self, path, default=_NO_DEFAULT):
		return self._get(self.read_bool_array(path), default)

	def get_string_array(self, path, default=_NO_DEFAULT):
		return self._get(self.read_string_array(path), default)

	def get_datetime_array(self, path, default=_NO_DEFAULT):
		return self._get(self.read_datetime_array(path), default)

	# --- Validator: schema-as-SHCL (see spec.md "Schema validation") ---------
	# The schema is an ordinary parsed document: a flat list of `field: <path>`
	# instances whose children are the constraints (closed vocabulary).
	# Validation reuses the accessor's path scan and the typed coercions, so
	# document strictness composes for free. Any schema fault (V09x) suppresses
	# data validation - one line-number space per result.

	def validate(self, schema):
		"""Validate against a schema document (itself plain SHCL). Empty result
		= the document conforms. Diagnostic lines are document lines (0 =
		document scope); schema faults (V09x, schema-file lines) suppress data
		validation entirely."""
		sdef, faults = _build_schema(schema)
		if faults:
			return faults
		out = []
		for c in sdef.cons:
			self._v_check(c, sdef, out)
		self._v_unknown(sdef, out)
		return out

	def _v_contexts(self, start, segs, anchor, out):
		# Resolution contexts: the whole document for a plain path; each
		# enclosing instance for the part of a path after a wildcard. required/
		# repeat evaluate per context (anchor line 0 = document scope), so
		# `server[*].port` + required means a port under EACH server -
		# vacuously true with no servers.
		cur = list(start)
		for i, seg in enumerate(segs):
			nxt = []
			for n in cur:
				if seg.star:
					nxt.extend(self.arena[n].children)
				else:
					nxt.extend(self._children_named(n, seg.name))
			if seg.star:
				# Name wildcard: same per-instance split as `[*]`, any child name.
				rest = segs[i + 1:]
				if not rest:
					out.append((anchor, nxt))
				else:
					for inst in nxt:
						self._v_contexts([inst], rest, self.arena[inst].line, out)
				return
			sel = seg.selector
			if sel is None:
				cur = nxt
			elif sel[0] == "val":
				want = _apply_escapes(sel[1])
				cur = [c for c in nxt if _disp_key(self.arena[c].value) == want]
			elif sel[0] == "idx":
				cur = [nxt[sel[1]]] if sel[1] < len(nxt) else []
			else:
				rest = segs[i + 1:]
				if not rest:
					out.append((anchor, nxt))
				else:
					for inst in nxt:
						self._v_contexts([inst], rest, self.arena[inst].line, out)
				return
		out.append((anchor, cur))

	def _v_check(self, c, sdef, out):
		self._v_check_from(c, sdef, ROOT, 0, out)

	# A mounted fragment's fields run per resolved node, right after that
	# node's own checks, in fragment order - depth-first, so diagnostic order
	# stays derivable. Termination is structural: every mount descends at
	# least one document level, and the document is finite.
	def _v_check_from(self, c, sdef, start, anchor0, out):
		ctxs = []
		self._v_contexts([start], c.segs, anchor0, ctxs)
		for anchor, found in ctxs:
			if c.required and not found:
				_vdiag(out, anchor, "required path missing: {}".format(c.path))
			if c.repeat is not None:
				lo, hi = c.repeat
				n = len(found)
				if n < lo or n > hi:
					_vdiag(out, anchor, "instance count out of bounds at '{}': {} not in {}..{}".format(c.path, n, lo, hi))
			for n in found:
				self._v_node(c, n, out)
				if c.inherits is not None:
					fcs = sdef.frags.get(c.inherits)
					if fcs is not None:
						for fc in fcs:
							self._v_check_from(fc, sdef, n, self.arena[n].line, out)

	def _v_node(self, c, n, out):
		node = self.arena[n]
		line = node.line
		base = c.ty[:-6] if c.ty is not None and c.ty.endswith("-array") else c.ty
		is_array = c.ty is not None and c.ty.endswith("-array")

		def wrong():
			_vdiag(out, line, "wrong type at '{}': value is not a valid {}".format(c.path, c.ty))

		if node.value.kind == "empty":
			# Empty passes everything; required already counted it as present.
			return
		if node.value.kind == "raw":
			# A raw block satisfies `raw` and scalar `string` (any value reads
			# as a string); every other kind is a type miss.
			if c.ty is not None and ((base != "raw" and base != "string") or is_array):
				wrong()
				return
			if c.allowed is not None and c.allowed[0] == "strings":
				if node.value.content not in c.allowed[1]:
					_vdiag(out, line, "value not allowed at '{}': {}".format(c.path, node.value.content))
			return
		els = node.value.els
		if base == "raw":
			wrong()
			return
		# A scalar kind on a multi-element value is the array-where-one-scalar-
		# expected miss - except string, which reads arrays.
		if c.ty is not None and not is_array and base != "string" and len(els) > 1:
			wrong()
			return
		if base == "int":
			vals = []
			for e in els:
				v = _parse_int_text(e, self._strictness)
				if v is None:
					wrong()
					return
				vals.append(v)
			if c.allowed is not None and c.allowed[0] == "ints":
				for i, v in enumerate(vals):
					if v not in c.allowed[1]:
						_vdiag(out, line, "value not allowed at '{}': {}".format(c.path, els[i].text))
						break
			if c.min_i is not None and any(v < c.min_i for v in vals):
				_vdiag(out, line, "value below min at '{}'".format(c.path))
			if c.max_i is not None and any(v > c.max_i for v in vals):
				_vdiag(out, line, "value above max at '{}'".format(c.path))
		elif base == "float":
			vals = []
			for e in els:
				v = _parse_float_text(e, self._strictness)
				if v is None:
					wrong()
					return
				vals.append(v)
			if c.allowed is not None and c.allowed[0] == "floats":
				for i, v in enumerate(vals):
					if v not in c.allowed[1]:
						_vdiag(out, line, "value not allowed at '{}': {}".format(c.path, els[i].text))
						break
			if c.min_f is not None and any(v < c.min_f for v in vals):
				_vdiag(out, line, "value below min at '{}'".format(c.path))
			if c.max_f is not None and any(v > c.max_f for v in vals):
				_vdiag(out, line, "value above max at '{}'".format(c.path))
		elif base == "bool":
			vals = []
			for e in els:
				v = _parse_bool_text(e.text, self._strictness)
				if v is None:
					wrong()
					return
				vals.append(v)
			if c.allowed is not None and c.allowed[0] == "bools":
				for i, v in enumerate(vals):
					if v not in c.allowed[1]:
						_vdiag(out, line, "value not allowed at '{}': {}".format(c.path, els[i].text))
						break
		elif base == "datetime":
			vals = []
			for e in els:
				v = parse_datetime(e.text)
				if v is None:
					wrong()
					return
				vals.append(v)
			if c.allowed is not None and c.allowed[0] == "dates":
				for i, v in enumerate(vals):
					if not any(_dt_equal(v, a) for a in c.allowed[1]):
						_vdiag(out, line, "value not allowed at '{}': {}".format(c.path, els[i].text))
						break
		else:
			# string kind or untyped: every element coerces; only the allowed
			# set can fail, in logical-string space.
			if c.allowed is not None and c.allowed[0] == "strings":
				for e in els:
					s = _apply_escapes(e.text)
					if s not in c.allowed[1]:
						_vdiag(out, line, "value not allowed at '{}': {}".format(c.path, s))
						break

	def _v_unknown(self, sdef, out):
		# Unknown-field sweep: a schema path legalizes its name chain and every
		# prefix (selectors ignored). Only the topmost unknown node is
		# reported; its subtree is implied unknown and skipped.
		cons = sdef.cons
		# Chains below a fragment mount only match by descending the mounts.
		has_mounts = any(c.inherits is not None for c in cons)
		legal = set()
		# Sibling names per parent chain, built once (schema order): _v_suggest
		# used to rebuild every chain per unknown field, which bit hardest on
		# the wholesale-unmatched documents the feature exists for.
		siblings = {}
		# Paths with a `*` segment can't live in the exact-chain hash; they
		# match element-wise (a star matches any one name, prefixes included).
		star_pats = []
		for c in cons:
			if any(s.star for s in c.segs):
				star_pats.append(c.segs)
			chain = ""
			for s in c.segs:
				if s.star:
					break   # no sibling entry for '*'; deeper chains are pattern-only
				siblings.setdefault(chain, []).append(s.name)
				chain = s.name if not chain else chain + "\0" + s.name
				legal.add(chain)
		stack = [(c, "", "") for c in reversed(self.arena[ROOT].children)]
		while stack:
			n, pchain, pshown = stack.pop()
			node = self.arena[n]
			chain = node.name if not pchain else pchain + "\0" + node.name
			shown = node.name if not pshown else pshown + "." + node.name
			if (
				chain not in legal
				and not _star_legal(star_pats, chain)
				and not (has_mounts and _chain_legal(cons, sdef.frags, chain))
			):
				hint = _v_suggest(siblings, pchain, node.name)
				_vdiag(out, node.line, "unknown field '{}'{}".format(shown, hint))
				continue
			for k in reversed(node.children):
				stack.append((k, chain, shown))


class StatusError(Exception):
	"""Raised by the must-exist convenience reads (get_* with no default): a
	public name a caller can actually catch. Carries the Status in .status."""

	def __init__(self, status):
		self.status = status
		super().__init__(status.name)


def _emit_name(name):
	if name and all(_is_bare_name_char(c) for c in name):
		return name
	return _quote_text(name)


def quote_segment(name):
	"""Quote one path segment so it can be spliced into a lookup path: a bare
	name passes through, anything else comes back quoted and escaped in the
	form the path scanner accepts. Splicing user-typed text into a path
	without this is path injection - a dotted name silently reads as nesting.
	Same spelling paths() and the canonical emitter produce."""
	return _emit_name(name)


def suppress_declared_repeats(schema, diags):
	"""Drop the H001 hints a schema disavows: a field whose declared repeat upper
	bound is above 1 repeats BY DESIGN (repetition is its instance mechanism),
	so the repeated-bare-leaf hint is structurally a false positive there and
	trains users to ignore hints. Matching is by leaf name - the filter
	consumers were hand-rolling - which errs toward quiet, for a hint. Used by
	`check --schema` and load_and_validate; call it wherever doc diagnostics
	and a schema meet. Mutates diags in place."""
	# Top-level fields plus every fragment's fields: a repeat declared inside
	# a mounted shape disavows the hint the same way.
	groups = [("field", schema.instances("field"))]
	for k in range(schema.count("fragment")):
		base = "fragment[#{}].field".format(k)
		groups.append((base, schema.instances(base)))
	names = []
	for base, paths in groups:
		for i, p in enumerate(paths):
			# repeat is a 1-2 element array (`repeat: lo[, hi]`); the bound
			# that matters here is the last one.
			rep = schema.read_int_array("{}[#{}].repeat".format(base, i))
			if rep.status != Status.Good:
				continue
			if not rep.value or rep.value[-1] <= 1:
				continue
			raw = _trim(p.rsplit(".", 1)[-1].split("[", 1)[0])
			if raw == "*":
				continue   # name wildcard: no single leaf name to disavow
			leaf = raw.strip("\"'")
			if leaf:
				names.append(_ascii_lower(leaf))
	if not names:
		return
	kept = []
	for d in diags:
		if d.code == "H001":
			parts = d.message.split("'")
			name = parts[1] if len(parts) > 1 else ""
			if name in names:
				continue
		kept.append(d)
	diags[:] = kept


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
			# Range-check the magnitude against the sign, so the negative i64-min
			# magnitude (0x8000000000000000) reads like its decimal spelling.
			mag = int(h, 16)
			if mag > (_I64_MAX + 1 if neg else _I64_MAX):
				return None
			return -mag if neg else mag
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


# ---------------------------------------------------------------------------
# Validator: schema build helpers (module level; methods live on Document)
# ---------------------------------------------------------------------------

_SCHEMA_TYPES = (
	"int", "float", "bool", "string", "datetime", "raw",
	"int-array", "float-array", "bool-array", "string-array", "datetime-array",
)


class _Constraint:
	__slots__ = (
		"path", "segs", "ty", "required", "allowed",
		"min_i", "max_i", "min_f", "max_f", "repeat",
		"inherits", "inherits_line",
		"desc", "default_text",
	)

	def __init__(self, path, segs):
		self.path = path          # as written in the schema; message text only
		self.segs = segs
		self.ty = None            # member of _SCHEMA_TYPES
		self.required = False
		self.allowed = None       # ("ints"|"floats"|"bools"|"dates"|"strings", list)
		self.min_i = None
		self.max_i = None
		self.min_f = None
		self.max_f = None
		self.repeat = None        # (lo, hi)
		self.inherits = None      # fragment mounted at this path (subtree shape)
		self.inherits_line = 0    # schema line of the `inherits` key, for V095
		# Generator-only (`shcl init`): validation ignores both.
		self.desc = None          # `desc`, a one-line description
		self.default_text = None  # `default`, emitted as an inline value

	def clone(self):
		cc = _Constraint(self.path, list(self.segs))
		cc.ty = self.ty
		cc.required = self.required
		cc.allowed = self.allowed
		cc.min_i = self.min_i
		cc.max_i = self.max_i
		cc.min_f = self.min_f
		cc.max_f = self.max_f
		cc.repeat = self.repeat
		cc.inherits = self.inherits
		cc.inherits_line = self.inherits_line
		cc.desc = self.desc
		cc.default_text = self.default_text
		return cc


class _SchemaDef:
	# An interpreted schema: the top-level constraints plus the named fragments
	# their `inherits` keys can mount.
	__slots__ = ("cons", "frags")

	def __init__(self, cons, frags):
		self.cons = cons
		self.frags = frags        # name -> list of _Constraint


def _vdiag(out, line, msg):
	out.append(Diagnostic(line, Severity.Error, msg, _diag_code(msg)))


def _single_text(v):
	# One scalar constraint value (escapes applied), or None for anything else.
	if v.kind == "cell" and len(v.els) == 1:
		return _apply_escapes(v.els[0].text)
	return None


def _dt_equal(a, b):
	# Field-wise; tuples/None compare by value already.
	return a.date == b.date and a.time == b.time and a.frac == b.frac and a.zone == b.zone


def _build_schema(schema):
	"""Interpret a parsed schema document into constraints and fragments. A
	non-empty fault list (V09x, schema-file lines) means the caller reports
	those and validates nothing."""
	faults = []
	cons = []
	frags = {}
	for f in schema.arena[ROOT].children:
		node = schema.arena[f]
		if node.name == "field":
			c = _parse_field(schema, f, faults)
			if c is not None:
				cons.append(c)
		elif node.name == "fragment":
			name = _single_text(node.value)
			if not name:
				_vdiag(faults, node.line, "bad schema fragment")
				continue
			if name in frags:
				_vdiag(faults, node.line, "bad schema fragment '{}': duplicate".format(name))
				continue
			fcs = []
			for k in schema.arena[f].children:
				kid = schema.arena[k]
				if kid.name == "field":
					c = _parse_field(schema, k, faults)
					if c is not None:
						fcs.append(c)
				else:
					_vdiag(faults, kid.line, "bad schema fragment '{}': unknown key '{}'".format(name, kid.name))
			frags[name] = fcs
		else:
			_vdiag(faults, node.line, "unknown schema key '{}'".format(node.name))
	# Every mount must name a declared fragment; cycles (self or mutual) are
	# legal - expansion is demand-driven against a finite document.
	for c in cons + [fc for fcs in frags.values() for fc in fcs]:
		if c.inherits is not None and c.inherits not in frags:
			_vdiag(faults, c.inherits_line, "unknown schema fragment '{}'".format(c.inherits))
	if faults:
		# One constraint per line in practice, so line order = file order.
		faults.sort(key=lambda d: d.line)
		return None, faults
	return _SchemaDef(cons, frags), []


def _parse_field(schema, f, faults):
	"""One `field:` instance (top-level or inside a fragment) -> a _Constraint.
	None = faults were reported and the constraint is dropped."""
	node = schema.arena[f]
	path = _single_text(node.value)
	if path is None:
		_vdiag(faults, node.line, "bad schema path")
		return None
	try:
		segs, value_text = _scan_lookup(path)
	except _PathError:
		segs, value_text = None, None
	if segs is None or value_text is not None:
		_vdiag(faults, node.line, "bad schema path: {}".format(path))
		return None
	c = _Constraint(path, segs)
	# Deferred so `min: 1` may precede `type: int` in the file.
	required = None
	allowed_at = None
	min_at = None
	max_at = None
	for k in schema.arena[f].children:
		kid = schema.arena[k]
		if kid.value.is_empty():
			continue  # dangling key: treated as absent
		if kid.name == "type":
			t = _single_text(kid.value)
			if t is not None:
				t = _ascii_lower(t)
			if t in _SCHEMA_TYPES:
				if c.ty is not None:
					_vdiag(faults, kid.line, "bad schema constraint 'type'")
				else:
					c.ty = t
			elif t is not None:
				_vdiag(faults, kid.line, "unknown schema type '{}'".format(t))
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'type'")
		elif kid.name == "required":
			t = _single_text(kid.value)
			b = _parse_bool_text(t, Strictness.Standard) if t is not None else None
			if b is not None and required is None:
				required = b
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'required'")
		elif kid.name == "allowed":
			if kid.value.kind == "cell" and allowed_at is None:
				allowed_at = k
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'allowed'")
		elif kid.name == "min":
			if kid.value.kind == "cell" and len(kid.value.els) == 1 and min_at is None:
				min_at = k
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'min'")
		elif kid.name == "max":
			if kid.value.kind == "cell" and len(kid.value.els) == 1 and max_at is None:
				max_at = k
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'max'")
		elif kid.name == "repeat":
			if kid.value.kind == "cell" and c.repeat is None and len(kid.value.els) in (1, 2):
				lo = _parse_uint(kid.value.els[0].text)
				hi = _parse_uint(kid.value.els[-1].text)
				if lo is not None and hi is not None and lo <= hi:
					c.repeat = (lo, hi)
				else:
					_vdiag(faults, kid.line, "bad schema constraint 'repeat'")
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'repeat'")
		elif kid.name == "inherits":
			t = _single_text(kid.value)
			if t and c.inherits is None:
				c.inherits = t
				c.inherits_line = kid.line
			else:
				_vdiag(faults, kid.line, "bad schema constraint 'inherits'")
		elif kid.name == "desc":
			# Generator-only (`shcl init`); validation ignores it. First wins.
			if c.desc is None:
				c.desc = _single_text(kid.value)
		elif kid.name == "default":
			if c.default_text is None:
				c.default_text = _emit_value_inline(kid.value)
		else:
			_vdiag(faults, kid.line, "unknown schema key '{}'".format(kid.name))
	if required is not None:
		c.required = required
	base = c.ty[:-6] if c.ty is not None and c.ty.endswith("-array") else c.ty
	if base is None:
		base = "string"
	if allowed_at is not None:
		kid = schema.arena[allowed_at]
		els = kid.value.els
		# Schema values are read at Standard; only the document's values
		# coerce at the document's strictness.
		ok = True
		if base == "int":
			vals = [_parse_int_text(e, Strictness.Standard) for e in els]
			ok = all(v is not None for v in vals)
			setv = ("ints", vals)
		elif base == "float":
			vals = [_parse_float_text(e, Strictness.Standard) for e in els]
			ok = all(v is not None for v in vals)
			setv = ("floats", vals)
		elif base == "bool":
			vals = [_parse_bool_text(e.text, Strictness.Standard) for e in els]
			ok = all(v is not None for v in vals)
			setv = ("bools", vals)
		elif base == "datetime":
			vals = [parse_datetime(e.text) for e in els]
			ok = all(v is not None for v in vals)
			setv = ("dates", vals)
		elif base == "raw":
			ok = False  # a raw body has no element space to enumerate
			setv = None
		else:
			setv = ("strings", [_apply_escapes(e.text) for e in els])
		if ok:
			c.allowed = setv
		else:
			_vdiag(faults, kid.line, "bad schema constraint 'allowed'")
	for at, is_min in ((min_at, True), (max_at, False)):
		if at is None:
			continue
		kid = schema.arena[at]
		el = kid.value.els[0]
		key = "min" if is_min else "max"
		if base == "int":
			v = _parse_int_text(el, Strictness.Standard)
			if v is None:
				_vdiag(faults, kid.line, "bad schema constraint '{}'".format(key))
			elif is_min:
				c.min_i = v
			else:
				c.max_i = v
		elif base == "float":
			v = _parse_float_text(el, Strictness.Standard)
			if v is None:
				_vdiag(faults, kid.line, "bad schema constraint '{}'".format(key))
			elif is_min:
				c.min_f = v
			else:
				c.max_f = v
		else:
			_vdiag(faults, kid.line, "bad schema constraint '{}'".format(key))
	return c


def _emit_value_inline(v):
	# Re-emit a schema `default`/`allowed` value as an inline value (minimal
	# quoting, array elements joined by ", "). None for empty or raw - neither has
	# a usable one-line form. Used by the generator, not the validator.
	if v.kind != "cell":
		return None
	return ", ".join(_emit_element(e) for e in v.els)


def _allowed_join(a):
	kind, items = a
	if kind == "ints":
		return ", ".join(str(x) for x in items)
	if kind == "floats":
		return ", ".join(format_float(x) for x in items)
	if kind == "bools":
		return ", ".join("true" if x else "false" for x in items)
	if kind == "dates":
		return ", ".join(str(x) for x in items)
	return ", ".join(items)  # strings


def _gen_annotation(c, tyname):
	# The `# type, ...` line summarizing a constraint, ASCII only.
	parts = [tyname]
	if c.allowed is not None:
		parts.append("one of: " + _allowed_join(c.allowed))
	elif c.min_i is not None or c.max_i is not None:
		if c.min_i is not None and c.max_i is not None:
			parts.append("{}-{}".format(c.min_i, c.max_i))
		elif c.min_i is not None:
			parts.append(">= {}".format(c.min_i))
		else:
			parts.append("<= {}".format(c.max_i))
	elif c.min_f is not None or c.max_f is not None:
		if c.min_f is not None and c.max_f is not None:
			parts.append(format_float(c.min_f) + "-" + format_float(c.max_f))
		elif c.min_f is not None:
			parts.append(">= " + format_float(c.min_f))
		else:
			parts.append("<= " + format_float(c.max_f))
	if c.repeat is not None:
		lo, hi = c.repeat
		parts.append("repeat {}".format(lo) if lo == hi else "repeat {}-{}".format(lo, hi))
	if c.required:
		parts.append("required")
	return ", ".join(parts)


def _gen_default_text(v):
	# A default carrying a literal newline cannot sit on a value line; the
	# quoted escaped spelling reads back to the same string.
	if "\n" not in v:
		return v
	s = ['"']
	for ch in v:
		if ch == "\\":
			s.append("\\\\")
		elif ch == '"':
			s.append('\\"')
		elif ch == "\n":
			s.append("\\n")
		elif ch == "\t":
			s.append("\\t")
		else:
			s.append(ch)
	s.append('"')
	return "".join(s)


def generate(schema):
	"""Emit a commented, typed starter config from a schema (`shcl init
	--schema`). Paths that must exist (required, or a repeat lower bound of 1+)
	are live (their `default`, or an empty value); optional paths are commented
	out so the file is valid and minimal as-is. A must-exist wildcard path
	whose parent gets materialized by another live line is generated too, in
	dotted form - otherwise the file would fail the very schema that produced
	it - and remaining wildcard or `[#N]` paths (which cannot be materialized)
	are listed in a trailing comment block. The output always loads clean and
	validates clean against its schema, except a repeat lower bound of 2+
	(identical generated lines would merge, so the shortfall is reported).
	Returns (text, faults): a non-empty fault list (V09x) means the schema is
	broken and text is empty."""
	sdef, faults = _build_schema(schema)
	if faults:
		return "", faults
	cons, cuts = _expand_mounts(sdef)

	def must_exist(c):
		return c.required or (c.repeat is not None and c.repeat[0] >= 1)

	def has_wild(c):
		return any(s.selector is not None and s.selector[0] == "wild" for s in c.segs)

	# `[#N]` needs a pre-existing instance and its `#` would start a comment
	# on a binding line; a path with a literal newline cannot be written at
	# all. Both go to the trailing note instead of emitting a broken line.
	def unwritable(c):
		return any((s.selector is not None and s.selector[0] == "idx") or s.star for s in c.segs) or "\n" in c.path

	# Live concrete paths materialize instances; decide which must-exist
	# wildcards get filled (their first-wildcard parent chain is a prefix of
	# some live path). Fixpoint: a fill can materialize another's parent.
	def names_of(segs):
		return [s.name for s in segs]

	live = [names_of(c.segs) for c in cons if not has_wild(c) and not unwritable(c) and must_exist(c)]
	fill = [False] * len(cons)
	while True:
		changed = False
		for i, c in enumerate(cons):
			if fill[i] or not has_wild(c) or unwritable(c) or not must_exist(c):
				continue
			k = next(j for j, s in enumerate(c.segs) if s.selector is not None and s.selector[0] == "wild")
			parent = names_of(c.segs[: k + 1])
			if any(len(p) >= len(parent) and p[: len(parent)] == parent for p in live):
				fill[i] = True
				live.append(names_of(c.segs))
				changed = True
		if not changed:
			break
	out = []
	wild = []
	first = True
	for i, c in enumerate(cons):
		tyname = c.ty if c.ty is not None else "any"
		if unwritable(c) or (has_wild(c) and not fill[i]):
			wild.append((c.path.replace("\n", "\\n"), tyname))
			continue
		if not first:
			out.append("\n")
		first = False
		if c.desc is not None:
			for line in c.desc.split("\n"):
				out.append("# " + line + "\n")
		# The annotation is a comment: a newline smuggled in via an allowed
		# string value must not break out of it.
		out.append("# " + _gen_annotation(c, tyname).replace("\n", "\\n") + "\n")
		# A filled wildcard emits in dotted form, targeting the first (the
		# materialized) instance.
		path = c.path.replace("[*]", "") if fill[i] else c.path
		prefix = "" if must_exist(c) else "#"
		if c.default_text is not None:
			out.append("{}{}: {}\n".format(prefix, path, _gen_default_text(c.default_text)))
		else:
			out.append("{}{}:\n".format(prefix, path))
	# Cycle-cut mounts last: their "type" column names the fragment that
	# belongs at the path.
	wild.extend(cuts)
	if wild:
		if not first:
			out.append("\n")
		out.append("# Paths needing an instance name (not generated):\n")
		for path, tyname in wild:
			out.append("#   {}   {}\n".format(path, tyname))
	return "".join(out), []


def _expand_mounts(sdef):
	"""Inline every fragment mount into a flat constraint list, depth-first in
	schema order, each field's path and segments prefixed by its mount's. A
	mount whose fragment is already expanding (a cycle) stops there and is
	returned as (path, fragment name) for the trailing not-generated block."""
	out = []
	cuts = []
	stack = []

	def go(lst, at):
		for c in lst:
			cc = c.clone()
			if at is not None:
				p, s = at
				cc.path = "{}.{}".format(p, c.path)
				cc.segs = list(s) + list(c.segs)
			path = cc.path
			segs = cc.segs
			out.append(cc)
			if c.inherits is not None:
				if c.inherits in stack:
					cuts.append((path.replace("\n", "\\n"), c.inherits))
				else:
					fcs = sdef.frags.get(c.inherits)
					if fcs is not None:
						stack.append(c.inherits)
						go(fcs, (path, segs))
						stack.pop()

	go(sdef.cons, None)
	return out, cuts


def _edit_distance(a, b):
	# Two-row Levenshtein; powers the "did you mean" prose (never the code).
	prev = list(range(len(b) + 1))
	cur = [0] * (len(b) + 1)
	for i in range(1, len(a) + 1):
		cur[0] = i
		for j in range(1, len(b) + 1):
			cost = 0 if a[i - 1] == b[j - 1] else 1
			cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost)
		prev, cur = cur, prev
	return prev[len(b)]


def _star_legal(pats, chain):
	"""Element-wise chain match against the star-bearing schema paths: a `*`
	segment matches any one name, and every prefix of a path is legal."""
	if not pats:
		return False
	parts = chain.split("\0")
	return any(
		len(p) >= len(parts) and all(p[i].star or p[i].name == seg for i, seg in enumerate(parts))
		for p in pats
	)


def _chain_legal(cons, frags, chain):
	"""Chain legality through fragment mounts: the general matcher - element-
	wise like _star_legal (stars wild, prefixes legal), and when a mount's whole
	path matched with chain left over, the remainder is retried against the
	mounted fragment's fields. Terminates: every descent consumes >= 1 part."""
	parts = chain.split("\0")
	return _chain_parts_legal(cons, frags, parts)


def _chain_parts_legal(cons, frags, parts):
	for c in cons:
		n = len(c.segs)
		k = min(len(parts), n)
		if all(c.segs[i].star or c.segs[i].name == parts[i] for i in range(k)):
			if len(parts) <= n:
				return True
			if c.inherits is not None:
				fcs = frags.get(c.inherits)
				if fcs is not None and _chain_parts_legal(fcs, frags, parts[n:]):
					return True
	return False


def _v_suggest(siblings, parent_chain, name):
	"""Closest legal sibling name (same parent chain, schema order, edit
	distance <= 2) as "; did you mean 'x'?" - or nothing. Prose only, never
	contract. The sibling lists are prebuilt once per validate."""
	best = None
	for s in siblings.get(parent_chain, ()):
		dist = _edit_distance(name, s)
		if dist <= 2 and (best is None or dist < best[0]):
			best = (dist, s)
	if best is None:
		return ""
	return "; did you mean '{}'?".format(best[1])
