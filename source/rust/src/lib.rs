// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

//! SHCL reference implementation: parser, accessor, writer/formatter.
//! Single file on purpose - the drop-in story is "copy this file into your tree".
//! The language spec lives in project/spec.md; the conformance corpus in
//! project/conformance/ pins every behavior here.

use std::collections::HashMap;

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

/// Per-document forgiveness knob. Set once at load; composes with per-call onBad.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Strictness {
	Loose,
	#[default]
	Standard,
	Strict,
}

impl Strictness {
	/// Accepts the CLI spellings: loose|standard|strict or 1|2|3.
	pub fn from_arg(s: &str) -> Option<Strictness> {
		match s.to_ascii_lowercase().as_str() {
			"loose" | "1" => Some(Strictness::Loose),
			"standard" | "2" => Some(Strictness::Standard),
			"strict" | "3" => Some(Strictness::Strict),
			_ => None,
		}
	}
}

/// Only `Error` fails a strict load; `Hint` flags legal-but-lookalike input.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity {
	Error,
	Hint,
}

#[derive(Debug, Clone)]
pub struct Diagnostic {
	pub line: usize, // 1-based
	pub severity: Severity,
	pub message: String,
}

/// Read status sentinels. `Empty` is informational - the empty value is still returned.
/// Ordered by severity so a worst-of aggregate is just `max`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Status {
	Good,
	Empty,
	NotFound,
	BadType,
	Multiple,
}

/// Full-tier read result: value plus status plus the original raw text (when the
/// path resolved), so a caller can always recover what was actually in the file.
/// Array reads also carry one status per slot (element, or wildcard instance) in
/// `slots`; `status` is then the worst slot. Scalar reads leave `slots` empty.
#[derive(Debug, Clone)]
pub struct Read<T> {
	pub value: T,
	pub status: Status,
	pub raw: Option<String>,
	pub slots: Vec<Status>,
}

impl<T> Read<T> {
	fn new(value: T, status: Status, raw: Option<String>) -> Read<T> {
		Read {
			value,
			status,
			raw,
			slots: Vec::new(),
		}
	}
	fn with_slots(value: T, status: Status, raw: Option<String>, slots: Vec<Status>) -> Read<T> {
		Read {
			value,
			status,
			raw,
			slots,
		}
	}
	pub fn ok(&self) -> bool {
		matches!(self.status, Status::Good | Status::Empty)
	}
}

#[derive(Debug)]
pub struct LoadError {
	pub diagnostics: Vec<Diagnostic>,
}

impl std::fmt::Display for LoadError {
	fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
		let n = self
			.diagnostics
			.iter()
			.filter(|d| d.severity == Severity::Error)
			.count();
		write!(f, "strict load failed: {} error diagnostic(s)", n)
	}
}

impl std::error::Error for LoadError {}

/// Local (floating) date/time unless a zone suffix was present. Fields mirror
/// what was written: a date-only value has no time, and vice versa.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ShclDateTime {
	pub date: Option<(i32, u32, u32)>,         // (year, month, day)
	pub time: Option<(u32, u32, Option<u32>)>, // (hour, minute, seconds if written)
	pub frac: Option<String>,                  // fractional-second digits as typed
	pub zone: Option<ZoneSpec>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ZoneSpec {
	Utc,
	OffsetMinutes(i32),
}

impl std::fmt::Display for ShclDateTime {
	fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
		if let Some((y, m, d)) = self.date {
			write!(f, "{:04}-{:02}-{:02}", y, m, d)?;
			if self.time.is_some() {
				write!(f, "T")?;
			}
		}
		if let Some((h, mi, s)) = self.time {
			write!(f, "{:02}:{:02}", h, mi)?;
			if let Some(sec) = s {
				write!(f, ":{:02}", sec)?;
			}
			if let Some(fr) = &self.frac {
				write!(f, ".{}", fr)?;
			}
		}
		match self.zone {
			Some(ZoneSpec::Utc) => write!(f, "Z")?,
			Some(ZoneSpec::OffsetMinutes(off)) => {
				let sign = if off < 0 { '-' } else { '+' };
				let a = off.abs();
				write!(f, "{}{:02}:{:02}", sign, a / 60, a % 60)?;
			}
			None => {}
		}
		Ok(())
	}
}

// ---------------------------------------------------------------------------
// In-memory model
// ---------------------------------------------------------------------------
// One rule covers everything: a node is (field-name, value, children); nodes
// merge when (name, value) matches; empty values merge into the wrapper node.

#[derive(Debug, Clone, PartialEq)]
struct Element {
	text: String, // quote-stripped, escapes NOT applied (applied on string read)
	quoted: bool,
}

#[derive(Debug, Clone, PartialEq)]
enum Value {
	Empty,
	Cell(Vec<Element>), // one element = scalar, more = inline array
	Raw {
		content: String,
		info: String,
		fence_char: u8,
		fence_len: usize,
	},
}

impl Value {
	/// Merge key: nodes with equal (name, key) collapse into one.
	fn key(&self) -> String {
		match self {
			Value::Empty => "e".to_string(),
			Value::Cell(els) => {
				// Length-prefix each element so the joined key is injective: a bare
				// NUL separator lets `[a, b]` collide with the single element
				// "a\0b" (NUL is legal in a quoted string), silently merging them.
				let mut k = String::from("c:");
				for e in els {
					k.push_str(&e.text.len().to_string());
					k.push(':');
					k.push_str(&e.text);
				}
				k
			}
			// Info-string is part of identity (a `sql` and a `python` block are
			// different values even with equal bodies); fence style is not. Info is
			// length-prefixed for the same injectivity reason as cell elements.
			Value::Raw { content, info, .. } => format!("r:{}:{}{}", info.len(), info, content),
		}
	}
	/// Human/display form; also what selectors match against (case-sensitive).
	fn display(&self) -> String {
		match self {
			Value::Empty => String::new(),
			Value::Cell(els) => els
				.iter()
				.map(|e| e.text.clone())
				.collect::<Vec<_>>()
				.join(", "),
			Value::Raw { content, .. } => content.clone(),
		}
	}
	fn is_empty(&self) -> bool {
		matches!(self, Value::Empty)
	}
}

#[derive(Debug)]
struct NodeData {
	name: String, // ASCII-folded to lower; non-ASCII never folds
	value: Value,
	children: Vec<usize>,
	parent: usize,
	line: usize,
	star_list: bool,  // value built from stacked "* " lines
	star_mixed: bool, // mix of "* " and field children already diagnosed
	// Comment trivia, verbatim from `#` to end of line. Never part of identity
	// or reads; merged instances concatenate leading, first trailing wins
	// (later ones demote to leading - a canonical line has room for one).
	leading: Vec<String>,
	trailing: String, // empty = none
}

/// A parsed SHCL document: the tree, its diagnostics, and its strictness level.
#[derive(Debug)]
pub struct Document {
	arena: Vec<NodeData>,
	diags: Vec<Diagnostic>,
	strictness: Strictness,
	orphans: Vec<String>, // comments after the last binding line
}

const ROOT: usize = 0;

// ---------------------------------------------------------------------------
// Lexical helpers
// ---------------------------------------------------------------------------

fn fold_name(s: &str) -> String {
	s.to_ascii_lowercase() // folds A-Z only; non-ASCII passes through untouched
}

fn is_bare_name_char(c: char) -> bool {
	c.is_ascii_alphanumeric() || c == '-' || c == '_'
}

/// Split off an unquoted trailing comment: (content, comment from `#` on).
/// A `\` shields the next char throughout. Comments are kept as trivia.
fn split_comment(s: &str) -> (&str, Option<&str>) {
	let mut in_quote: Option<char> = None;
	let mut it = s.char_indices();
	while let Some((byte, c)) = it.next() {
		if c == '\\' {
			it.next();
			continue;
		}
		match in_quote {
			Some(q) if c == q => in_quote = None,
			None if c == '"' || c == '\'' => in_quote = Some(c),
			None if c == '#' => return (&s[..byte], Some(&s[byte..])),
			_ => {}
		}
	}
	(s, None)
}

/// Split on unquoted commas; `\` shields the next char.
fn split_unquoted_commas(s: &str) -> Vec<&str> {
	let mut parts = Vec::new();
	let mut in_quote: Option<char> = None;
	let mut start = 0usize;
	let mut it = s.char_indices();
	while let Some((byte, c)) = it.next() {
		if c == '\\' {
			it.next();
			continue;
		}
		match in_quote {
			Some(q) if c == q => in_quote = None,
			None if c == '"' || c == '\'' => in_quote = Some(c),
			None if c == ',' => {
				parts.push(&s[start..byte]);
				start = byte + 1;
			}
			_ => {}
		}
	}
	parts.push(&s[start..]);
	parts
}

/// A dangling trailing backslash would swallow the separator after it on
/// re-emit; store the doubled spelling instead (identical on string read).
fn normalize_dangling_backslash(mut t: String) -> String {
	let run = t.chars().rev().take_while(|&c| c == '\\').count();
	if run % 2 == 1 {
		t.push('\\');
	}
	t
}

/// Trim, then strip one matching outer quote pair if present. Unquoted empty
/// slots return None (dropped, never an error).
fn parse_element(piece: &str) -> Option<Element> {
	let t = piece.trim();
	if t.is_empty() {
		return None;
	}
	let chars: Vec<char> = t.chars().collect();
	let first = chars[0];
	if (first == '"' || first == '\'') && chars.len() >= 2 && *chars.last().unwrap() == first {
		// The closing quote must not itself be escaped (`"a\"` is not closed).
		let mut esc = false;
		for &c in &chars[1..chars.len() - 1] {
			esc = c == '\\' && !esc;
		}
		if !esc {
			let inner: String = chars[1..chars.len() - 1].iter().collect();
			return Some(Element {
				text: inner,
				quoted: true,
			});
		}
	}
	Some(Element {
		text: normalize_dangling_backslash(t.to_string()),
		quoted: false,
	})
}

fn parse_cell(text: &str) -> Value {
	let mut els = Vec::new();
	for piece in split_unquoted_commas(text) {
		if let Some(e) = parse_element(piece) {
			els.push(e);
		}
	}
	if els.is_empty() {
		Value::Empty
	} else {
		Value::Cell(els)
	}
}

/// Escape processing (string reads): \t \n \\ \" \'; unknown escapes stay literal.
fn apply_escapes(s: &str) -> String {
	let mut out = String::with_capacity(s.len());
	let mut it = s.chars();
	while let Some(c) = it.next() {
		if c != '\\' {
			out.push(c);
			continue;
		}
		match it.next() {
			Some('t') => out.push('\t'),
			Some('n') => out.push('\n'),
			Some('\\') => out.push('\\'),
			Some('"') => out.push('"'),
			Some('\'') => out.push('\''),
			Some(other) => {
				out.push('\\');
				out.push(other);
			}
			None => out.push('\\'),
		}
	}
	out
}

/// Opening fence: a run of >=3 backticks or tildes, then an optional info-string.
fn fence_open(rest: &str) -> Option<(u8, usize, String)> {
	let first = rest.as_bytes().first().copied()?;
	if first != b'`' && first != b'~' {
		return None;
	}
	let run = rest.bytes().take_while(|&b| b == first).count();
	if run < 3 {
		return None;
	}
	Some((first, run, rest[run..].trim().to_string()))
}

fn is_fence_close(line: &str, ch: u8, min_len: usize) -> bool {
	let t = line.trim();
	t.len() >= min_len && !t.is_empty() && t.bytes().all(|b| b == ch)
}

// ---------------------------------------------------------------------------
// Path scanner (shared by file lines and accessor queries)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
enum Selector {
	ByValue(String),
	ByIndex(usize),
	Wildcard,
}

#[derive(Debug, Clone)]
struct Segment {
	name: String, // folded
	selector: Option<Selector>,
}

struct PathScan {
	segments: Vec<Segment>,
	value_text: Option<String>, // text after the separator colon, trimmed
}

/// Scan `a . b : [sel] . c : value`. Whitespace around dots/colons/brackets is
/// insignificant. A colon is a selector colon only when the next non-ws char is
/// `[`; otherwise it separates the value. Err(reason) means genuinely ambiguous
/// input, which the caller skips with a diagnostic.
fn scan_path(input: &str) -> Result<PathScan, String> {
	let chars: Vec<char> = input.chars().collect();
	let mut pos = 0usize;
	fn skip_ws(chars: &[char], pos: &mut usize) {
		while *pos < chars.len() && (chars[*pos] == ' ' || chars[*pos] == '\t') {
			*pos += 1;
		}
	}
	fn read_quoted(chars: &[char], pos: &mut usize) -> Result<String, String> {
		let q = chars[*pos];
		*pos += 1;
		let mut out = String::new();
		loop {
			if *pos >= chars.len() {
				return Err("unterminated quote".into());
			}
			let c = chars[*pos];
			if c == '\\' && *pos + 1 < chars.len() {
				out.push(c);
				out.push(chars[*pos + 1]);
				*pos += 2;
				continue;
			}
			*pos += 1;
			if c == q {
				return Ok(out);
			}
			out.push(c);
		}
	}
	let mut segments: Vec<Segment> = Vec::new();
	loop {
		skip_ws(&chars, &mut pos);
		if pos >= chars.len() {
			return Err("empty path".into());
		}
		// Field name: quoted or bare.
		let name = if chars[pos] == '"' || chars[pos] == '\'' {
			read_quoted(&chars, &mut pos)?
		} else {
			let start = pos;
			while pos < chars.len() && is_bare_name_char(chars[pos]) {
				pos += 1;
			}
			if pos == start {
				return Err(format!("expected field name, found '{}'", chars[pos]));
			}
			chars[start..pos].iter().collect()
		};
		let mut selector: Option<Selector> = None;
		skip_ws(&chars, &mut pos);
		// Optional selector, with its optional sugar colon (colon counts as
		// selector sugar only when the next non-ws char is an open bracket).
		let mut bracket_at: Option<usize> = None;
		if pos < chars.len() && chars[pos] == '[' {
			bracket_at = Some(pos);
		} else if pos < chars.len() && chars[pos] == ':' {
			let mut q = pos + 1;
			skip_ws(&chars, &mut q);
			if q < chars.len() && chars[q] == '[' {
				bracket_at = Some(q);
			}
		}
		if let Some(b) = bracket_at {
			pos = b + 1;
			skip_ws(&chars, &mut pos);
			if pos < chars.len() && (chars[pos] == '"' || chars[pos] == '\'') {
				let v = read_quoted(&chars, &mut pos)?;
				selector = Some(Selector::ByValue(v)); // quotes force a value match, even numeric
			} else {
				let start = pos;
				while pos < chars.len() && chars[pos] != ']' {
					pos += 1;
				}
				let body: String = chars[start..pos]
					.iter()
					.collect::<String>()
					.trim()
					.to_string();
				selector = Some(if body == "*" {
					Selector::Wildcard
				} else if let Some(n) = body.strip_prefix('#').and_then(|d| d.parse::<usize>().ok())
				{
					Selector::ByIndex(n)
				} else if let Ok(n) = body.parse::<usize>() {
					Selector::ByIndex(n)
				} else if body.is_empty() {
					return Err("empty selector".into());
				} else {
					Selector::ByValue(normalize_dangling_backslash(body))
				});
			}
			skip_ws(&chars, &mut pos);
			if pos >= chars.len() || chars[pos] != ']' {
				return Err("unterminated selector".into());
			}
			pos += 1;
			skip_ws(&chars, &mut pos);
		}
		segments.push(Segment {
			name: fold_name(&name),
			selector,
		});
		if pos >= chars.len() {
			return Ok(PathScan {
				segments,
				value_text: None,
			});
		}
		match chars[pos] {
			'.' => {
				pos += 1;
			}
			':' => {
				pos += 1;
				let rest: String = chars[pos..].iter().collect();
				return Ok(PathScan {
					segments,
					value_text: Some(rest.trim().to_string()),
				});
			}
			c => return Err(format!("unexpected '{}' after field", c)),
		}
	}
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

struct Parser {
	arena: Vec<NodeData>,
	diags: Vec<Diagnostic>,
	// (indent string, node) for each open level; [0] is the virtual root.
	stack: Vec<(String, usize)>,
	// Per-node (name, value-key) -> first matching child, parallel to arena.
	// Pure lookup accelerator for select_or_create; children keeps the order.
	child_map: Vec<HashMap<(String, String), usize>>,
	// Whole-line comments waiting for the next line that binds a node.
	pending: Vec<String>,
}

impl Parser {
	fn new() -> Parser {
		Parser {
			arena: vec![NodeData {
				name: String::new(),
				value: Value::Empty,
				children: Vec::new(),
				parent: 0,
				line: 0,
				star_list: false,
				star_mixed: false,
				leading: Vec::new(),
				trailing: String::new(),
			}],
			diags: Vec::new(),
			stack: vec![(String::new(), ROOT)],
			child_map: vec![HashMap::new()],
			pending: Vec::new(),
		}
	}

	fn err(&mut self, line: usize, msg: impl Into<String>) {
		self.diags.push(Diagnostic {
			line,
			severity: Severity::Error,
			message: msg.into(),
		});
	}

	/// Find (or create by merge rule) the child of `parent` with this (name, value).
	fn select_or_create(&mut self, parent: usize, name: &str, value: Value, line: usize) -> usize {
		let map_key = (name.to_string(), value.key());
		if let Some(&c) = self.child_map[parent].get(&map_key) {
			return c;
		}
		let idx = self.arena.len();
		self.arena.push(NodeData {
			name: name.to_string(),
			value,
			children: Vec::new(),
			parent,
			line,
			star_list: false,
			star_mixed: false,
			leading: Vec::new(),
			trailing: String::new(),
		});
		self.arena[parent].children.push(idx);
		self.child_map.push(HashMap::new());
		self.child_map[parent].insert(map_key, idx);
		idx
	}

	/// A node's value mutated in place (empty field filled, star element added):
	/// move its map entry from the old key to the new one. First-wins on both
	/// sides so lookups keep matching the earliest sibling, like the scan did.
	fn remap_child(&mut self, node: usize, old_key: String) {
		let parent = self.arena[node].parent;
		let name = self.arena[node].name.clone();
		if self.child_map[parent].get(&(name.clone(), old_key.clone())) == Some(&node) {
			self.child_map[parent].remove(&(name.clone(), old_key));
		}
		let new_key = self.arena[node].value.key();
		self.child_map[parent]
			.entry((name, new_key))
			.or_insert(node);
	}

	/// Hand pending leading comments (and this line's trailing one) to a node.
	/// First trailing wins; a later one demotes to leading so nothing is lost.
	fn attach_trivia(&mut self, node: usize, trailing: Option<&str>) {
		self.arena[node].leading.append(&mut self.pending);
		if let Some(t) = trailing {
			if self.arena[node].trailing.is_empty() {
				self.arena[node].trailing = t.to_string();
			} else {
				self.arena[node].leading.push(t.to_string());
			}
		}
	}

	/// Resolve which open level this indent belongs to. Child only when the
	/// current top's indent is a proper prefix; otherwise the indent must equal
	/// an open level exactly (dedent), else it is a recoverable error.
	fn resolve_parent(&mut self, indent: &str) -> Option<usize> {
		let (top_indent, top_node) = self.stack.last().unwrap().clone();
		if indent.len() > top_indent.len() && indent.starts_with(&top_indent) {
			return Some(top_node);
		}
		for i in (0..self.stack.len()).rev() {
			if self.stack[i].0 == indent {
				// Sibling of stack[i]: its parent is the entry below it.
				let parent = if i == 0 { ROOT } else { self.stack[i - 1].1 };
				self.stack.truncate(i.max(1));
				// Keep the sentinel; a top-level line resolves to ROOT.
				if i == 0 {
					self.stack.truncate(1);
				}
				return Some(parent);
			}
		}
		None
	}

	/// Walk path segments under `parent`, select-or-creating; returns the node
	/// for the last segment carrying `value`. None aborts the line (diagnosed).
	fn attach_path(
		&mut self,
		parent: usize,
		segs: &[Segment],
		value: Value,
		line: usize,
	) -> Option<usize> {
		// Field child under a stacked list: diagnose the mix once, keep the field.
		if self.arena[parent].star_list && !self.arena[parent].star_mixed {
			self.arena[parent].star_mixed = true;
			self.err(line, "field mixed with list elements");
		}
		let mut cur = parent;
		for (i, seg) in segs.iter().enumerate() {
			let is_last = i + 1 == segs.len();
			match (&seg.selector, is_last) {
				(Some(Selector::ByValue(v)), _) => {
					// Same display() predicate resolve_from uses, so a selector
					// also selects an array-valued instance instead of creating
					// a spurious second one. Create only when nothing matches.
					let found = self.arena[cur].children.iter().copied().find(|&c| {
						self.arena[c].name == seg.name && self.arena[c].value.display() == *v
					});
					cur = match found {
						Some(c) => c,
						None => {
							let disc = Value::Cell(vec![Element {
								text: v.clone(),
								quoted: false,
							}]);
							self.select_or_create(cur, &seg.name, disc, line)
						}
					};
					if is_last && !value.is_empty() {
						// `a.b[X]: v` - the discriminator is the value; a second
						// value has nowhere unambiguous to go.
						self.err(
							line,
							format!("value after selector on '{}' ignored", seg.name),
						);
					}
				}
				(Some(Selector::ByIndex(n)), _) => {
					let matches: Vec<usize> = self.arena[cur]
						.children
						.iter()
						.copied()
						.filter(|&c| self.arena[c].name == seg.name)
						.collect();
					if let Some(&found) = matches.get(*n) {
						cur = found;
					} else {
						self.err(line, format!("no instance {} of '{}'", n, seg.name));
						return None;
					}
				}
				(Some(Selector::Wildcard), _) => {
					self.err(line, "wildcard selector is query-only");
					return None;
				}
				(None, false) => {
					cur = self.select_or_create(cur, &seg.name, Value::Empty, line);
				}
				(None, true) => {
					cur = self.select_or_create(cur, &seg.name, value.clone(), line);
				}
			}
		}
		Some(cur)
	}

	/// Consume raw-block content after an opening fence. Returns (value, next line
	/// index). Content keeps relative indentation; the common leading run is stripped.
	fn consume_raw(
		&mut self,
		lines: &[String],
		mut i: usize,
		open_line: usize,
		ch: u8,
		len: usize,
		info: String,
	) -> (Value, usize) {
		let mut content: Vec<String> = Vec::new();
		let mut closed = false;
		while i < lines.len() {
			if is_fence_close(&lines[i], ch, len) {
				closed = true;
				i += 1;
				break;
			}
			content.push(lines[i].clone());
			i += 1;
		}
		if !closed {
			self.err(open_line, "unterminated raw block");
		}
		// Strip the common leading whitespace (the visual nesting); keep the rest.
		let mut common: Option<String> = None;
		for l in content.iter().filter(|l| !l.trim().is_empty()) {
			let lead: String = l.chars().take_while(|c| *c == ' ' || *c == '\t').collect();
			common = Some(match common {
				None => lead,
				Some(prev) => {
					let mut p = String::new();
					for (a, b) in prev.chars().zip(lead.chars()) {
						if a == b {
							p.push(a);
						} else {
							break;
						}
					}
					p
				}
			});
		}
		let common = common.unwrap_or_default();
		let stripped: Vec<String> = content
			.iter()
			.map(|l| {
				if l.trim().is_empty() {
					String::new()
				} else {
					l.strip_prefix(&common).unwrap_or(l).to_string()
				}
			})
			.collect();
		(
			Value::Raw {
				content: stripped.join("\n"),
				info,
				fence_char: ch,
				fence_len: len,
			},
			i,
		)
	}

	/// A bare fence line is a value line for its parent field: fills an empty
	/// value, else creates a new instance of that field (the repeated-leaf rule).
	/// Returns the node the block landed on (None = no parent, diagnosed).
	fn bind_block(&mut self, parent: usize, value: Value, line: usize) -> Option<usize> {
		if parent == ROOT {
			self.err(line, "raw block with no parent field");
			return None;
		}
		if self.arena[parent].value.is_empty() {
			let old_key = self.arena[parent].value.key();
			self.arena[parent].value = value;
			self.remap_child(parent, old_key);
			Some(parent)
		} else {
			let (name, grandparent) = (self.arena[parent].name.clone(), self.arena[parent].parent);
			Some(self.select_or_create(grandparent, &name, value, line))
		}
	}

	/// One stacked-list element (`* scalar`) appends to the parent's array.
	fn add_star_element(&mut self, parent: usize, body: &str, line: usize) {
		if parent == ROOT {
			self.err(line, "list element with no parent field");
			return;
		}
		// Uniform-or-nothing (spec): a mix with field children is not a block array.
		if !self.arena[parent].children.is_empty() {
			self.err(line, "list element mixed with field children; ignored");
			return;
		}
		let trimmed = body.trim();
		if trimmed.is_empty() {
			self.err(line, "empty list element");
			return;
		}
		// One scalar per line; a bare comma is an error, not a second element.
		if split_unquoted_commas(trimmed).len() > 1 {
			self.err(line, "bare comma in list element (one element per line)");
			return;
		}
		let el = match parse_element(trimmed) {
			Some(e) => e,
			None => {
				self.err(line, "empty list element");
				return;
			}
		};
		let old_key = self.arena[parent].value.key();
		let node = &mut self.arena[parent];
		let mutated = match &mut node.value {
			Value::Empty => {
				node.value = Value::Cell(vec![el]);
				node.star_list = true;
				true
			}
			Value::Cell(els) if node.star_list => {
				els.push(el);
				true
			}
			_ => false,
		};
		if mutated {
			self.remap_child(parent, old_key);
		} else {
			self.err(line, "field already has a value; list element ignored");
		}
	}

	/// Legal input that looks like a common mistake: a field repeating as a bare
	/// scalar leaf. Mandatory hint per spec (never fails a load).
	fn emit_repeated_leaf_hints(&mut self) {
		let mut hints: Vec<(usize, String)> = Vec::new();
		for parent in 0..self.arena.len() {
			// Group by name in first-appearance order: hint order must be
			// deterministic or the cross-binding check can't compare `check` output.
			let mut group_of: HashMap<&str, usize> = HashMap::new();
			let mut by_name: Vec<(&str, Vec<usize>)> = Vec::new();
			for &c in &self.arena[parent].children {
				let name = self.arena[c].name.as_str();
				match group_of.get(name) {
					Some(&g) => by_name[g].1.push(c),
					None => {
						group_of.insert(name, by_name.len());
						by_name.push((name, vec![c]));
					}
				}
			}
			for (name, group) in by_name {
				if group.len() < 2 {
					continue;
				}
				let all_scalar_leaves = group.iter().all(|&c| {
					self.arena[c].children.is_empty()
						&& matches!(self.arena[c].value, Value::Cell(_))
						&& !self.arena[c].star_list
				});
				if all_scalar_leaves {
					let line = group.iter().map(|&c| self.arena[c].line).max().unwrap_or(0);
					let joined = group
						.iter()
						.map(|&c| self.arena[c].value.display())
						.collect::<Vec<_>>()
						.join(", ");
					hints.push((
						line,
						format!(
							"'{}' repeats as a bare leaf - did you mean '{}: {}'?",
							name, name, joined
						),
					));
				}
			}
		}
		for (line, message) in hints {
			self.diags.push(Diagnostic {
				line,
				severity: Severity::Hint,
				message,
			});
		}
	}

	fn parse(mut self, text: &str, strictness: Strictness) -> Document {
		// UTF-8 BOM strip, then split keeping raw lines (CR stripped per line).
		let text = text.strip_prefix('\u{feff}').unwrap_or(text);
		let lines: Vec<String> = text
			.split('\n')
			.map(|l| l.strip_suffix('\r').unwrap_or(l).to_string())
			.collect();
		let mut i = 0usize;
		while i < lines.len() {
			let lineno = i + 1;
			let line = lines[i].trim_end();
			let indent: String = line
				.chars()
				.take_while(|c| *c == ' ' || *c == '\t')
				.collect();
			let rest = &line[indent.len()..];
			if rest.is_empty() {
				i += 1;
				continue;
			}
			// Whole-line comment: hold it for the next line that binds a node.
			if rest.starts_with('#') {
				self.pending.push(rest.to_string());
				i += 1;
				continue;
			}
			// Child-indent fence: a value line for its parent field.
			if let Some((ch, len, info)) = fence_open(rest) {
				let parent = match self.resolve_parent(&indent) {
					Some(p) => p,
					None => {
						self.err(lineno, "indentation matches no open level");
						i += 1;
						continue;
					}
				};
				let (value, next) = self.consume_raw(&lines, i + 1, lineno, ch, len, info);
				if let Some(node) = self.bind_block(parent, value, lineno) {
					self.attach_trivia(node, None);
				}
				i = next;
				continue;
			}
			// Stacked-list element: colon-less by construction ('*' can't begin a name).
			if let Some(after) = rest.strip_prefix('*') {
				if after.starts_with(' ') || after.starts_with('\t') {
					let parent = match self.resolve_parent(&indent) {
						Some(p) => p,
						None => {
							self.err(lineno, "indentation matches no open level");
							i += 1;
							continue;
						}
					};
					let (body, comment) = split_comment(after);
					// Elements have no node of their own; trivia rides the field.
					if parent != ROOT {
						self.attach_trivia(parent, comment);
					}
					self.add_star_element(parent, body, lineno);
					i += 1;
					continue;
				}
				self.err(lineno, "malformed line: '*' must be followed by a space");
				i += 1;
				continue;
			}
			// Field line.
			let (before, comment) = split_comment(rest);
			let content = before.trim_end();
			if content.is_empty() {
				// Only a comment survived (e.g. an escaped lead-in); keep it.
				if let Some(c) = comment {
					self.pending.push(c.to_string());
				}
				i += 1;
				continue;
			}
			let parent = match self.resolve_parent(&indent) {
				Some(p) => p,
				None => {
					self.err(lineno, "indentation matches no open level");
					i += 1;
					continue;
				}
			};
			let scan = match scan_path(content) {
				Ok(s) => s,
				Err(reason) => {
					self.err(lineno, format!("malformed line skipped: {}", reason));
					i += 1;
					continue;
				}
			};
			let mut next = i + 1;
			let value = match &scan.value_text {
				None => {
					// A clean path with no colon is the one defined repair:
					// the obvious intent is that path with an empty value.
					self.err(lineno, "missing colon; repaired as an empty value");
					Value::Empty
				}
				Some(v) if v.is_empty() => Value::Empty,
				Some(v) => {
					if let Some((ch, len, info)) = fence_open(v) {
						// Same-line fence spelling.
						let (val, n) = self.consume_raw(&lines, i + 1, lineno, ch, len, info);
						next = n;
						val
					} else {
						parse_cell(v)
					}
				}
			};
			if let Some(node) = self.attach_path(parent, &scan.segments, value, lineno) {
				self.attach_trivia(node, comment);
				self.stack.push((indent, node));
			}
			i = next;
		}
		self.emit_repeated_leaf_hints();
		Document {
			arena: self.arena,
			diags: self.diags,
			strictness,
			orphans: self.pending,
		}
	}
}

// ---------------------------------------------------------------------------
// Document: load, diagnostics, formatter
// ---------------------------------------------------------------------------

impl Document {
	/// Parse at Standard strictness. Never fails: bad lines are skipped and
	/// diagnosed, good values stay readable.
	pub fn parse(text: &str) -> Document {
		Parser::new().parse(text, Strictness::Standard)
	}

	/// Parse at a chosen strictness. Only Strict can fail (any error diagnostic).
	pub fn parse_with(text: &str, strictness: Strictness) -> Result<Document, LoadError> {
		let doc = Parser::new().parse(text, strictness);
		if strictness == Strictness::Strict
			&& doc.diags.iter().any(|d| d.severity == Severity::Error)
		{
			return Err(LoadError {
				diagnostics: doc.diags,
			});
		}
		Ok(doc)
	}

	pub fn diagnostics(&self) -> &[Diagnostic] {
		&self.diags
	}

	pub fn strictness(&self) -> Strictness {
		self.strictness
	}

	/// Canonical form: block layout, tabs, insertion order, minimal quoting,
	/// redundancy collapsed, comments re-emitted as attached trivia. Scalar
	/// text is never rewritten.
	pub fn to_canonical(&self) -> String {
		let mut out = String::new();
		for &c in &self.arena[ROOT].children {
			self.emit_node(c, 0, &mut out);
		}
		// Comments that never found a following line re-emit at the end.
		for c in &self.orphans {
			out.push_str(c);
			out.push('\n');
		}
		out
	}

	fn emit_node(&self, idx: usize, depth: usize, out: &mut String) {
		let node = &self.arena[idx];
		let pad: String = "\t".repeat(depth);
		// Same-line fence spelling can't carry an inline comment (an unbalanced
		// quote in the info-string could hide the `#` on reparse), so its
		// trailing comment joins the leading lines instead.
		let would_merge = if let Value::Raw { .. } = &node.value {
			let parent = node.parent;
			let me = self.arena[parent].children.iter().position(|&c| c == idx);
			me.is_some_and(|p| {
				self.arena[parent].children[..p]
					.iter()
					.any(|&c| self.arena[c].name == node.name && self.arena[c].value.is_empty())
			})
		} else {
			false
		};
		for c in &node.leading {
			out.push_str(&pad);
			out.push_str(c);
			out.push('\n');
		}
		if would_merge && !node.trailing.is_empty() {
			out.push_str(&pad);
			out.push_str(&node.trailing);
			out.push('\n');
		}
		out.push_str(&pad);
		out.push_str(&emit_name(&node.name));
		out.push(':');
		match &node.value {
			Value::Empty => {
				push_trailing(out, &node.trailing);
				out.push('\n');
			}
			Value::Cell(els) => {
				out.push(' ');
				let joined = els.iter().map(emit_element).collect::<Vec<_>>().join(", ");
				out.push_str(&joined);
				push_trailing(out, &node.trailing);
				out.push('\n');
			}
			Value::Raw {
				content,
				info,
				fence_char,
				fence_len,
			} => {
				// Child-indent spelling is canonical: bare name line, fenced
				// block one level deeper, verbatim content. Exception: if an
				// earlier same-name sibling is empty, the bare `name:` header
				// would merge into it on reparse and the fence would fill that
				// instance instead - so use the same-line spelling there.
				if would_merge {
					out.push(' ');
				} else {
					push_trailing(out, &node.trailing);
					out.push('\n');
				}
				let pad: String = "\t".repeat(depth + 1); // block body pad, one deeper
				let fence: String = std::iter::repeat_n(*fence_char as char, *fence_len).collect();
				if !would_merge {
					out.push_str(&pad);
				}
				out.push_str(&fence);
				if !info.is_empty() {
					// An info-string starting with the fence char would extend
					// the run on reparse; a space keeps the fence length intact.
					if info.as_bytes()[0] == *fence_char {
						out.push(' ');
					}
					out.push_str(info);
				}
				out.push('\n');
				if !content.is_empty() {
					for l in content.split('\n') {
						if !l.is_empty() {
							out.push_str(&pad);
						}
						out.push_str(l);
						out.push('\n');
					}
				}
				out.push_str(&pad);
				out.push_str(&fence);
				out.push('\n');
			}
		}
		for &c in &self.arena[idx].children {
			self.emit_node(c, depth + 1, out);
		}
	}
}

/// Inline comment, canonically two spaces before the `#`.
fn push_trailing(out: &mut String, trailing: &str) {
	if !trailing.is_empty() {
		out.push_str("  ");
		out.push_str(trailing);
	}
}

fn emit_name(name: &str) -> String {
	if !name.is_empty() && name.chars().all(is_bare_name_char) {
		name.to_string()
	} else {
		quote_text(name)
	}
}

/// Minimal quoting: bare unless a reserved character (or lookalike hazard) forces it.
fn emit_element(e: &Element) -> String {
	let t = &e.text;
	let needs = t.is_empty()
		|| t.chars()
			.any(|c| matches!(c, ' ' | '\t' | ',' | ':' | '#' | '"' | '\'' | '[' | ']'))
		|| fence_open(t).is_some();
	if needs { quote_text(t) } else { t.clone() }
}

/// Quote chars that are NOT already escaped in the raw text; escaped ones must
/// stay untouched or every round-trip would re-escape them.
fn bare_quote_counts(t: &str) -> (usize, usize) {
	let (mut dq, mut sq) = (0usize, 0usize);
	let mut it = t.chars();
	while let Some(c) = it.next() {
		match c {
			'\\' => {
				it.next();
			}
			'"' => dq += 1,
			'\'' => sq += 1,
			_ => {}
		}
	}
	(dq, sq)
}

fn quote_text(t: &str) -> String {
	let (dq, sq) = bare_quote_counts(t);
	if dq == 0 {
		format!("\"{}\"", t)
	} else if sq == 0 {
		format!("'{}'", t)
	} else {
		// Both quote kinds appear bare: escape the doubles, wrap in doubles.
		let mut out = String::from("\"");
		let mut it = t.chars();
		while let Some(c) = it.next() {
			match c {
				'\\' => {
					out.push(c);
					if let Some(n) = it.next() {
						out.push(n);
					}
				}
				'"' => out.push_str("\\\""),
				_ => out.push(c),
			}
		}
		out.push('"');
		out
	}
}

// ---------------------------------------------------------------------------
// Accessor: path resolution
// ---------------------------------------------------------------------------

enum Resolved {
	None,
	One(usize),
	Many(Vec<usize>),
	// Wildcard: one slot per instance, in file order; Err = why the sub-path
	// did not land on one node (NotFound missing, Multiple ambiguous).
	Slots(Vec<Result<usize, Status>>),
}

impl Document {
	fn children_named(&self, parent: usize, name: &str) -> Vec<usize> {
		self.arena[parent]
			.children
			.iter()
			.copied()
			.filter(|&c| self.arena[c].name == name)
			.collect()
	}

	fn resolve_from(&self, start: &[usize], segs: &[Segment]) -> Resolved {
		let mut cur: Vec<usize> = start.to_vec();
		for (i, seg) in segs.iter().enumerate() {
			let mut next: Vec<usize> = Vec::new();
			for &n in &cur {
				next.extend(self.children_named(n, &seg.name));
			}
			match &seg.selector {
				None => cur = next,
				Some(Selector::ByValue(v)) => {
					cur = next
						.into_iter()
						.filter(|&c| self.arena[c].value.display() == *v)
						.collect();
				}
				Some(Selector::ByIndex(k)) => {
					cur = next.get(*k).map(|&c| vec![c]).unwrap_or_default();
				}
				Some(Selector::Wildcard) => {
					// Remaining path resolves per-instance; slots stay aligned.
					let rest = &segs[i + 1..];
					let mut slots: Vec<Result<usize, Status>> = Vec::new();
					for inst in next {
						if rest.is_empty() {
							slots.push(Ok(inst));
						} else {
							match self.resolve_from(&[inst], rest) {
								Resolved::One(x) => slots.push(Ok(x)),
								Resolved::None => slots.push(Err(Status::NotFound)),
								_ => slots.push(Err(Status::Multiple)),
							}
						}
					}
					return Resolved::Slots(slots);
				}
			}
		}
		match cur.len() {
			0 => Resolved::None,
			1 => Resolved::One(cur[0]),
			_ => Resolved::Many(cur),
		}
	}

	fn resolve(&self, path: &str) -> Result<Resolved, Status> {
		let scan = scan_path(path).map_err(|_| Status::NotFound)?;
		if scan.value_text.is_some() {
			return Err(Status::NotFound); // a query has no value part
		}
		Ok(self.resolve_from(&[ROOT], &scan.segments))
	}

	/// Instance count at a path (0 when nothing matches).
	pub fn count(&self, path: &str) -> usize {
		match self.resolve(path) {
			Ok(Resolved::None) | Err(_) => 0,
			Ok(Resolved::One(_)) => 1,
			Ok(Resolved::Many(v)) => v.len(),
			Ok(Resolved::Slots(s)) => s.len(),
		}
	}

	/// Instance values at a path, in file order. Wildcard slots that did not
	/// resolve stay in the list as "" so indices keep matching count().
	pub fn instances(&self, path: &str) -> Vec<String> {
		match self.resolve(path) {
			Ok(Resolved::One(n)) => vec![self.arena[n].value.display()],
			Ok(Resolved::Many(v)) => v.iter().map(|&n| self.arena[n].value.display()).collect(),
			Ok(Resolved::Slots(s)) => s
				.into_iter()
				.map(|r| match r {
					Ok(n) => self.arena[n].value.display(),
					Err(_) => String::new(),
				})
				.collect(),
			_ => Vec::new(),
		}
	}
}

// ---------------------------------------------------------------------------
// Writer: typed emit, defaults, comments, structural edits
// ---------------------------------------------------------------------------
// The reverse of the Accessor. A setter builds the canonical stored text for a
// typed value (the inverse of the matching read) and places it at a path,
// creating intermediate nodes on the way. Reads and to_canonical walk children
// vecs, so mutating the arena directly is enough - the parser's child_map is
// already gone and is not maintained here.

fn cell_of(text: String) -> Value {
	Value::Cell(vec![Element {
		text,
		quoted: false,
	}])
}

/// Encode a logical string into stored element text so a scalar read
/// (apply_escapes) hands it back verbatim and an emit/reparse round-trips. Only
/// backslash, newline, and tab need encoding; emit_element wraps quote/reserved
/// chars itself, and reparse strips that wrapping.
fn encode_string(s: &str) -> String {
	let mut out = String::with_capacity(s.len());
	for c in s.chars() {
		match c {
			'\\' => out.push_str("\\\\"),
			'\n' => out.push_str("\\n"),
			'\t' => out.push_str("\\t"),
			_ => out.push(c),
		}
	}
	out
}

/// Pick a backtick fence long enough that no content line closes it early.
fn choose_fence(content: &str) -> (u8, usize) {
	let mut maxrun = 0usize;
	for line in content.split('\n') {
		let t = line.trim();
		if !t.is_empty() && t.bytes().all(|b| b == b'`') {
			maxrun = maxrun.max(t.len());
		}
	}
	(b'`', (maxrun + 1).max(3))
}

/// Inline-array value; the empty array is an empty value (reads back Empty).
fn array_cell(texts: Vec<String>) -> Value {
	if texts.is_empty() {
		Value::Empty
	} else {
		Value::Cell(
			texts
				.into_iter()
				.map(|text| Element {
					text,
					quoted: false,
				})
				.collect(),
		)
	}
}

impl Document {
	/// A fresh document with no bindings - the start point for schema-driven
	/// generation. Loads at Standard; set values, then to_canonical().
	pub fn new() -> Document {
		Document::parse("")
	}

	fn new_child(&mut self, parent: usize, name: &str, value: Value) -> usize {
		let idx = self.arena.len();
		self.arena.push(NodeData {
			name: name.to_string(),
			value,
			children: Vec::new(),
			parent,
			line: 0,
			star_list: false,
			star_mixed: false,
			leading: Vec::new(),
			trailing: String::new(),
		});
		self.arena[parent].children.push(idx);
		idx
	}

	fn child_or_create(&mut self, parent: usize, name: &str) -> usize {
		match self.arena[parent]
			.children
			.iter()
			.copied()
			.find(|&c| self.arena[c].name == name)
		{
			Some(c) => c,
			None => self.new_child(parent, name, Value::Empty),
		}
	}

	/// Walk (creating as needed) to the node a write targets. A trailing name
	/// with no selector hits the first same-named instance (or a new one); a
	/// `[value]` selector selects the matching instance or creates it; `[#k]`
	/// must already exist. None = path unusable for a write (bad scan, a value
	/// part, a wildcard, or a missing indexed instance).
	fn place(&mut self, path: &str) -> Option<usize> {
		let scan = scan_path(path).ok()?;
		if scan.value_text.is_some() || scan.segments.is_empty() {
			return None;
		}
		let mut cur = ROOT;
		for seg in &scan.segments {
			cur = match &seg.selector {
				None => self.child_or_create(cur, &seg.name),
				Some(Selector::ByValue(v)) => {
					let found = self.arena[cur].children.iter().copied().find(|&c| {
						self.arena[c].name == seg.name && self.arena[c].value.display() == *v
					});
					match found {
						Some(c) => c,
						None => self.new_child(cur, &seg.name, cell_of(v.clone())),
					}
				}
				Some(Selector::ByIndex(k)) => {
					let matches: Vec<usize> = self.arena[cur]
						.children
						.iter()
						.copied()
						.filter(|&c| self.arena[c].name == seg.name)
						.collect();
					*matches.get(*k)?
				}
				Some(Selector::Wildcard) => return None,
			};
		}
		Some(cur)
	}

	fn set_value(&mut self, path: &str, value: Value) {
		if let Some(node) = self.place(path) {
			self.arena[node].value = value;
		}
	}

	/// True when the path resolves to at least one real node.
	pub fn exists(&self, path: &str) -> bool {
		match self.resolve(path) {
			Ok(Resolved::One(_)) | Ok(Resolved::Many(_)) => true,
			Ok(Resolved::Slots(s)) => s.iter().any(|r| r.is_ok()),
			_ => false,
		}
	}

	/// Delete the node(s) at a path (with their subtrees); returns how many.
	pub fn remove(&mut self, path: &str) -> usize {
		let targets: Vec<usize> = match self.resolve(path) {
			Ok(Resolved::One(n)) => vec![n],
			Ok(Resolved::Many(v)) => v,
			Ok(Resolved::Slots(s)) => s.into_iter().filter_map(|r| r.ok()).collect(),
			_ => Vec::new(),
		};
		for &t in &targets {
			let p = self.arena[t].parent;
			self.arena[p].children.retain(|&c| c != t);
		}
		targets.len()
	}

	/// Attach a leading comment line to the node at a path (creating an empty
	/// node if it does not exist yet, so a section can be annotated). A missing
	/// `#` is added; only the first line is kept (a comment is one line).
	pub fn set_comment(&mut self, path: &str, text: &str) {
		if let Some(node) = self.place(path) {
			let line = text.split('\n').next().unwrap_or("");
			let c = if line.starts_with('#') {
				line.to_string()
			} else {
				format!("# {}", line)
			};
			self.arena[node].leading.push(c);
		}
	}

	pub fn set_int(&mut self, path: &str, v: i64) {
		self.set_value(path, cell_of(v.to_string()));
	}
	pub fn set_float(&mut self, path: &str, v: f64) {
		self.set_value(path, cell_of(format!("{}", v)));
	}
	pub fn set_bool(&mut self, path: &str, v: bool) {
		self.set_value(path, cell_of(if v { "true" } else { "false" }.to_string()));
	}
	pub fn set_string(&mut self, path: &str, v: &str) {
		self.set_value(path, cell_of(encode_string(v)));
	}
	pub fn set_datetime(&mut self, path: &str, v: &ShclDateTime) {
		self.set_value(path, cell_of(v.to_string()));
	}
	pub fn set_raw(&mut self, path: &str, content: &str, info: &str) {
		let (fence_char, fence_len) = choose_fence(content);
		self.set_value(
			path,
			Value::Raw {
				content: content.to_string(),
				info: info.to_string(),
				fence_char,
				fence_len,
			},
		);
	}
	pub fn set_empty(&mut self, path: &str) {
		self.set_value(path, Value::Empty);
	}

	pub fn set_int_array(&mut self, path: &str, v: &[i64]) {
		self.set_value(path, array_cell(v.iter().map(|x| x.to_string()).collect()));
	}
	pub fn set_float_array(&mut self, path: &str, v: &[f64]) {
		self.set_value(
			path,
			array_cell(v.iter().map(|x| format!("{}", x)).collect()),
		);
	}
	pub fn set_bool_array(&mut self, path: &str, v: &[bool]) {
		self.set_value(
			path,
			array_cell(
				v.iter()
					.map(|x| if *x { "true" } else { "false" }.to_string())
					.collect(),
			),
		);
	}
	pub fn set_string_array(&mut self, path: &str, v: &[&str]) {
		self.set_value(
			path,
			array_cell(v.iter().map(|x| encode_string(x)).collect()),
		);
	}
	pub fn set_datetime_array(&mut self, path: &str, v: &[ShclDateTime]) {
		self.set_value(path, array_cell(v.iter().map(|x| x.to_string()).collect()));
	}

	// Default (only-if-absent) forms - the "emit defaults" half of the Writer.
	pub fn set_int_default(&mut self, path: &str, v: i64) {
		if !self.exists(path) {
			self.set_int(path, v);
		}
	}
	pub fn set_float_default(&mut self, path: &str, v: f64) {
		if !self.exists(path) {
			self.set_float(path, v);
		}
	}
	pub fn set_bool_default(&mut self, path: &str, v: bool) {
		if !self.exists(path) {
			self.set_bool(path, v);
		}
	}
	pub fn set_string_default(&mut self, path: &str, v: &str) {
		if !self.exists(path) {
			self.set_string(path, v);
		}
	}
	pub fn set_datetime_default(&mut self, path: &str, v: &ShclDateTime) {
		if !self.exists(path) {
			self.set_datetime(path, v);
		}
	}
	pub fn set_raw_default(&mut self, path: &str, content: &str, info: &str) {
		if !self.exists(path) {
			self.set_raw(path, content, info);
		}
	}
	pub fn set_int_array_default(&mut self, path: &str, v: &[i64]) {
		if !self.exists(path) {
			self.set_int_array(path, v);
		}
	}
	pub fn set_float_array_default(&mut self, path: &str, v: &[f64]) {
		if !self.exists(path) {
			self.set_float_array(path, v);
		}
	}
	pub fn set_bool_array_default(&mut self, path: &str, v: &[bool]) {
		if !self.exists(path) {
			self.set_bool_array(path, v);
		}
	}
	pub fn set_string_array_default(&mut self, path: &str, v: &[&str]) {
		if !self.exists(path) {
			self.set_string_array(path, v);
		}
	}
	pub fn set_datetime_array_default(&mut self, path: &str, v: &[ShclDateTime]) {
		if !self.exists(path) {
			self.set_datetime_array(path, v);
		}
	}
}

impl Default for Document {
	fn default() -> Document {
		Document::new()
	}
}

// ---------------------------------------------------------------------------
// Coercion ("intelligent but safe"; Loose re-admits a closed list of tricks)
// ---------------------------------------------------------------------------

const CURRENCY: &[char] = &[
	'$', '¢', '£', '¤', '¥', '₩', '₪', '₫', '€', '₭', '₮', '₱', '₲', '₴', '₹', '₺', '₼', '₽', '₾',
	'₿',
];

fn strip_currency(t: &str) -> &str {
	let mut it = t.chars();
	match it.next() {
		Some(c) if CURRENCY.contains(&c) => it.as_str(),
		_ => t,
	}
}

fn parse_int_text(e: &Element, level: Strictness) -> Option<i64> {
	let mut t = e.text.trim();
	if level == Strictness::Loose {
		t = strip_currency(t);
	}
	// Plain decimal.
	let body = t.strip_prefix(['+', '-']).unwrap_or(t);
	if !body.is_empty() && body.bytes().all(|b| b.is_ascii_digit()) {
		return t.parse::<i64>().ok();
	}
	// Hex.
	let (neg, hex) = match t.strip_prefix('-') {
		Some(r) => (true, r),
		None => (false, t.strip_prefix('+').unwrap_or(t)),
	};
	if let Some(h) = hex.strip_prefix("0x").or_else(|| hex.strip_prefix("0X"))
		&& !h.is_empty()
		&& h.bytes().all(|b| b.is_ascii_hexdigit())
	{
		let v = i64::from_str_radix(h, 16).ok()?;
		return Some(if neg { -v } else { v });
	}
	// Thousands separators, only inside quotes (bare commas are reserved).
	if e.quoted && t.contains(',') {
		let sign_body = t.strip_prefix(['+', '-']).unwrap_or(t);
		let groups: Vec<&str> = sign_body.split(',').collect();
		let well_formed = groups.len() > 1
			&& !groups[0].is_empty()
			&& groups[0].len() <= 3
			&& groups[0].bytes().all(|b| b.is_ascii_digit())
			&& groups[1..]
				.iter()
				.all(|g| g.len() == 3 && g.bytes().all(|b| b.is_ascii_digit()));
		if well_formed {
			return t.replace(',', "").parse::<i64>().ok();
		}
	}
	// Loose: a float (including %) rounds, half away from zero.
	if level == Strictness::Loose
		&& let Some(f) = parse_float_text(e, level)
	{
		let r = f.round();
		if r >= i64::MIN as f64 && r <= i64::MAX as f64 {
			return Some(r as i64);
		}
	}
	None
}

fn float_shape_ok(t: &str) -> bool {
	let body = t.strip_prefix(['+', '-']).unwrap_or(t);
	if body.is_empty() {
		return false;
	}
	let (mantissa, exp) = match body.split_once(['e', 'E']) {
		Some((m, x)) => (m, Some(x)),
		None => (body, None),
	};
	if let Some(x) = exp {
		let xb = x.strip_prefix(['+', '-']).unwrap_or(x);
		if xb.is_empty() || !xb.bytes().all(|b| b.is_ascii_digit()) {
			return false;
		}
	}
	let (int_part, frac_part) = match mantissa.split_once('.') {
		Some((a, b)) => (a, b),
		None => (mantissa, ""),
	};
	if int_part.is_empty() && frac_part.is_empty() {
		return false;
	}
	int_part.bytes().all(|b| b.is_ascii_digit()) && frac_part.bytes().all(|b| b.is_ascii_digit())
}

fn parse_float_text(e: &Element, level: Strictness) -> Option<f64> {
	let mut t = e.text.trim();
	let mut percent = false;
	if level == Strictness::Loose {
		t = strip_currency(t);
		if let Some(inner) = t.strip_suffix('%') {
			t = inner.trim_end();
			percent = true;
		}
	}
	let v = if float_shape_ok(t) {
		t.parse::<f64>().ok()?
	} else {
		// An integer is a valid float on read (incl. hex and quoted thousands).
		let el = Element {
			text: t.to_string(),
			quoted: e.quoted,
		};
		parse_int_text_no_loose(&el)? as f64
	};
	Some(if percent { v / 100.0 } else { v })
}

/// Integer forms only (no Loose float fallback) - used by the float path so the
/// two can't recurse into each other.
fn parse_int_text_no_loose(e: &Element) -> Option<i64> {
	parse_int_text(e, Strictness::Standard)
}

fn parse_bool_text(t: &str, level: Strictness) -> Option<bool> {
	let s = t.trim().to_ascii_lowercase();
	match (level, s.as_str()) {
		(_, "true") => Some(true),
		(_, "false") => Some(false),
		(Strictness::Strict, _) => None,
		(_, "yes") | (_, "on") | (_, "1") => Some(true),
		(_, "no") | (_, "off") | (_, "0") => Some(false),
		(Strictness::Loose, "t")
		| (Strictness::Loose, "y")
		| (Strictness::Loose, "enable")
		| (Strictness::Loose, "enabled") => Some(true),
		(Strictness::Loose, "f")
		| (Strictness::Loose, "n")
		| (Strictness::Loose, "disable")
		| (Strictness::Loose, "disabled") => Some(false),
		_ => None,
	}
}

// ---------------------------------------------------------------------------
// Date/time (closed whitelist; shape match, then calendar validation)
// ---------------------------------------------------------------------------

const MONTHS: &[(&str, u32)] = &[
	("jan", 1),
	("feb", 2),
	("mar", 3),
	("apr", 4),
	("may", 5),
	("jun", 6),
	("jul", 7),
	("aug", 8),
	("sep", 9),
	("oct", 10),
	("nov", 11),
	("dec", 12),
	("january", 1),
	("february", 2),
	("march", 3),
	("april", 4),
	("june", 6),
	("july", 7),
	("august", 8),
	("september", 9),
	("october", 10),
	("november", 11),
	("december", 12),
];

fn month_from_name(s: &str) -> Option<u32> {
	let l = s.to_ascii_lowercase();
	MONTHS.iter().find(|(n, _)| *n == l).map(|(_, m)| *m)
}

fn days_in_month(y: i32, m: u32) -> u32 {
	match m {
		1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
		4 | 6 | 9 | 11 => 30,
		2 => {
			if (y % 4 == 0 && y % 100 != 0) || y % 400 == 0 {
				29
			} else {
				28
			}
		}
		_ => 0,
	}
}

fn valid_date(y: i32, m: u32, d: u32) -> bool {
	(1..=12).contains(&m) && d >= 1 && d <= days_in_month(y, m)
}

fn parse_date_part(s: &str) -> Option<(i32, u32, u32)> {
	let s = s.trim();
	// Compact 8-digit YYYYMMDD.
	if s.len() == 8 && s.bytes().all(|b| b.is_ascii_digit()) {
		let y: i32 = s[..4].parse().ok()?;
		let m: u32 = s[4..6].parse().ok()?;
		let d: u32 = s[6..8].parse().ok()?;
		return valid_date(y, m, d).then_some((y, m, d));
	}
	// Space-separated named-month forms; a comma may follow the day in "Mon DD, YYYY".
	let toks: Vec<&str> = s.split_whitespace().collect();
	if toks.len() == 3 {
		if let Some(m) = month_from_name(toks[0]) {
			let day_tok = toks[1].strip_suffix(',').unwrap_or(toks[1]);
			let d: u32 = day_tok.parse().ok()?;
			let y: i32 = parse_year4(toks[2])?;
			return valid_date(y, m, d).then_some((y, m, d));
		}
		if let Some(m) = month_from_name(toks[1]) {
			let d: u32 = toks[0].parse().ok()?;
			let y: i32 = parse_year4(toks[2])?;
			return valid_date(y, m, d).then_some((y, m, d));
		}
		return None;
	}
	if toks.len() != 1 {
		return None;
	}
	// Delimited forms: one of - / . used uniformly.
	let delim = s.chars().find(|c| matches!(c, '-' | '/' | '.'))?;
	let parts: Vec<&str> = s.split(delim).collect();
	if parts.len() != 3 || parts.iter().any(|p| p.is_empty()) {
		return None;
	}
	// The delimiter must be uniform: no other delimiter chars anywhere.
	if s.chars().filter(|c| matches!(c, '-' | '/' | '.')).count() != 2 {
		return None;
	}
	if parts[0].len() == 4 && parts[0].bytes().all(|b| b.is_ascii_digit()) {
		// Year-first all-numeric.
		let y: i32 = parts[0].parse().ok()?;
		let m: u32 = parse_num2(parts[1])?;
		let d: u32 = parse_num2(parts[2])?;
		return valid_date(y, m, d).then_some((y, m, d));
	}
	if let Some(m) = month_from_name(parts[0]) {
		let d: u32 = parse_num2(parts[1])?;
		let y: i32 = parse_year4(parts[2])?;
		return valid_date(y, m, d).then_some((y, m, d));
	}
	if let Some(m) = month_from_name(parts[1]) {
		let d: u32 = parse_num2(parts[0])?;
		let y: i32 = parse_year4(parts[2])?;
		return valid_date(y, m, d).then_some((y, m, d));
	}
	None // everything else (MM/DD/YYYY, 2-digit years, epoch) is rejected by decision
}

fn parse_year4(s: &str) -> Option<i32> {
	(s.len() == 4 && s.bytes().all(|b| b.is_ascii_digit())).then(|| s.parse().ok())?
}

fn parse_num2(s: &str) -> Option<u32> {
	((s.len() == 1 || s.len() == 2) && s.bytes().all(|b| b.is_ascii_digit()))
		.then(|| s.parse().ok())?
}

/// (hour, minute, seconds-if-written), fraction digits, zone.
type TimeParts = ((u32, u32, Option<u32>), Option<String>, Option<ZoneSpec>);

/// Time with optional meridiem, fraction, zone: `H:MM[:SS[.f+]][ AM|PM][Z|+HH:MM]`.
fn parse_time_part(s: &str) -> Option<TimeParts> {
	let mut t = s.trim();
	// Zone suffix first (only valid after a time).
	let mut zone: Option<ZoneSpec> = None;
	if let Some(rest) = t.strip_suffix(['Z', 'z']) {
		zone = Some(ZoneSpec::Utc);
		t = rest.trim_end();
	} else if t.len() >= 6 {
		// Byte-wise on purpose: a str slice here can land mid-char and panic when
		// the tail holds multibyte text. All-ASCII match implies the cut is a
		// char boundary, so the later &t[..len-6] is safe.
		let tail = &t.as_bytes()[t.len() - 6..];
		let sign = tail[0];
		if (sign == b'+' || sign == b'-')
			&& tail[1].is_ascii_digit()
			&& tail[2].is_ascii_digit()
			&& tail[3] == b':'
			&& tail[4].is_ascii_digit()
			&& tail[5].is_ascii_digit()
		{
			let hh = i32::from(tail[1] - b'0') * 10 + i32::from(tail[2] - b'0');
			let mm = i32::from(tail[4] - b'0') * 10 + i32::from(tail[5] - b'0');
			if hh <= 23 && mm <= 59 {
				let mut off = hh * 60 + mm;
				if sign == b'-' {
					off = -off;
				}
				zone = Some(ZoneSpec::OffsetMinutes(off));
				t = t[..t.len() - 6].trim_end();
			}
		}
	}
	// Meridiem: mandatory minutes already implied by the H:MM shape; dotted
	// a.m. is rejected (the '.' fails the digit checks below).
	let mut meridiem: Option<bool> = None; // true = PM
	let lower = t.to_ascii_lowercase();
	if let Some(rest) = lower.strip_suffix("am") {
		meridiem = Some(false);
		t = &t[..rest.trim_end().len()];
	} else if let Some(rest) = lower.strip_suffix("pm") {
		meridiem = Some(true);
		t = &t[..rest.trim_end().len()];
	}
	let t = t.trim_end();
	// Fraction: only after seconds, '.' delimiter, 1-9 digits.
	let (hms, frac) = match t.split_once('.') {
		Some((a, f)) => {
			if f.is_empty() || f.len() > 9 || !f.bytes().all(|b| b.is_ascii_digit()) {
				return None;
			}
			(a, Some(f.to_string()))
		}
		None => (t, None),
	};
	let parts: Vec<&str> = hms.split(':').collect();
	if parts.len() < 2 || parts.len() > 3 {
		return None;
	}
	if frac.is_some() && parts.len() != 3 {
		return None; // fraction can only follow HH:MM:SS
	}
	let h_raw: u32 = parse_num2(parts[0])?;
	let mi: u32 = (parts[1].len() == 2)
		.then(|| parse_num2(parts[1]))
		.flatten()?;
	let sec: Option<u32> = match parts.get(2) {
		Some(p) => Some((p.len() == 2).then(|| parse_num2(p)).flatten()?),
		None => None,
	};
	if mi > 59 || sec.is_some_and(|x| x > 59) {
		return None;
	}
	let h = match meridiem {
		None => {
			if h_raw > 23 {
				return None;
			}
			h_raw
		}
		Some(pm) => {
			if !(1..=12).contains(&h_raw) {
				return None;
			}
			match (pm, h_raw) {
				(false, 12) => 0,
				(false, x) => x,
				(true, 12) => 12,
				(true, x) => x + 12,
			}
		}
	};
	Some(((h, mi, sec), frac, zone))
}

/// Whole-value date/time parse per the whitelist. None = BadType.
pub fn parse_datetime(text: &str) -> Option<ShclDateTime> {
	let t = text.trim();
	if t.is_empty() {
		return None;
	}
	if let Some(colon) = t.find(':') {
		// Scan back over the 1-2 hour digits to find where the time starts.
		let bytes = t.as_bytes();
		let mut k = colon;
		while k > 0 && bytes[k - 1].is_ascii_digit() && colon - k < 2 {
			k -= 1;
		}
		if k == colon {
			return None; // ':' with no hour digits before it
		}
		if k == 0 {
			// Time-only value.
			let ((h, mi, s), frac, zone) = parse_time_part(t)?;
			return Some(ShclDateTime {
				date: None,
				time: Some((h, mi, s)),
				frac,
				zone,
			});
		}
		// Combined: one separator char between date and time.
		let sep = t[..k].chars().last()?;
		if !matches!(sep, 'T' | 't' | ' ' | '_' | '-' | '/' | '.') {
			return None;
		}
		let date_str = &t[..k - sep.len_utf8()];
		let date = parse_date_part(date_str)?;
		let ((h, mi, s), frac, zone) = parse_time_part(&t[k..])?;
		return Some(ShclDateTime {
			date: Some(date),
			time: Some((h, mi, s)),
			frac,
			zone,
		});
	}
	// Date-only.
	let date = parse_date_part(t)?;
	Some(ShclDateTime {
		date: Some(date),
		time: None,
		frac: None,
		zone: None,
	})
}

// ---------------------------------------------------------------------------
// Accessor: typed reads
// ---------------------------------------------------------------------------

impl Document {
	/// Single-node value at a path, or the failing status.
	fn value_at(&self, path: &str) -> Result<&Value, Status> {
		match self.resolve(path)? {
			Resolved::None => Err(Status::NotFound),
			Resolved::Many(_) | Resolved::Slots(_) => Err(Status::Multiple),
			Resolved::One(n) => Ok(&self.arena[n].value),
		}
	}

	fn scalar_element<'a>(&self, v: &'a Value) -> Result<&'a Element, Status> {
		match v {
			Value::Empty => Err(Status::Empty),
			Value::Raw { .. } => Err(Status::BadType),
			Value::Cell(els) if els.len() == 1 => Ok(&els[0]),
			Value::Cell(_) => Err(Status::BadType), // an array is not one scalar
		}
	}

	fn read_scalar<T: Default>(
		&self,
		path: &str,
		coerce: impl Fn(&Element) -> Option<T>,
	) -> Read<T> {
		let value = match self.value_at(path) {
			Ok(v) => v,
			Err(st) => return Read::new(T::default(), st, None),
		};
		let raw = Some(value.display());
		match self.scalar_element(value) {
			Ok(el) => match coerce(el) {
				Some(v) => Read::new(v, Status::Good, raw),
				None => Read::new(T::default(), Status::BadType, raw),
			},
			Err(st) => Read::new(T::default(), st, raw),
		}
	}

	pub fn read_int(&self, path: &str) -> Read<i64> {
		let lvl = self.strictness;
		self.read_scalar(path, |e| parse_int_text(e, lvl))
	}

	pub fn read_float(&self, path: &str) -> Read<f64> {
		let lvl = self.strictness;
		self.read_scalar(path, |e| parse_float_text(e, lvl))
	}

	pub fn read_bool(&self, path: &str) -> Read<bool> {
		let lvl = self.strictness;
		self.read_scalar(path, |e| parse_bool_text(&e.text, lvl))
	}

	pub fn read_datetime(&self, path: &str) -> Read<ShclDateTime> {
		self.read_scalar(path, |e| parse_datetime(&e.text))
	}

	/// Any value reads as a string: a raw block yields its content, an array its
	/// canonical inline text. Escapes are applied.
	pub fn read_string(&self, path: &str) -> Read<String> {
		let value = match self.value_at(path) {
			Ok(v) => v,
			Err(st) => return Read::new(String::new(), st, None),
		};
		let raw = Some(value.display());
		match value {
			Value::Empty => Read::new(String::new(), Status::Empty, raw),
			Value::Raw { content, .. } => Read::new(content.clone(), Status::Good, raw),
			Value::Cell(els) if els.len() == 1 => {
				Read::new(apply_escapes(&els[0].text), Status::Good, raw)
			}
			// Canonical inline form (quoting + escapes intact), so the string
			// re-parses to the same array - not the bare display join.
			Value::Cell(els) => Read::new(
				els.iter().map(emit_element).collect::<Vec<_>>().join(", "),
				Status::Good,
				raw,
			),
		}
	}

	/// Raw-block content (verbatim). Non-block values are BadType.
	pub fn read_raw(&self, path: &str) -> Read<String> {
		let value = match self.value_at(path) {
			Ok(v) => v,
			Err(st) => return Read::new(String::new(), st, None),
		};
		let raw = Some(value.display());
		match value {
			Value::Raw { content, .. } => Read::new(content.clone(), Status::Good, raw),
			Value::Empty => Read::new(String::new(), Status::Empty, raw),
			_ => Read::new(String::new(), Status::BadType, raw),
		}
	}

	/// The advisory info-string of a raw block ("" when absent).
	pub fn read_raw_info(&self, path: &str) -> Read<String> {
		let value = match self.value_at(path) {
			Ok(v) => v,
			Err(st) => return Read::new(String::new(), st, None),
		};
		match value {
			Value::Raw { info, .. } => Read::new(info.clone(), Status::Good, Some(value.display())),
			_ => Read::new(String::new(), Status::BadType, Some(value.display())),
		}
	}

	fn read_array<T: Default>(
		&self,
		path: &str,
		coerce: impl Fn(&Element) -> Option<T>,
	) -> Read<Vec<T>> {
		// Wildcard paths: one slot per instance, missing sub-paths keep their slot
		// (spec: never silently dropped). Each slot reads like a scalar of the
		// target type and records its own status; the aggregate is the worst one.
		match self.resolve(path) {
			Err(st) => Read::new(Vec::new(), st, None),
			Ok(Resolved::Slots(slots)) => {
				let mut out: Vec<T> = Vec::new();
				let mut sts: Vec<Status> = Vec::new();
				for slot in &slots {
					match slot {
						Err(st) => {
							out.push(T::default());
							sts.push(*st);
						}
						Ok(n) => match self.scalar_element(&self.arena[*n].value) {
							Ok(el) => match coerce(el) {
								Some(v) => {
									out.push(v);
									sts.push(Status::Good);
								}
								None => {
									out.push(T::default());
									sts.push(Status::BadType);
								}
							},
							Err(st) => {
								out.push(T::default());
								sts.push(st);
							}
						},
					}
				}
				let status = if sts.is_empty() {
					Status::Empty
				} else {
					sts.iter().copied().max().unwrap_or(Status::Good)
				};
				Read::with_slots(out, status, None, sts)
			}
			Ok(Resolved::None) => Read::new(Vec::new(), Status::NotFound, None),
			Ok(Resolved::Many(_)) => Read::new(Vec::new(), Status::Multiple, None),
			Ok(Resolved::One(n)) => {
				let value = &self.arena[n].value;
				let raw = Some(value.display());
				match value {
					Value::Empty => Read::new(Vec::new(), Status::Empty, raw),
					Value::Raw { .. } => Read::new(Vec::new(), Status::BadType, raw),
					Value::Cell(els) => {
						let mut out = Vec::with_capacity(els.len());
						let mut sts = Vec::with_capacity(els.len());
						for el in els {
							match coerce(el) {
								Some(v) => {
									out.push(v);
									sts.push(Status::Good);
								}
								None => {
									out.push(T::default());
									sts.push(Status::BadType);
								}
							}
						}
						let status = sts.iter().copied().max().unwrap_or(Status::Good);
						Read::with_slots(out, status, raw, sts)
					}
				}
			}
		}
	}

	pub fn read_int_array(&self, path: &str) -> Read<Vec<i64>> {
		let lvl = self.strictness;
		self.read_array(path, |e| parse_int_text(e, lvl))
	}

	pub fn read_float_array(&self, path: &str) -> Read<Vec<f64>> {
		let lvl = self.strictness;
		self.read_array(path, |e| parse_float_text(e, lvl))
	}

	pub fn read_bool_array(&self, path: &str) -> Read<Vec<bool>> {
		let lvl = self.strictness;
		self.read_array(path, |e| parse_bool_text(&e.text, lvl))
	}

	pub fn read_datetime_array(&self, path: &str) -> Read<Vec<ShclDateTime>> {
		self.read_array(path, |e| parse_datetime(&e.text))
	}

	pub fn read_string_array(&self, path: &str) -> Read<Vec<String>> {
		self.read_array(path, |e| Some(apply_escapes(&e.text)))
	}

	// Full tier, Result form: Ok(value) on Good; the sentinel otherwise. Empty
	// still surfaces as Err(Empty) here; use read_* to also get the empty value.

	pub fn get_int(&self, path: &str) -> Result<i64, Status> {
		let r = self.read_int(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	pub fn get_float(&self, path: &str) -> Result<f64, Status> {
		let r = self.read_float(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	pub fn get_bool(&self, path: &str) -> Result<bool, Status> {
		let r = self.read_bool(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	pub fn get_string(&self, path: &str) -> Result<String, Status> {
		let r = self.read_string(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	pub fn get_raw(&self, path: &str) -> Result<String, Status> {
		let r = self.read_raw(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	pub fn get_datetime(&self, path: &str) -> Result<ShclDateTime, Status> {
		let r = self.read_datetime(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}
}
