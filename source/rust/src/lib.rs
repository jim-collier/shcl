// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

//! SHCL reference implementation: parser, accessor, writer/formatter.
//! Single file on purpose - the drop-in story is "copy this file into your tree".
//! The language spec lives in project/spec.md; the conformance corpus in
//! project/conformance/ pins every behavior here.
//! Every other binding mirrors this file's structure on purpose (parity over
//! idiom - see style-guide.md), so restructuring here means restructuring all.

use std::collections::{HashMap, HashSet};

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

/// One parser or validator finding, tied to a source line.
#[derive(Debug, Clone)]
pub struct Diagnostic {
	pub line: usize, // 1-based
	pub severity: Severity,
	pub message: String,
	/// Stable machine code (E001.., H001..) identifying the diagnostic kind. The
	/// contract lives here; the `message` prose is a free, per-binding voice.
	pub code: &'static str,
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

impl std::fmt::Display for Status {
	/// The names the other three bindings print. Without this a status reaches
	/// a user-facing message as debug output.
	fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
		f.write_str(match self {
			Status::Good => "Good",
			Status::Empty => "Empty",
			Status::NotFound => "NotFound",
			Status::BadType => "BadType",
			Status::Multiple => "Multiple",
		})
	}
}

/// What load_file found: the four cases a consumer's own load path otherwise
/// confuses. Clean and HadErrors both carry a usable document; NotFound and
/// Unreadable come back with an empty one.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FileStatus {
	Clean,      // read and parsed, no error diagnostics (hints allowed)
	HadErrors,  // read and parsed, but error diagnostics are present
	NotFound,   // no file at the path
	Unreadable, // exists but could not be read (permissions, a directory, bad encoding, past a read_file cap)
}

/// Why a save did not happen. The two cases need different handling, so they
/// are separate values rather than two spellings of one message: `Refused` is
/// the lost-content gate, which the caller can reverse with `save_file_lossy`,
/// and `Io` is the disk's answer, which they cannot.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SaveError {
	/// The load dropped content this save would delete (see `lost_count`).
	Refused { path: String, lost: usize },
	/// The write itself failed; carries the reported message.
	Io(String),
}

impl std::fmt::Display for SaveError {
	fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
		match self {
			SaveError::Refused { path, lost } => write!(
				f,
				"{}: refusing to save: load dropped {} line(s)/value(s) this write would delete (see diagnostics; save_file_lossy overrides)",
				path, lost
			),
			SaveError::Io(m) => f.write_str(m),
		}
	}
}

impl std::error::Error for SaveError {}

/// Why a write would fail (`write_reason()`): the distinctions behind a
/// setter's bare `false`. `Writable` = the path passes the writer's
/// validation; the rest name the five ways it cannot.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WriteReason {
	Writable,
	BadPath,     // empty path, the scanner rejected it, or a segment carries a line break
	ValueInPath, // the path carries a `: value` part; writes take values separately
	Wildcard,    // wildcard selectors are query-only
	NoSuchIndex, // a `[#k]` instance that does not (and can never) exist
	TooDeep,     // deeper than the nesting cap; the writer never creates past it
}

/// Full-tier read result: value plus status plus the original raw text (when the
/// path resolved), so a caller can always recover what was actually in the file.
/// Array reads also carry one status per slot (element, or wildcard instance) in
/// `slots`; `status` is then the worst slot. Scalar reads leave `slots` empty.
/// `line` is the 1-based source line of the resolved binding (0 when the path
/// did not resolve to one node, or the node was writer-built), so a consumer
/// check the schema cannot express can still cite the line. `quoted` is true
/// when the read's single scalar element was quoted in the source - the escape
/// hatch that lets a downstream language reserve `@null` while `"@null"` stays
/// a plain string. Arrays, raw blocks, and empties leave it false.
#[derive(Debug, Clone)]
pub struct Read<T> {
	pub value: T,
	pub status: Status,
	pub raw: Option<String>,
	pub slots: Vec<Status>,
	pub line: usize,
	pub quoted: bool,
}

impl<T> Read<T> {
	fn new(value: T, status: Status, raw: Option<String>) -> Read<T> {
		Read {
			value,
			status,
			raw,
			slots: Vec::new(),
			line: 0,
			quoted: false,
		}
	}
	fn with_slots(value: T, status: Status, raw: Option<String>, slots: Vec<Status>) -> Read<T> {
		Read {
			value,
			status,
			raw,
			slots,
			line: 0,
			quoted: false,
		}
	}
	fn at(mut self, line: usize, quoted: bool) -> Read<T> {
		self.line = line;
		self.quoted = quoted;
		self
	}
	/// Whether the author addressed this field at all: `Good` or `Empty`. Note
	/// this deliberately answers differently from the convenience tier, which
	/// falls back on `Empty` like any other non-Good read - `ok` asks "is this
	/// field spoken for", `get_*_or` asks "do I have a usable value", and an
	/// explicitly emptied field is the case where those two diverge.
	pub fn ok(&self) -> bool {
		matches!(self.status, Status::Good | Status::Empty)
	}
}

/// A failed strict load: the diagnostics that failed it, plus the recovered tree.
#[derive(Debug)]
pub struct LoadError {
	pub diagnostics: Vec<Diagnostic>,
	/// The document the parse produced anyway. Recover-and-continue means the
	/// diagnostics are the point of a failed strict load, and the tree is what
	/// a Standard load would have kept - so a caller can still inspect both.
	pub document: Document,
}

impl std::fmt::Display for LoadError {
	fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
		// Name the first few failures right in the message; the bare count made
		// callers dig for information the error was already holding.
		let errs: Vec<&Diagnostic> = self
			.diagnostics
			.iter()
			.filter(|d| d.severity == Severity::Error)
			.collect();
		write!(f, "strict load failed: {} error diagnostic(s)", errs.len())?;
		for d in errs.iter().take(3) {
			write!(f, "; line {}: {} {}", d.line, d.code, d.message)?;
		}
		if errs.len() > 3 {
			write!(f, "; +{} more", errs.len() - 3)?;
		}
		Ok(())
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

/// The name the Go binding uses for the same type; either spelling works.
pub type DateTime = ShclDateTime;

/// Two datetimes name the same moment, whatever the spelling. The struct
/// mirrors what was written, so `12:00:00Z` and `12:00:00+00:00` are different
/// values field by field while naming one time, and `12:00:00` and
/// `12:00:00.0` differ only in written precision. A `[value]` selector matches
/// on text, but an `allowed` set is about the value, so it compares here. An
/// absent zone is local and matches no zone at all - that is the one spelling
/// difference that is a real difference.
fn same_moment(a: &ShclDateTime, b: &ShclDateTime) -> bool {
	let frac = |f: &Option<String>| {
		f.as_deref()
			.map_or(String::new(), |d| d.trim_end_matches('0').to_string())
	};
	let zone = |z: &Option<ZoneSpec>| match z {
		Some(ZoneSpec::Utc) | Some(ZoneSpec::OffsetMinutes(0)) => Some(0),
		Some(ZoneSpec::OffsetMinutes(m)) => Some(*m),
		None => None,
	};
	if a.date.is_some() != b.date.is_some()
		|| a.time.is_some() != b.time.is_some()
		|| a.time.map(|t| t.2) != b.time.map(|t| t.2)
		|| frac(&a.frac) != frac(&b.frac)
	{
		return false;
	}
	match (zone(&a.zone), zone(&b.zone)) {
		(None, None) => a.date == b.date && a.time == b.time,
		// Zoned values are instants: the written clock less its offset, the
		// date carrying the day wrap. A time alone lives on a 24-hour cycle.
		(Some(ao), Some(bo)) => {
			let minutes = |dt: &ShclDateTime, off: i32| {
				let hm = dt
					.time
					.map_or(0, |(h, m, _)| i64::from(h) * 60 + i64::from(m))
					- i64::from(off);
				match dt.date {
					Some((y, m, d)) => days_from_civil(y, m, d) * 1440 + hm,
					None => hm.rem_euclid(1440),
				}
			};
			minutes(a, ao) == minutes(b, bo)
		}
		_ => false,
	}
}

/// Days since 1970-01-01 for a civil date, negative before it.
fn days_from_civil(y: i32, m: u32, d: u32) -> i64 {
	let y = i64::from(if m <= 2 { y - 1 } else { y });
	let era = y.div_euclid(400);
	let yoe = y - era * 400;
	let doy = (153 * i64::from((m + 9) % 12) + 2) / 5 + i64::from(d) - 1;
	let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	era * 146_097 + doe - 719_468
}

/// A datetime's zone suffix as written.
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
	text: String, // quote-stripped, escapes NOT applied (applied on string read; names differ - see scan_path_ex)
	quoted: bool,
}

/// One whole-line comment held as trivia, plus whether a blank line preceded
/// it - so a blank between comment-only regions survives the round-trip
/// (blank runs collapse to one, same as nodes).
#[derive(Debug, Clone)]
struct Lead {
	text: String,
	blank_before: bool,
}

impl Lead {
	fn plain(text: String) -> Lead {
		Lead {
			text,
			blank_before: false,
		}
	}
}

/// A pending whole-line comment during parse: text, source indent (used only
/// to decide whether it hangs on a deeper block), and the blank it consumed.
/// `ceiling` is the shortest incoming indent already checked against it: a
/// later check can only hang it from a shorter one, so a longer one skips it.
struct Pend {
	text: String,
	indent: String,
	blank_before: bool,
	ceiling: usize,
}

#[derive(Debug, Clone, PartialEq)]
enum Value {
	Empty,
	Cell(Vec<Element>), // one element = scalar, more = inline array
	Raw(Box<RawVal>),   // boxed: the four fields would triple the enum's size
}

#[derive(Debug, Clone, PartialEq)]
struct RawVal {
	content: String,
	info: String,
	fence_char: u8,
	fence_len: usize,
}

/// The identity spelling of an element's text: escapes resolved, so two
/// spellings of one string are one instance. Names have followed that rule
/// since 2.0, and a `[value]` selector matches on the resolved text already -
/// without this, one selector addressed two instances. Borrowed when there is
/// nothing to resolve, which is nearly every element.
fn key_text(s: &str) -> std::borrow::Cow<'_, str> {
	if s.contains('\\') {
		std::borrow::Cow::Owned(apply_escapes(s))
	} else {
		std::borrow::Cow::Borrowed(s)
	}
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
					let t = key_text(&e.text);
					k.push_str(&t.len().to_string());
					k.push(':');
					k.push_str(&t);
				}
				k
			}
			// Info-string is part of identity (a `sql` and a `python` block are
			// different values even with equal bodies); fence style is not. Info is
			// length-prefixed for the same injectivity reason as cell elements.
			Value::Raw(r) => format!("r:{}:{}{}", r.info.len(), r.info, r.content),
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
			Value::Raw(r) => r.content.clone(),
		}
	}
	fn is_empty(&self) -> bool {
		matches!(self, Value::Empty)
	}
}

#[derive(Debug, Clone)]
struct NodeData {
	name: String, // ASCII-folded to lower; non-ASCII never folds
	value: Value,
	children: Vec<usize>,
	parent: usize,
	line: usize,
	star_list: bool,  // value built from stacked "* " lines
	star_mixed: bool, // mix of "* " and field children already diagnosed
	// Comment trivia, boxed off to the side: most nodes carry none, and the
	// four empty containers were a third of every node.
	trivia: Option<Box<Trivia>>,
	// Blank-line grouping is the other half of hand-authored layout: set when
	// a blank line preceded this node's binding line (runs collapse to one).
	blank_before: bool,
	// This node has decided its `src` - set by the first source line whose
	// value the node holds, whether or not a string was worth keeping.
	src_set: bool,
	// Verbatim value text from the source line (after the colon, comment
	// stripped, trimmed) - what a read's `raw` hands back. None when the value
	// was synthesized (writer, stacked list, fence) OR when the spelling is
	// exactly the display form; raw falls back to the display form either way.
	src: Option<String>,
	// The name as the author spelled it (case unfolded, quotes and escapes
	// resolved) - what authored_name() hands back, via authored(). Merged
	// instances keep the first binding's spelling, like line() and comments.
	// Empty = spelled exactly like `name` (the overwhelmingly common case).
	name_src: String,
}

/// Comment trivia, verbatim from `#` to end of line. Never part of identity
/// or reads; merged instances concatenate leading, first trailing wins
/// (later ones demote to leading - a canonical line has room for one).
#[derive(Debug, Clone, Default)]
struct Trivia {
	leading: Vec<Lead>,
	trailing: String, // empty = none
	// Whole-line comments that followed this node's subtree at a deeper indent
	// than the next binding - they belong to this block, not the next node, so
	// a run trailing a block's last child stays put instead of re-attaching
	// dedented. Emitted after the subtree at this node's depth.
	after: Vec<Lead>,
	// Whole-line comments written inside this node's block when no bound child
	// could take them - a header whose children are all commented still owns
	// those lines. Emitted after the subtree one level deeper than this node.
	inside: Vec<Lead>,
}

impl NodeData {
	fn leading(&self) -> &[Lead] {
		self.trivia.as_deref().map_or(&[], |t| &t.leading)
	}
	fn trailing(&self) -> &str {
		self.trivia.as_deref().map_or("", |t| &t.trailing)
	}
	fn after(&self) -> &[Lead] {
		self.trivia.as_deref().map_or(&[], |t| &t.after)
	}
	fn inside(&self) -> &[Lead] {
		self.trivia.as_deref().map_or(&[], |t| &t.inside)
	}
	fn triv_mut(&mut self) -> &mut Trivia {
		self.trivia.get_or_insert_with(Default::default)
	}
	/// The as-authored name spelling; empty name_src means "same as name".
	fn authored(&self) -> &str {
		if self.name_src.is_empty() {
			&self.name
		} else {
			&self.name_src
		}
	}
}

/// Store a name's authored spelling: the empty sentinel when it matches the
/// folded name, so the duplicate string never gets allocated.
fn spelled(name: &str, name_src: &str) -> String {
	if name_src == name {
		String::new()
	} else {
		name_src.to_string()
	}
}

/// True when a source value spelling is exactly the display form - the case
/// where `src` need not be stored, since raw's fallback reproduces it.
fn src_matches_display(v: &Value, s: &str) -> bool {
	match v {
		Value::Empty => s.is_empty(),
		Value::Raw(r) => s == r.content,
		Value::Cell(els) => {
			let mut rest = s;
			for (i, e) in els.iter().enumerate() {
				if i > 0 {
					match rest.strip_prefix(", ") {
						Some(r) => rest = r,
						None => return false,
					}
				}
				match rest.strip_prefix(e.text.as_str()) {
					Some(r) => rest = r,
					None => return false,
				}
			}
			rest.is_empty()
		}
	}
}

/// A parsed SHCL document: the tree, its diagnostics, and its strictness level.
// Clone is a plain deep copy: the arena is index-based, so cloning the vector
// copies the whole tree with no reference to fix up.
#[derive(Debug, Clone)]
pub struct Document {
	arena: Vec<NodeData>,
	diags: Vec<Diagnostic>,
	strictness: Strictness,
	orphans: Vec<Lead>, // top-level comments after the last binding line
	// Lines or values parsing dropped that canonical output cannot re-emit
	// (bad indentation, an unusable selector, past the depth cap, ...).
	// Content-malformed lines are NOT counted - they are retained as trivia
	// and survive a save. lost_count() serves it; save_file() gates on it.
	lost: usize,
	// Built on the first path lookup and kept current by the writer (a new
	// child appends, a removed one unlinks); only a merge drops it. Without it
	// every lookup scans the parent's children, so a flat document read or
	// written key by key was quadratic.
	index: std::sync::OnceLock<Box<NameIndex>>,
}

/// The first child of each (parent, name), chained on to the next same-named
/// sibling, plus the chain tail so an append is O(1). A hash collision chains
/// a stranger in; the lookup checks the name, so the chain is only ever a
/// superset.
#[derive(Debug, Clone)]
struct NameIndex {
	first: HashMap<u64, usize>,
	last: HashMap<u64, usize>,
	next_same: Vec<usize>, // per node; NIL ends the chain
}

const NIL: usize = usize::MAX;

impl NameIndex {
	fn append(&mut self, key: u64, node: usize) {
		if self.next_same.len() <= node {
			self.next_same.resize(node + 1, NIL);
		}
		self.next_same[node] = NIL;
		match self.last.insert(key, node) {
			Some(prev) => self.next_same[prev] = node,
			None => {
				self.first.insert(key, node);
			}
		}
	}

	// Walks the chain to find the predecessor; a chain is one name's siblings.
	fn unlink(&mut self, key: u64, node: usize) {
		let Some(&head) = self.first.get(&key) else {
			return;
		};
		let next = self.next_same[node];
		if head == node {
			if next == NIL {
				self.first.remove(&key);
				self.last.remove(&key);
			} else {
				self.first.insert(key, next);
			}
		} else {
			let mut c = head;
			while c != NIL && self.next_same[c] != node {
				c = self.next_same[c];
			}
			if c == NIL {
				return;
			}
			self.next_same[c] = next;
			if next == NIL {
				self.last.insert(key, c);
			}
		}
		self.next_same[node] = NIL;
	}
}

fn name_key(parent: usize, name: &str) -> u64 {
	let mut h = Fnv::new();
	h.dec(parent);
	h.byte(0xFF);
	h.bytes(name.as_bytes());
	h.0
}

const ROOT: usize = 0;
// Stack entry for a binding line that was skipped: it still owns its indent
// level, so the lines written under it are skipped with it instead of
// re-parenting one level up.
const DEAD: usize = usize::MAX;
// Stack entry for a line whose indent matched no open level (E012): never a
// level a sibling can bind at, but deeper lines are still under it.
const UNOPENED: usize = usize::MAX - 1;

/// Merge a later instance into an earlier one under the in-file merge rule:
/// children and trivia move over, first trailing wins (a second demotes to a
/// leading line), first spelling stays. The caller drops the loser from the
/// parent's child list; it keeps its arena slot, unreferenced.
fn fold_node_into(arena: &mut [NodeData], survivor: usize, loser: usize) {
	let kids = std::mem::take(&mut arena[loser].children);
	for &k in &kids {
		arena[k].parent = survivor;
	}
	arena[survivor].children.extend(kids);
	if let Some(mut lt) = arena[loser].trivia.take() {
		let st = arena[survivor].triv_mut();
		st.leading.append(&mut lt.leading);
		if !lt.trailing.is_empty() {
			if st.trailing.is_empty() {
				st.trailing = std::mem::take(&mut lt.trailing);
			} else {
				st.leading
					.push(Lead::plain(std::mem::take(&mut lt.trailing)));
			}
		}
		st.after.append(&mut lt.after);
		st.inside.append(&mut lt.inside);
	}
}

/// Maximum nesting depth (levels below the document root), enforced at load
/// and by the Writer. Deeper lines are skipped with an `E016` error. The cap
/// is what keeps the recursive tree walks (emit, merge, clone) safely inside
/// every binding's stack, so a hostile or machine-generated document can make
/// a load fail but never crash the consumer.
pub const MAX_DEPTH: usize = 512;

// ---------------------------------------------------------------------------
// Lexical helpers
// ---------------------------------------------------------------------------

/// The text between the brackets of a value spelled the way JSON, TOML and
/// YAML spell an array, or None when the line is not that shape. The path
/// scanner reads the brackets as a selector, so the line arrives with no value
/// text and the old repair blamed a colon that is plainly there. The colon
/// that counts is the field's own: one inside a quoted name or a selector is
/// not it. Selector sugar (`base:[Boston]`) is spelled the same way and is
/// legal, so the caller decides by what the brackets hold.
fn bracket_array_body(content: &str) -> Option<&str> {
	match name_half(content, false) {
		NameHalf::Colon(colon) => {
			let rest = content[colon + 1..].trim();
			rest.strip_prefix('[').and_then(|r| r.strip_suffix(']'))
		}
		_ => None,
	}
}

/// How the name half of a field line ends.
enum NameHalf {
	Colon(usize), // the field's own colon, byte offset
	Hash(usize),  // an unquoted `#` first: a comment, or a malformed name
	End,
}

/// Scan a field line's name half the way the path scanner reads it. A quote
/// opens only where the scanner opens one - as a segment's first char (a
/// quoted name) or a selector body's first char (a quoted discriminator) - so
/// `O'Brien` in a bare selector is text, not an open quote hiding the `#`
/// after it. `\` shields the next char inside quotes only; a bare selector
/// body runs to the first `]` unescaped, as it does in the scanner. With
/// `sugar` a colon followed by `[` is selector sugar rather than the
/// separator. An unquoted `#` ends the half wherever it sits: a comment, or a
/// malformed name.
fn name_half(s: &str, sugar: bool) -> NameHalf {
	let mut in_quote: Option<char> = None;
	let mut in_sel = false;
	let mut at_start = true; // first char of a segment, or of a selector body
	let mut it = s.char_indices();
	while let Some((byte, c)) = it.next() {
		if let Some(q) = in_quote {
			if c == '\\' {
				it.next();
			} else if c == q {
				in_quote = None;
			}
			continue;
		}
		if is_wsp(c) {
			continue;
		}
		if c == '#' {
			return NameHalf::Hash(byte);
		}
		if in_sel {
			if c == ']' {
				in_sel = false;
			} else if at_start && (c == '"' || c == '\'') {
				in_quote = Some(c);
			}
			at_start = false;
			continue;
		}
		match c {
			'"' | '\'' if at_start => in_quote = Some(c),
			'[' => {
				in_sel = true;
				at_start = true;
				continue;
			}
			'.' => {
				at_start = true;
				continue;
			}
			':' => {
				let rest = s[byte + 1..].trim_start_matches([' ', '\t']);
				if !(sugar && rest.starts_with('[')) {
					return NameHalf::Colon(byte);
				}
			}
			_ => {}
		}
		at_start = false;
	}
	NameHalf::End
}

/// Offset of the `#` that starts a comment in value text, scanning from
/// `from`. A quote opens a quoted piece only at the start of a piece - the
/// start of the value, or after an unquoted comma - which is the spec's rule:
/// a piece is quoted only when it begins with one. So an apostrophe in prose
/// (`don't panic  # keep`) hides nothing. `\` shields the next char.
fn value_comment_at(s: &str, from: usize) -> Option<usize> {
	let mut in_quote: Option<char> = None;
	let mut at_start = true;
	let mut it = s[from..].char_indices();
	while let Some((off, c)) = it.next() {
		if c == '\\' {
			it.next();
			at_start = false;
			continue;
		}
		match in_quote {
			Some(q) => {
				if c == q {
					in_quote = None;
				}
			}
			None => match c {
				'"' | '\'' if at_start => in_quote = Some(c),
				'#' => return Some(from + off),
				',' => {
					at_start = true;
					continue;
				}
				_ => {}
			},
		}
		if !is_wsp(c) {
			at_start = false;
		}
	}
	None
}

/// Folds A-Z only; non-ASCII passes through untouched. Borrowed when there is
/// nothing to fold, the way key_text borrows when there is nothing to resolve.
fn fold_name(s: &str) -> std::borrow::Cow<'_, str> {
	if s.bytes().any(|b| b.is_ascii_uppercase()) {
		std::borrow::Cow::Owned(s.to_ascii_lowercase())
	} else {
		std::borrow::Cow::Borrowed(s)
	}
}

fn is_bare_name_char(c: char) -> bool {
	c.is_ascii_alphanumeric() || c == '-' || c == '_'
}

/// The grammar's `wsp`: a space or a tab. The parser trims with this and
/// nothing wider - a no-break space or a line separator after a value is
/// content, and a Unicode trim used to delete it with no diagnostic.
fn is_wsp(c: char) -> bool {
	c == ' ' || c == '\t'
}

fn trim_wsp(s: &str) -> &str {
	s.trim_matches(is_wsp)
}

/// The end of a line, or of a line's content before its comment: `wsp`, plus
/// a carriage return, which the load takes off a line end anyway - so a
/// retained line or a comment written back never ends in one the next load
/// would strip. A CR followed by content stays content.
fn trim_wsp_end(s: &str) -> &str {
	s.trim_end_matches(|c| is_wsp(c) || c == '\r')
}

/// Split off an unquoted trailing comment from a field line: (content, comment
/// from `#` on). The name half is read the scanner's way and the value half
/// the value's way (see value_comment_at). Comments are kept as trivia.
fn split_comment(s: &str) -> (&str, Option<&str>) {
	if !s.contains('#') {
		return (s, None);
	}
	let hash = match name_half(s, true) {
		NameHalf::Hash(i) => Some(i),
		NameHalf::Colon(i) => value_comment_at(s, i + 1),
		NameHalf::End => None,
	};
	match hash {
		Some(i) => (&s[..i], Some(&s[i..])),
		None => (s, None),
	}
}

/// The same for value text alone: a list element, or a setter's argument.
fn split_value_comment(s: &str) -> (&str, Option<&str>) {
	if !s.contains('#') {
		return (s, None);
	}
	match value_comment_at(s, 0) {
		Some(i) => (&s[..i], Some(&s[i..])),
		None => (s, None),
	}
}

/// Split on unquoted commas; a quote opens only at the start of a piece (see
/// value_comment_at), and `\` shields the next char.
fn split_unquoted_commas(s: &str) -> Vec<&str> {
	if !s.contains(',') {
		return vec![s];
	}
	let mut parts = Vec::new();
	let mut in_quote: Option<char> = None;
	let mut at_start = true;
	let mut start = 0usize;
	let mut it = s.char_indices();
	while let Some((byte, c)) = it.next() {
		if c == '\\' {
			it.next();
			at_start = false;
			continue;
		}
		match in_quote {
			Some(q) => {
				if c == q {
					in_quote = None;
				}
			}
			None => match c {
				'"' | '\'' if at_start => in_quote = Some(c),
				',' => {
					parts.push(&s[start..byte]);
					start = byte + 1;
					at_start = true;
					continue;
				}
				_ => {}
			},
		}
		if !is_wsp(c) {
			at_start = false;
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

/// True when some piece starts with a quote that never closes (the closing
/// quote missing or escaped). Such a piece stays literal - and a quote-aware
/// comment strip has already swallowed any trailing `#` comment into it - so
/// the parser calls it out instead of letting the typo look deliberate.
/// Mid-text apostrophes (`it's fine`) are legal prose and stay silent.
fn unterminated_quote(text: &str) -> bool {
	if !text.contains('"') && !text.contains('\'') {
		return false;
	}
	for piece in split_unquoted_commas(text) {
		let t = trim_wsp(piece);
		if !quoted_shape(t) {
			let Some(&first) = t.as_bytes().first() else {
				continue;
			};
			if first == b'"' || first == b'\'' {
				return true;
			}
		}
	}
	false
}

/// Value text for a diagnostic message: line breaks and tabs escaped, so one
/// diagnostic is one line. A raw block's body is the value that made this
/// necessary - it carries its own newlines.
fn one_line(s: &str) -> String {
	s.replace('\\', "\\\\")
		.replace('\n', "\\n")
		.replace('\r', "\\r")
		.replace('\t', "\\t")
}

/// True when the text is one quote pair: a quote char at both ends, the last
/// one not escaped. Quotes and the backslash are ASCII, so bytes suffice.
fn quoted_shape(t: &str) -> bool {
	let b = t.as_bytes();
	let Some(&first) = b.first() else {
		return false;
	};
	if (first != b'"' && first != b'\'') || b.len() < 2 || b[b.len() - 1] != first {
		return false;
	}
	let mut esc = false;
	for &c in &b[1..b.len() - 1] {
		esc = c == b'\\' && !esc;
	}
	!esc
}

/// Trim, then strip one matching outer quote pair if present. Unquoted empty
/// slots return None (dropped, never an error).
fn parse_element(piece: &str) -> Option<Element> {
	let t = trim_wsp(piece);
	if t.is_empty() {
		return None;
	}
	if quoted_shape(t) {
		return Some(Element {
			text: t[1..t.len() - 1].to_string(),
			quoted: true,
		});
	}
	Some(Element {
		text: normalize_dangling_backslash(t.to_string()),
		quoted: false,
	})
}

/// Whether `parse_cell` would build more than `max` elements. Counts the
/// pieces the way the splitter cuts them, without building any, so a capped
/// parse refuses an over-long line before holding the array.
fn cell_exceeds(text: &str, max: usize) -> bool {
	let mut count = 0usize;
	let mut has_content = false;
	let mut in_quote: Option<char> = None;
	let mut it = text.chars();
	while let Some(c) = it.next() {
		if c == '\\' {
			has_content = true;
			it.next();
			continue;
		}
		match in_quote {
			Some(q) if c == q => in_quote = None,
			None if (c == '"' || c == '\'') && !has_content => in_quote = Some(c),
			None if c == ',' => {
				if has_content {
					count += 1;
					if count > max {
						return true;
					}
				}
				has_content = false;
				continue;
			}
			_ => {}
		}
		if !is_wsp(c) {
			has_content = true;
		}
	}
	if has_content {
		count += 1;
	}
	count > max
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

/// The predicate a `[value]` selector matches with: display form with escapes
/// applied on both sides, so `["q\"uote"]` finds `'q"uote'` - a logical-string
/// match, not spelling against spelling.
fn disp_key(v: &Value) -> String {
	apply_escapes(&v.display())
}

/// The single-element restriction a QUOTED `[value]` selector adds on top of
/// the display match: quoting selects the scalar spelling only, so the scalar
/// "a, b" and the list a, b stop meeting the same selector.
fn single_scalar(v: &Value) -> bool {
	matches!(v, Value::Cell(els) if els.len() == 1)
}

// FNV-1a, fed the same byte sequence the key strings would spell - the
// accelerator maps key on a u64 and verify hits against the arena, so the
// strings themselves never get built. The hash only has to be stable within
// one parse, not injective; a collision just chains in the slot.
struct Fnv(u64);

impl Fnv {
	fn new() -> Fnv {
		Fnv(0xcbf2_9ce4_8422_2325)
	}
	fn byte(&mut self, b: u8) {
		self.0 = (self.0 ^ u64::from(b)).wrapping_mul(0x100_0000_01b3);
	}
	fn bytes(&mut self, s: &[u8]) {
		for &b in s {
			self.byte(b);
		}
	}
	/// A length prefix in decimal, spelled without allocating.
	fn dec(&mut self, mut n: usize) {
		let mut buf = [0u8; 20];
		let mut i = buf.len();
		loop {
			i -= 1;
			buf[i] = b'0' + (n % 10) as u8;
			n /= 10;
			if n == 0 {
				break;
			}
		}
		self.bytes(&buf[i..]);
	}
}

/// Hash of the (name, merge-key) pair, spelling what value.key() spells
/// without building it.
fn merge_hash(name: &str, v: &Value) -> u64 {
	let mut h = Fnv::new();
	h.bytes(name.as_bytes());
	h.byte(0xFF); // separator; equality still verifies both parts
	match v {
		Value::Empty => h.byte(b'e'),
		Value::Cell(els) => {
			h.bytes(b"c:");
			for e in els {
				let t = key_text(&e.text);
				h.dec(t.len());
				h.byte(b':');
				h.bytes(t.as_bytes());
			}
		}
		Value::Raw(r) => {
			h.bytes(b"r:");
			h.dec(r.info.len());
			h.byte(b':');
			h.bytes(r.info.as_bytes());
			h.bytes(r.content.as_bytes());
		}
	}
	h.0
}

/// Hash of the value's merge key alone (no name part).
fn value_hash(v: &Value) -> u64 {
	merge_hash("", v)
}

/// The exact (name, merge-key) equality a hashed hit is verified with -
/// compares what the two key strings would hold, element by element.
fn merge_eq(name_a: &str, va: &Value, name_b: &str, vb: &Value) -> bool {
	if name_a != name_b {
		return false;
	}
	match (va, vb) {
		(Value::Empty, Value::Empty) => true,
		(Value::Cell(a), Value::Cell(b)) => {
			a.len() == b.len()
				&& a.iter()
					.zip(b)
					.all(|(x, y)| key_text(&x.text) == key_text(&y.text))
		}
		(Value::Raw(a), Value::Raw(b)) => a.info == b.info && a.content == b.content,
		_ => false,
	}
}

/// apply_escapes as a streaming feed into the hash - the same state machine,
/// one char at a time, no intermediate string.
struct EscHash {
	h: Fnv,
	pending: bool,
}

impl EscHash {
	fn emit(&mut self, c: char) {
		let mut b = [0u8; 4];
		self.h.bytes(c.encode_utf8(&mut b).as_bytes());
	}
	fn push(&mut self, c: char) {
		if self.pending {
			self.pending = false;
			match c {
				't' => self.emit('\t'),
				'n' => self.emit('\n'),
				'\\' => self.emit('\\'),
				'"' => self.emit('"'),
				'\'' => self.emit('\''),
				other => {
					self.emit('\\');
					self.emit(other);
				}
			}
		} else if c == '\\' {
			self.pending = true;
		} else {
			self.emit(c);
		}
	}
	fn finish(mut self) -> u64 {
		if self.pending {
			self.emit('\\');
		}
		self.h.0
	}
}

/// Hash of the (name, display-with-escapes-applied) pair a `[value]` selector
/// matches with - what disp_key would spell, streamed instead of built.
fn disp_hash(name: &str, v: &Value) -> u64 {
	let mut h = Fnv::new();
	h.bytes(name.as_bytes());
	h.byte(0xFF);
	let mut esc = EscHash { h, pending: false };
	match v {
		Value::Empty => {}
		Value::Cell(els) => {
			for (i, e) in els.iter().enumerate() {
				if i > 0 {
					esc.push(',');
					esc.push(' ');
				}
				for c in e.text.chars() {
					esc.push(c);
				}
			}
		}
		Value::Raw(r) => {
			for c in r.content.chars() {
				esc.push(c);
			}
		}
	}
	esc.finish()
}

/// The query-side twin of disp_hash: the selector's text already has its
/// escapes applied, so its bytes feed straight in.
fn disp_hash_text(name: &str, want: &str) -> u64 {
	let mut h = Fnv::new();
	h.bytes(name.as_bytes());
	h.byte(0xFF);
	h.bytes(want.as_bytes());
	h.0
}

/// One accelerator bucket: node indices in insertion order. Nearly always one
/// entry; a second means two different keys collided in the 64-bit hash.
#[derive(Debug)]
enum Slot {
	One(usize),
	Many(Vec<usize>),
}

impl Slot {
	fn push(&mut self, idx: usize) {
		match self {
			Slot::One(a) => *self = Slot::Many(vec![*a, idx]),
			Slot::Many(v) => v.push(idx),
		}
	}
	fn first_match(&self, f: impl Fn(usize) -> bool) -> Option<usize> {
		match self {
			Slot::One(a) => f(*a).then_some(*a),
			Slot::Many(v) => v.iter().copied().find(|&c| f(c)),
		}
	}
	/// Drop `idx` if present; true = the slot is now empty and the caller
	/// should remove the map entry.
	fn remove(&mut self, idx: usize) -> bool {
		match self {
			Slot::One(a) => *a == idx,
			Slot::Many(v) => {
				v.retain(|&c| c != idx);
				if v.len() == 1 {
					let only = v[0];
					*self = Slot::One(only);
					return false;
				}
				v.is_empty()
			}
		}
	}
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
	Some((first, run, trim_wsp(&rest[run..]).to_string()))
}

// `min_len` is the opening fence's length, which the grammar puts at three or
// more, so the length test already rules out the empty line that `all` would
// otherwise accept.
fn is_fence_close(line: &str, ch: u8, min_len: usize) -> bool {
	let t = trim_wsp(line);
	t.len() >= min_len && t.bytes().all(|b| b == ch)
}

/// The leading space/tab run of a line (the indent).
fn leading_ws(line: &str) -> &str {
	let n = line
		.bytes()
		.take_while(|&b| b == b' ' || b == b'\t')
		.count();
	&line[..n]
}

/// Remove a raw block's nesting indent from one content line: only what the
/// line actually shares with it, so a shallower line (whitespace-only, or
/// written flush left) keeps its own spacing rather than being blanked.
fn strip_common<'a>(line: &'a str, common: &str) -> &'a str {
	let mut k = 0;
	for (a, b) in common.chars().zip(line.chars()) {
		if a != b {
			break;
		}
		k += a.len_utf8();
	}
	&line[k..]
}

// ---------------------------------------------------------------------------
// Path scanner (shared by file lines and accessor queries)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
enum Selector {
	// quoted: the selector text was quoted in the path. A quoted selector is
	// scalar-only - it matches a single-element value whose logical string
	// equals the text - so quoting distinguishes the scalar "a, b" from the
	// two-element list a, b, the same way quoting escapes elsewhere.
	ByValue { text: String, quoted: bool },
	ByIndex(u64), // u64, not usize: index width must not vary with the target's pointer size
	Wildcard,
}

#[derive(Debug, Clone)]
struct Segment {
	name: String,     // folded
	name_src: String, // as authored: unfolded, quotes stripped, escapes applied
	selector: Option<Selector>,
	star: bool, // bare `*` name wildcard; quoted "*" stays a literal name
}

struct PathScan {
	segments: Vec<Segment>,
	value_text: Option<String>, // text after the separator colon, trimmed
}

/// The spelling of an index selector - an optional `#`, an optional `+`, then
/// digits - whatever its size. The grammar says `1*DIGIT`, with no upper bound.
fn index_shape(body: &str) -> bool {
	let b = body.strip_prefix('#').unwrap_or(body);
	let b = b.strip_prefix('+').unwrap_or(b);
	!b.is_empty() && b.bytes().all(|c| c.is_ascii_digit())
}

/// usize view of a selector index: None when it does not fit the target's
/// pointer width. An index that big can only mean "no such instance"; a bare
/// `as` cast would wrap into a live element on a 32-bit build.
fn index_usize(k: u64) -> Option<usize> {
	usize::try_from(k).ok()
}

/// Scan `a . b : [sel] . c : value`. Whitespace around dots/colons/brackets is
/// insignificant. A colon is a selector colon only when the next non-ws char is
/// `[`; otherwise it separates the value. Err(reason) means genuinely ambiguous
/// input, which the caller skips with a diagnostic.
fn scan_path(input: &str) -> Result<PathScan, String> {
	scan_path_ex(input, false)
}

/// Query spelling of scan_path: also accepts a bare `*` segment (the name
/// wildcard - any child name). Document lines never take it; only lookups
/// (reads, the writer probe, schema paths) do.
fn scan_lookup(input: &str) -> Result<PathScan, String> {
	scan_path_ex(input, true)
}

fn scan_path_ex(input: &str, stars: bool) -> Result<PathScan, String> {
	// Byte cursor with inline char decoding (a Vec<char> per call was a parse
	// hot spot). Every position the scanner stops on is a char boundary: it
	// only byte-matches ASCII structure chars, which UTF-8 guarantees cannot
	// appear inside a multibyte sequence, and otherwise advances by whole
	// chars. Backslash still shields the next CHAR, multibyte included.
	let bytes = input.as_bytes();
	let mut pos = 0usize;
	// First char at a known boundary; the fallback arm is unreachable (callers
	// check pos < len first) but keeps the decode total.
	fn char_at(s: &str, pos: usize) -> char {
		s[pos..].chars().next().unwrap_or('\u{0}')
	}
	fn skip_ws(bytes: &[u8], pos: &mut usize) {
		while *pos < bytes.len() && (bytes[*pos] == b' ' || bytes[*pos] == b'\t') {
			*pos += 1;
		}
	}
	fn read_quoted(s: &str, pos: &mut usize) -> Result<String, String> {
		let q = char::from(s.as_bytes()[*pos]); // caller checked: ASCII quote
		*pos += 1;
		let mut out = String::new();
		loop {
			if *pos >= s.len() {
				return Err("unterminated quote".into());
			}
			let c = char_at(s, *pos);
			if c == '\\' && *pos + 1 < s.len() {
				let next = char_at(s, *pos + 1);
				out.push(c);
				out.push(next);
				*pos += 1 + next.len_utf8();
				continue;
			}
			*pos += c.len_utf8();
			if c == q {
				return Ok(out);
			}
			out.push(c);
		}
	}
	let mut segments: Vec<Segment> = Vec::new();
	loop {
		skip_ws(bytes, &mut pos);
		if pos >= bytes.len() {
			return Err("empty path".into());
		}
		// Field name: quoted, bare, or (lookups only) the `*` name wildcard.
		let mut star = false;
		let name = if bytes[pos] == b'"' || bytes[pos] == b'\'' {
			read_quoted(input, &mut pos)?
		} else if stars && bytes[pos] == b'*' {
			pos += 1;
			star = true;
			"*".to_string()
		} else {
			let start = pos;
			// Bare-name chars are ASCII, so the byte-as-char view is exact
			// (bytes >= 0x80 map to chars the predicate rejects either way).
			while pos < bytes.len() && is_bare_name_char(char::from(bytes[pos])) {
				pos += 1;
			}
			if pos == start {
				return Err(format!(
					"expected field name, found '{}'",
					char_at(input, pos)
				));
			}
			input[start..pos].to_string()
		};
		let mut selector: Option<Selector> = None;
		skip_ws(bytes, &mut pos);
		// Optional selector, with its optional sugar colon (colon counts as
		// selector sugar only when the next non-ws char is an open bracket).
		let mut bracket_at: Option<usize> = None;
		if pos < bytes.len() && bytes[pos] == b'[' {
			bracket_at = Some(pos);
		} else if pos < bytes.len() && bytes[pos] == b':' {
			let mut q = pos + 1;
			skip_ws(bytes, &mut q);
			if q < bytes.len() && bytes[q] == b'[' {
				bracket_at = Some(q);
			}
		}
		if let Some(b) = bracket_at {
			pos = b + 1;
			skip_ws(bytes, &mut pos);
			if pos < bytes.len() && (bytes[pos] == b'"' || bytes[pos] == b'\'') {
				let v = read_quoted(input, &mut pos)?;
				selector = Some(Selector::ByValue {
					text: v,
					quoted: true,
				}); // quotes force a value match, even numeric - and scalar-only
			} else {
				let start = pos;
				while pos < bytes.len() && bytes[pos] != b']' {
					pos += 1;
				}
				let body: String = trim_wsp(&input[start..pos]).to_string();
				selector = Some(if body == "*" {
					Selector::Wildcard
				} else if let Some(n) = body.strip_prefix('#').and_then(|d| d.parse::<u64>().ok()) {
					Selector::ByIndex(n)
				} else if let Ok(n) = body.parse::<u64>() {
					Selector::ByIndex(n)
				} else if index_shape(&body) {
					// All digits but past u64: an index no instance can have,
					// not a value selector that would create one on a write.
					Selector::ByIndex(u64::MAX)
				} else if body.is_empty() {
					return Err("empty selector".into());
				} else {
					Selector::ByValue {
						text: normalize_dangling_backslash(body),
						quoted: false,
					}
				});
			}
			skip_ws(bytes, &mut pos);
			if pos >= bytes.len() || bytes[pos] != b']' {
				return Err("unterminated selector".into());
			}
			pos += 1;
			skip_ws(bytes, &mut pos);
		}
		if star && selector.is_some() {
			return Err("selector on a name wildcard".into());
		}
		// Names resolve escapes, the same rule values follow when they are
		// compared: two spellings of one name are one name. name_src keeps the
		// source spelling, which is what `authored_name` hands back - empty
		// when it matches, the same sentinel NodeData uses.
		//
		// A name with no backslash and no upper case is already its own
		// resolved, folded spelling, so the scanner's buffer becomes the name
		// and nothing is allocated. That is nearly every name in a document,
		// and this runs once per segment per line: building and then freeing
		// two more strings each time was a third of the parse on a flat file.
		let plain = !name.contains('\\') && !name.bytes().any(|b| b.is_ascii_uppercase());
		let (seg_name, seg_src) = if plain {
			(name, String::new())
		} else {
			(fold_name(&key_text(&name)).into_owned(), name)
		};
		segments.push(Segment {
			name: seg_name,
			name_src: seg_src,
			selector,
			star,
		});
		if pos >= bytes.len() {
			return Ok(PathScan {
				segments,
				value_text: None,
			});
		}
		match bytes[pos] {
			b'.' => {
				pos += 1;
			}
			b':' => {
				pos += 1;
				return Ok(PathScan {
					segments,
					value_text: Some(trim_wsp(&input[pos..]).to_string()),
				});
			}
			_ => return Err(format!("unexpected '{}' after field", char_at(input, pos))),
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
	// Per-node hash-of-(name, value-key) -> matching children, parallel to
	// arena and lazily boxed so leaves never allocate one. Pure lookup
	// accelerator for select_or_create; children keeps the order. No key
	// strings are stored - a hit is verified against the arena with merge_eq.
	// The box is the point: an inline Option<HashMap> costs 48 bytes per node.
	#[allow(clippy::box_collection)]
	child_map: Vec<Option<Box<HashMap<u64, Slot>>>>,
	// Per-node hash-of-(name, display) -> first matching child: the `[value]`
	// selector accelerator (its predicate is display(), a different and
	// non-injective key from child_map's). Same first-wins discipline, same
	// mutation sites; ownership is by hash, and a query verifies its hit.
	#[allow(clippy::box_collection)]
	disp_map: Vec<Option<Box<HashMap<u64, usize>>>>,
	// Whole-line comments waiting for the next line that binds a node. The
	// source indent is kept only to decide after-attachment (a comment deeper
	// than the next binding hangs on the block it sits in).
	pending: Vec<Pend>,
	// (end index, indent length): every pending entry before `end` has a
	// ceiling at or under that length. Lengths rise along the stack, so a
	// hang check pops the marks above its own indent and walks only what
	// they covered. Without it a run of retained bad lines is rewalked per
	// line and a plain text file parses in quadratic time.
	pend_marks: Vec<(usize, usize)>,
	saw_blank: bool, // a blank line waits to become the next bound node's blank_before
	// An open stacked list defers its merge-key remap (rebuilding the key per
	// element is O(list^2) time); (node, key hash, display hash) at deferral
	// start, flushed before any map lookup and at end of parse.
	star_open: Option<(usize, u64, u64)>,
	// Node -> line of the re-open that H002-hinted it. A merge under a hinted
	// container combines the same two textual regions, so it hints too even
	// when it lands on the newest child at its own scope - that is how every
	// merged level reports, not just the outermost. The stored line splits old
	// children (hint) from ones the re-opened region itself created (silent).
	reentered: HashMap<usize, usize>,
	lost: usize, // dropped lines/values canonical output cannot re-emit
	// parse_limited's caps, 0 = uncapped: nodes counted against the arena
	// (root excluded), elements against a single value's cell, diagnostics
	// against the list. Past the diagnostic cap nothing is listed, only
	// counted (errors, hints), for the one tail entry the parse ends with.
	max_nodes: usize,
	max_elements: usize,
	max_diags: usize,
	unlisted: (usize, usize),
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
				trivia: None,
				blank_before: false,
				src_set: false,
				src: None,
				name_src: String::new(),
			}],
			diags: Vec::new(),
			stack: vec![(String::new(), ROOT)],
			child_map: vec![None],
			disp_map: vec![None],
			pending: Vec::new(),
			pend_marks: Vec::new(),
			saw_blank: false,
			star_open: None,
			reentered: HashMap::new(),
			lost: 0,
			max_nodes: 0,
			max_elements: 0,
			max_diags: 0,
			unlisted: (0, 0),
		}
	}

	fn limited(max_nodes: usize, max_elements: usize, max_diags: usize) -> Parser {
		let mut p = Parser::new();
		p.max_nodes = max_nodes;
		p.max_elements = max_elements;
		p.max_diags = max_diags;
		p
	}

	/// Every parse diagnostic goes through here, so the cap sees them all.
	fn diag(&mut self, d: Diagnostic) {
		if self.max_diags != 0 && self.diags.len() >= self.max_diags {
			if d.severity == Severity::Error {
				self.unlisted.0 += 1;
			} else {
				self.unlisted.1 += 1;
			}
			return;
		}
		self.diags.push(d);
	}

	fn err(&mut self, line: usize, code: &'static str, msg: impl Into<String>) {
		let message = msg.into();
		self.diag(Diagnostic {
			line,
			severity: Severity::Error,
			message,
			code,
		});
	}

	/// Find (or create by merge rule) the child of `parent` with this (name, value).
	fn select_or_create(
		&mut self,
		parent: usize,
		name: &str,
		name_src: &str,
		value: Value,
		line: usize,
	) -> usize {
		self.star_flush();
		let h = merge_hash(name, &value);
		if let Some(slot) = self.child_map[parent].as_deref().and_then(|m| m.get(&h))
			&& let Some(c) = slot
				.first_match(|c| merge_eq(&self.arena[c].name, &self.arena[c].value, name, &value))
		{
			return c;
		}
		let idx = self.arena.len();
		let hd = disp_hash(name, &value);
		self.arena.push(NodeData {
			name: name.to_string(),
			name_src: spelled(name, name_src),
			value,
			children: Vec::new(),
			parent,
			line,
			star_list: false,
			star_mixed: false,
			trivia: None,
			blank_before: false,
			src_set: false,
			src: None,
		});
		self.arena[parent].children.push(idx);
		self.child_map.push(None);
		self.disp_map.push(None);
		self.child_map[parent]
			.get_or_insert_with(Default::default)
			.entry(h)
			.and_modify(|s| s.push(idx))
			.or_insert(Slot::One(idx));
		self.disp_map[parent]
			.get_or_insert_with(Default::default)
			.entry(hd)
			.or_insert(idx);
		idx
	}

	/// Apply an open stacked list's deferred remap. Runs before any map lookup
	/// (and at end of parse), so both maps are always fresh when queried.
	fn star_flush(&mut self) {
		if let Some((node, key, disp)) = self.star_open.take() {
			self.remap_child(node, key, disp);
		}
	}

	/// A node's value mutated in place (empty field filled, star element added):
	/// move its map entry from the old key to the new one. First-wins on both
	/// sides so lookups keep matching the earliest sibling, like the scan did.
	fn remap_child(&mut self, node: usize, old_key: u64, old_disp: u64) {
		let parent = self.arena[node].parent;
		if let Some(m) = self.child_map[parent].as_deref_mut()
			&& let Some(slot) = m.get_mut(&old_key)
			&& slot.remove(node)
		{
			m.remove(&old_key);
		}
		let new_key = merge_hash(&self.arena[node].name, &self.arena[node].value);
		let already = self.child_map[parent]
			.as_deref()
			.and_then(|m| m.get(&new_key))
			.and_then(|s| {
				s.first_match(|c| {
					merge_eq(
						&self.arena[c].name,
						&self.arena[c].value,
						&self.arena[node].name,
						&self.arena[node].value,
					)
				})
			});
		if already.is_none() {
			self.child_map[parent]
				.get_or_insert_with(Default::default)
				.entry(new_key)
				.and_modify(|s| s.push(node))
				.or_insert(Slot::One(node));
		}
		if let Some(m) = self.disp_map[parent].as_deref_mut()
			&& m.get(&old_disp) == Some(&node)
		{
			m.remove(&old_disp);
		}
		let new_disp = disp_hash(&self.arena[node].name, &self.arena[node].value);
		self.disp_map[parent]
			.get_or_insert_with(Default::default)
			.entry(new_disp)
			.or_insert(node);
	}

	/// A value that mutates after its sibling group was keyed - an empty field
	/// filled by a fence, a stacked list closed - can land on a key an earlier
	/// sibling already holds, which the keyed lookup can no longer catch. Fold
	/// those pairs so the tree matches a reparse of its own canonical text.
	/// Depth-first, since folding can carry duplicates down a level.
	fn fold_late_dups(&mut self) {
		let mut stack = vec![ROOT];
		while let Some(parent) = stack.pop() {
			let kids = std::mem::take(&mut self.arena[parent].children);
			let mut first: HashMap<u64, Slot> = HashMap::new();
			let mut keep: Vec<usize> = Vec::with_capacity(kids.len());
			for c in kids {
				let h = merge_hash(&self.arena[c].name, &self.arena[c].value);
				let survivor = first.get(&h).and_then(|s| {
					s.first_match(|x| {
						merge_eq(
							&self.arena[x].name,
							&self.arena[x].value,
							&self.arena[c].name,
							&self.arena[c].value,
						)
					})
				});
				match survivor {
					Some(s) => fold_node_into(&mut self.arena, s, c),
					None => {
						first
							.entry(h)
							.and_modify(|s| s.push(c))
							.or_insert(Slot::One(c));
						keep.push(c);
					}
				}
			}
			stack.extend(keep.iter().copied());
			self.arena[parent].children = keep;
		}
	}

	/// Hand pending leading comments (and this line's trailing one) to a node.
	/// First trailing wins; a later one demotes to leading so nothing is lost.
	fn attach_trivia(&mut self, node: usize, trailing: Option<&str>) {
		if !self.pending.is_empty() {
			let t = self.arena[node].triv_mut();
			for p in self.pending.drain(..) {
				t.leading.push(Lead {
					text: p.text,
					blank_before: p.blank_before,
				});
			}
			self.pend_marks.clear();
		}
		if let Some(tr) = trailing {
			let t = self.arena[node].triv_mut();
			if t.trailing.is_empty() {
				t.trailing = tr.to_string();
			} else {
				t.leading.push(Lead::plain(tr.to_string()));
			}
		}
	}

	/// Comments written deeper than the incoming line belong to the block they
	/// sit in, not to the next binding: hang each on the deepest node whose
	/// bound indent prefixes the comment's, among the levels the incoming
	/// line is closing. Written at that node's own level the comment trails
	/// it (`after`); written deeper it sits inside the node's block
	/// (`inside`) - so a header whose children are all commented still owns
	/// them at their depth. Runs before the incoming line resolves (and at
	/// end of parse with the empty indent, so tail comments keep their block).
	fn hang_deeper_pending(&mut self, new_indent: &str) {
		if self.pending.is_empty() {
			return;
		}
		let new_len = new_indent.len();
		// Only entries above the last mark at or under this indent can hang.
		while self.pend_marks.last().is_some_and(|m| m.1 > new_len) {
			self.pend_marks.pop();
		}
		let start = self.pend_marks.last().map_or(0, |m| m.0);
		let taken: Vec<Pend> = self.pending.drain(start..).collect();
		for mut p in taken {
			if p.ceiling > new_len {
				// A level shallower than the incoming line stays open and may
				// still gain children, so a comment must not hang there - it
				// would emit below the child; keep it pending instead.
				let target = self
					.stack
					.iter()
					.rev()
					.find(|(ind, node)| {
						*node != ROOT
							&& *node != DEAD && *node != UNOPENED
							&& ind.len() >= new_indent.len()
							&& p.indent.starts_with(ind.as_str())
					})
					.map(|(ind, n)| (*n, ind.len() == p.indent.len()));
				// A root node's trailing comment emits at column zero, which
				// is exactly how the document's own trailing comment is
				// spelled, so keeping the two apart here made a merge depend on
				// whether the layer had been formatted first. Let it orphan,
				// the way a reload of this document's own output reads it. A
				// comment deeper than the node keeps an indent of its own and
				// comes back where it was, so it still hangs.
				let expressible =
					target.is_some_and(|(n, own)| !own || self.arena[n].parent != ROOT);
				if let (Some((n, at_own_level)), true) = (target, expressible) {
					let lead = Lead {
						text: p.text,
						blank_before: p.blank_before,
					};
					if at_own_level {
						self.arena[n].triv_mut().after.push(lead);
					} else {
						self.arena[n].triv_mut().inside.push(lead);
					}
					continue;
				}
				p.ceiling = new_len;
			}
			self.pending.push(p);
		}
		match self.pend_marks.last_mut() {
			Some(m) if m.1 == new_len => m.0 = self.pending.len(),
			_ => self.pend_marks.push((self.pending.len(), new_len)),
		}
	}

	/// Resolve which open level this indent belongs to. Child only when the
	/// current top's indent is a proper prefix; otherwise the indent must equal
	/// an open level exactly (dedent), else it is a recoverable error.
	fn resolve_parent(&mut self, indent: &str) -> Option<usize> {
		let Some((top_indent, top_node)) = self.stack.last() else {
			return None; // sentinel invariant; degrade, never abort
		};
		if indent.len() > top_indent.len() && indent.starts_with(top_indent.as_str()) {
			return Some(if *top_node == UNOPENED {
				DEAD
			} else {
				*top_node
			});
		}
		for i in (0..self.stack.len()).rev() {
			if self.stack[i].0 == indent && self.stack[i].1 != UNOPENED {
				// Sibling of stack[i]: its parent is the entry below it.
				let parent = if i == 0 { ROOT } else { self.stack[i - 1].1 };
				// Keep the sentinel; a top-level line resolves to ROOT.
				self.stack.truncate(i.max(1));
				return Some(if parent == UNOPENED { DEAD } else { parent });
			}
		}
		// Skipped, but it still owns its indent: whatever is written deeper is
		// skipped with it, and a sibling at the same bad indent is refused the
		// same way instead of binding one level up.
		while self.stack.len() > 1 {
			let top = &self.stack[self.stack.len() - 1].0;
			if indent.len() > top.len() && indent.starts_with(top.as_str()) {
				break;
			}
			self.stack.pop();
		}
		self.stack.push((indent.to_string(), UNOPENED));
		None
	}

	/// Diagnose a line written under a skipped line, and skip it too. Its own
	/// level stays dead so deeper lines go the same way.
	fn skip_under_dead(&mut self, line: usize, indent: &str) {
		self.err(line, "E018", "parent line was skipped; line skipped");
		self.lost += 1;
		self.stack.push((indent.to_string(), DEAD));
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
		// Owned here and handed to the last segment once; Option so the loop
		// can move it out without a clone.
		let mut value = Some(value);
		self.star_flush();
		// Field child under a stacked list: diagnose the mix once, keep the field.
		if self.arena[parent].star_list && !self.arena[parent].star_mixed {
			self.arena[parent].star_mixed = true;
			self.err(line, "E001", "field mixed with list elements");
		}
		// Nesting cap: parent depth plus the segments this line adds. Checked
		// before any node is created so a rejected line leaves nothing behind.
		let mut parent_depth = 0usize;
		let mut up = parent;
		while up != ROOT {
			parent_depth += 1;
			up = self.arena[up].parent;
		}
		if parent_depth + segs.len() > MAX_DEPTH {
			self.err(
				line,
				"E016",
				format!("nesting deeper than {} levels; line skipped", MAX_DEPTH),
			);
			self.lost += 1;
			return None;
		}
		let mut cur = parent;
		for (i, seg) in segs.iter().enumerate() {
			let is_last = i + 1 == segs.len();
			match (&seg.selector, is_last) {
				(Some(Selector::ByValue { text, quoted }), _) => {
					// Same escape-applied display predicate resolve_from uses, so
					// a selector also selects an array-valued instance instead of
					// creating a spurious second one - via the disp_map accelerator
					// (the inline spelling was quadratic in siblings without it).
					// Create only when nothing matches.
					// A quoted selector is scalar-only, so it is the one that needs the
					// fallback scan: the accelerator keeps just the first same-display
					// child, which may be the non-scalar one. An unquoted selector takes
					// whatever the accelerator holds and does not scan, so it can bind a
					// raw block where a quoted selector picks the scalar sibling.
					let found = self.find_by_value(cur, &seg.name, text, *quoted);
					cur = match found {
						Some(c) => c,
						None => {
							let disc = Value::Cell(vec![Element {
								text: text.clone(),
								quoted: false,
							}]);
							self.select_or_create(cur, &seg.name, &seg.name_src, disc, line)
						}
					};
					if is_last && value.as_ref().is_some_and(|v| !v.is_empty()) {
						// `a.b[X]: v` - the discriminator is the value; a second
						// value has nowhere unambiguous to go.
						self.err(
							line,
							"E002",
							format!("value after selector on '{}' ignored", seg.name),
						);
						self.lost += 1;
					}
				}
				(Some(Selector::ByIndex(n)), _) => {
					let found = index_usize(*n).and_then(|i| {
						self.arena[cur]
							.children
							.iter()
							.copied()
							.filter(|&c| self.arena[c].name == seg.name)
							.nth(i)
					});
					if let Some(found) = found {
						cur = found;
					} else {
						self.err(line, "E003", format!("no instance {} of '{}'", n, seg.name));
						self.lost += 1;
						return None;
					}
					if is_last && value.as_ref().is_some_and(|v| !v.is_empty()) {
						// Same as the value selector: the instance is already
						// chosen, so a trailing value has nowhere to bind.
						self.err(
							line,
							"E002",
							format!("value after selector on '{}' ignored", seg.name),
						);
						self.lost += 1;
					}
				}
				(Some(Selector::Wildcard), _) => {
					self.err(line, "E004", "wildcard selector is query-only");
					self.lost += 1;
					return None;
				}
				(None, false) => {
					cur = self.select_or_create(cur, &seg.name, &seg.name_src, Value::Empty, line);
				}
				(None, true) => {
					let parent = cur;
					let before = self.arena.len();
					let v = value.take().unwrap_or(Value::Empty);
					cur = self.select_or_create(cur, &seg.name, &seg.name_src, v, line);
					// Two separately-written bindings just combined: legal (the
					// merge rule), but only the parser can see it happened, so
					// say so. Adjacent re-mentions (still the newest binding at
					// this scope) and selector/path-intermediate merges stay
					// silent - those are the deliberate redundant-path idiom.
					// Under a hinted container the newest-child pass does not
					// apply to children the earlier region wrote: those merges
					// combine the same two regions, so every level reports.
					if cur < before && self.arena[cur].line != line {
						let non_last = self.arena[parent].children.last() != Some(&cur);
						let cross_region = self
							.reentered
							.get(&parent)
							.is_some_and(|&rl| self.arena[cur].line < rl);
						if non_last || cross_region {
							let at = self.arena[cur].line;
							let name = seg.name.clone();
							self.diag(Diagnostic {
								line,
								severity: Severity::Hint,
								message: format!(
									"{}line {} (same name and value combine)",
									h002_head(&name),
									at
								),
								code: "H002",
							});
							self.reentered.insert(cur, line);
						}
					}
				}
			}
		}
		Some(cur)
	}

	/// The child of `cur` named `name` whose display form is the selector text
	/// (escapes applied), or None. Quoted selectors only match a single scalar.
	fn find_by_value(&self, cur: usize, name: &str, text: &str, quoted: bool) -> Option<usize> {
		let want = apply_escapes(text);
		self.disp_map[cur]
			.as_deref()
			.and_then(|m| m.get(&disp_hash_text(name, &want)))
			.copied()
			.filter(|&c| self.arena[c].name == name && disp_key(&self.arena[c].value) == want)
			.filter(|&c| !quoted || single_scalar(&self.arena[c].value))
			.or_else(|| {
				if !quoted {
					return None;
				}
				self.arena[cur].children.iter().copied().find(|&c| {
					self.arena[c].name == name
						&& single_scalar(&self.arena[c].value)
						&& disp_key(&self.arena[c].value) == want
				})
			})
	}

	/// Consume raw-block content after an opening fence. Returns (value, next line
	/// index). The closing fence's indent is stripped from each content line
	/// (the opening line's when the block never closes); the rest is content.
	fn consume_raw(
		&mut self,
		lines: &[&str],
		mut i: usize,
		open_line: usize,
		open_indent: &str,
		fence: (u8, usize, String),
	) -> (Value, usize) {
		let (ch, len, info) = fence;
		let mut content: Vec<&str> = Vec::new();
		let mut nest = open_indent;
		let mut closed = false;
		while i < lines.len() {
			if is_fence_close(lines[i], ch, len) {
				// The closing fence's indent is the nesting; everything a content
				// line carries past it is content, so a body whose lines all
				// share an indent keeps it (a writer-built block depends on that).
				nest = leading_ws(lines[i]);
				closed = true;
				i += 1;
				break;
			}
			content.push(lines[i]);
			i += 1;
		}
		if !closed {
			self.err(open_line, "E005", "unterminated raw block");
		}
		let stripped: Vec<&str> = content.iter().map(|l| strip_common(l, nest)).collect();
		(
			Value::Raw(Box::new(RawVal {
				content: stripped.join("\n"),
				info,
				fence_char: ch,
				fence_len: len,
			})),
			i,
		)
	}

	/// A bare fence line is a value line for its parent field: fills an empty
	/// value, else creates a new instance of that field (the repeated-leaf rule).
	/// Returns the node the block landed on (None = no parent, diagnosed).
	fn bind_block(&mut self, parent: usize, value: Value, line: usize) -> Option<usize> {
		if parent == ROOT {
			self.err(line, "E006", "raw block with no parent field");
			self.lost += 1;
			return None;
		}
		if self.arena[parent].value.is_empty() {
			let old_key = merge_hash(&self.arena[parent].name, &self.arena[parent].value);
			let old_disp = disp_hash(&self.arena[parent].name, &self.arena[parent].value);
			self.arena[parent].value = value;
			self.remap_child(parent, old_key, old_disp);
			Some(parent)
		} else {
			// Copied out: select_or_create takes the arena mutably.
			let (name, name_src, grandparent) = (
				self.arena[parent].name.clone(),
				self.arena[parent].authored().to_string(),
				self.arena[parent].parent,
			);
			Some(self.select_or_create(grandparent, &name, &name_src, value, line))
		}
	}

	/// One stacked-list element (`* scalar`) appends to the parent's array.
	/// True when the element was added; false when the line was dropped.
	fn add_star_element(&mut self, parent: usize, body: &str, line: usize) -> bool {
		if parent == ROOT {
			self.err(line, "E007", "list element with no parent field");
			self.lost += 1;
			return false;
		}
		// Uniform-or-nothing (spec): a mix with field children is not a block array.
		if !self.arena[parent].children.is_empty() {
			self.err(
				line,
				"E008",
				"list element mixed with field children; ignored",
			);
			self.lost += 1;
			return false;
		}
		let trimmed = trim_wsp(body);
		if trimmed.is_empty() {
			self.err(line, "E009", "empty list element");
			self.lost += 1;
			return false;
		}
		// One scalar per line; a bare comma is an error, not a second element.
		if split_unquoted_commas(trimmed).len() > 1 {
			self.err(
				line,
				"E010",
				"bare comma in list element (one element per line)",
			);
			self.lost += 1;
			return false;
		}
		if unterminated_quote(trimmed) {
			self.err(line, "E017", "unterminated quote in value");
		}
		let Some(el) = parse_element(trimmed) else {
			self.err(line, "E009", "empty list element");
			self.lost += 1;
			return false;
		};
		// Element cap: each element line past it is refused on its own, the way
		// any other bad element line is.
		if self.max_elements != 0
			&& let Value::Cell(els) = &self.arena[parent].value
			&& els.len() >= self.max_elements
		{
			self.err(
				line,
				"E021",
				format!(
					"array longer than {} elements; line skipped",
					self.max_elements
				),
			);
			self.lost += 1;
			return false;
		}
		if self.arena[parent].value.is_empty() {
			let old_key = merge_hash(&self.arena[parent].name, &self.arena[parent].value);
			let old_disp = disp_hash(&self.arena[parent].name, &self.arena[parent].value);
			self.arena[parent].value = Value::Cell(vec![el]);
			self.arena[parent].star_list = true;
			// First element: remap now (Empty -> cell changes both keys), then
			// open the deferral window with the current keys. Rebuilding the
			// keys per appended element was O(list^2) time; the maps only need
			// to be fresh when queried, and every query flushes first.
			self.remap_child(parent, old_key, old_disp);
			let k = merge_hash(&self.arena[parent].name, &self.arena[parent].value);
			let d = disp_hash(&self.arena[parent].name, &self.arena[parent].value);
			self.star_open = Some((parent, k, d));
		} else if matches!(self.arena[parent].value, Value::Cell(_)) && self.arena[parent].star_list
		{
			if !matches!(self.star_open, Some((n, _, _)) if n == parent) {
				self.star_flush();
				let old_key = merge_hash(&self.arena[parent].name, &self.arena[parent].value);
				let old_disp = disp_hash(&self.arena[parent].name, &self.arena[parent].value);
				self.star_open = Some((parent, old_key, old_disp));
			}
			if let Value::Cell(els) = &mut self.arena[parent].value {
				els.push(el);
			}
		} else {
			self.err(
				line,
				"E011",
				"field already has a value; list element ignored",
			);
			self.lost += 1;
			return false;
		}
		true
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
					hints.push((line, format!("{}{}'?", h001_head(name), joined)));
				}
			}
		}
		for (line, message) in hints {
			self.diag(Diagnostic {
				line,
				severity: Severity::Hint,
				message,
				code: "H001", // repeated bare leaf
			});
		}
	}

	fn parse(mut self, text: &str, strictness: Strictness) -> Document {
		// UTF-8 BOM strip, then split keeping raw lines (CR stripped per line).
		// Lines borrow `text`: they are only read, so no owned copies needed.
		// The whole trailing CR run goes, not just one: a raw block keeps its
		// content untrimmed, so a line left ending in CR would be written back as
		// CRLF and read as neither - the one shape where the count is visible.
		let text = text.strip_prefix('\u{feff}').unwrap_or(text);
		let mut lines: Vec<&str> = text.split('\n').map(|l| l.trim_end_matches('\r')).collect();
		// A newline-terminated text splits into one more piece than it has
		// lines. An unterminated raw block took that empty tail as a body
		// line, so the same last line read differently with and without its
		// newline, which the grammar says are one document.
		if text.ends_with('\n') {
			lines.pop();
		}
		let mut i = 0usize;
		let mut node_capped = false;
		while i < lines.len() {
			// Node cap: reported at the first line not parsed, so the count can
			// overshoot by at most one line's path. The unparsed remainder counts
			// as lost, which is what keeps save_file from writing a silently
			// truncated document.
			if self.max_nodes != 0 && self.arena.len() - 1 > self.max_nodes {
				self.err(
					i + 1,
					"E020",
					format!("node cap of {} exceeded; parse stopped", self.max_nodes),
				);
				self.lost += lines[i..]
					.iter()
					.filter(|l| !trim_wsp(l).is_empty())
					.count();
				node_capped = true;
				break;
			}
			let lineno = i + 1;
			let line = trim_wsp_end(lines[i]);
			// Indent chars are ASCII space/tab, so a byte scan slices the same run.
			let ilen = line
				.bytes()
				.take_while(|&b| b == b' ' || b == b'\t')
				.count();
			let indent = &line[..ilen];
			let rest = &line[ilen..];
			if rest.is_empty() {
				self.saw_blank = true;
				i += 1;
				continue;
			}
			// Whole-line comment: hold it for the next line that binds a node.
			// It consumes a pending blank into its own flag, so a blank between
			// comment-only regions survives the round-trip.
			if rest.starts_with('#') {
				self.pending.push(Pend {
					text: rest.to_string(),
					indent: indent.to_string(),
					blank_before: std::mem::take(&mut self.saw_blank),
					ceiling: indent.len(),
				});
				i += 1;
				continue;
			}
			// Any other line consumes the pending blank; only a field line that
			// binds turns it into grouping.
			let had_blank = std::mem::take(&mut self.saw_blank);
			// A binding line claims the pending comments - but deeper-written
			// ones hang on their own block first.
			self.hang_deeper_pending(indent);
			// Child-indent fence: a value line for its parent field.
			if let Some(fence) = fence_open(rest) {
				let parent = self.resolve_parent(indent);
				let (value, next) = self.consume_raw(&lines, i + 1, lineno, indent, fence);
				let Some(parent) = parent else {
					// The body goes with its fence: parsed live, it would read as
					// root bindings and the closing fence would open a second block.
					self.err(lineno, "E012", "indentation matches no open level");
					self.lost += 1;
					i = next;
					continue;
				};
				if parent == DEAD {
					self.skip_under_dead(lineno, indent);
				} else if let Some(node) = self.bind_block(parent, value, lineno) {
					self.attach_trivia(node, None);
				}
				i = next;
				continue;
			}
			// Stacked-list element: colon-less by construction ('*' can't begin a name).
			if let Some(after) = rest.strip_prefix('*') {
				// A `*` alone after the trim: whether a space followed it
				// decides between an empty element and a malformed line, and
				// only the untrimmed line still knows.
				let spaced = after.starts_with([' ', '\t'])
					|| (after.is_empty()
						&& matches!(lines[i].as_bytes().get(ilen + 1), Some(b' ' | b'\t')));
				if spaced {
					let Some(parent) = self.resolve_parent(indent) else {
						self.err(lineno, "E012", "indentation matches no open level");
						self.lost += 1;
						i += 1;
						continue;
					};
					if parent == DEAD {
						self.skip_under_dead(lineno, indent);
						i += 1;
						continue;
					}
					let (body, comment) = split_value_comment(after);
					// Elements have no node of their own; trivia rides the field.
					// At the root there is no field (E007), so the comment rides
					// the document like any other pending one.
					if parent != ROOT {
						self.attach_trivia(parent, comment);
					} else if let Some(c) = comment {
						self.pending.push(Pend {
							text: c.to_string(),
							indent: indent.to_string(),
							blank_before: had_blank,
							ceiling: indent.len(),
						});
					}
					// A dropped element holds its indent level like any
					// skipped line, so what is written under it is skipped
					// with it (E018) rather than re-parenting to the field.
					if !self.add_star_element(parent, body, lineno) {
						self.stack.push((indent.to_string(), DEAD));
					}
					i += 1;
					continue;
				}
				let Some(parent) = self.resolve_parent(indent) else {
					self.err(lineno, "E012", "indentation matches no open level");
					self.lost += 1;
					i += 1;
					continue;
				};
				if parent == DEAD {
					self.skip_under_dead(lineno, indent);
					i += 1;
					continue;
				}
				self.err(
					lineno,
					"E013",
					"malformed line: '*' must be followed by a space",
				);
				// Content-malformed at any position, so it is safe to retain
				// verbatim as trivia: re-emitted, it re-diagnoses identically
				// and can never read as a live binding. A hand-typo no longer
				// vanishes on the consumer's next save. The BOM exception the
				// sibling site below carries cannot apply here: this line
				// starts with the '*' that brought us in.
				self.pending.push(Pend {
					text: trim_wsp_end(rest).to_string(),
					indent: indent.to_string(),
					blank_before: had_blank,
					ceiling: indent.len(),
				});
				self.stack.push((indent.to_string(), DEAD));
				i += 1;
				continue;
			}
			// Field line.
			let (mut before, mut comment) = split_comment(rest);
			// A same-line fence runs to the end of the line: the child-indent
			// spelling keeps a `#` in its info-string, the grammar gives the
			// same-line alternative no comment at all, and the emitter already
			// assumes it. Without this, `a: ```c#` loses the `#`. The cheap
			// test comes first so an ordinary commented line is not scanned
			// twice.
			if comment.is_some()
				&& (before.contains("```") || before.contains("~~~"))
				&& scan_path(trim_wsp_end(before))
					.is_ok_and(|s| s.value_text.is_some_and(|v| fence_open(&v).is_some()))
			{
				before = rest;
				comment = None;
			}
			let content = trim_wsp_end(before);
			if content.is_empty() {
				// Only a comment survived (e.g. an escaped lead-in); keep it.
				if let Some(c) = comment {
					self.pending.push(Pend {
						text: c.to_string(),
						indent: indent.to_string(),
						blank_before: had_blank,
						ceiling: indent.len(),
					});
				}
				i += 1;
				continue;
			}
			let Some(parent) = self.resolve_parent(indent) else {
				self.err(lineno, "E012", "indentation matches no open level");
				self.lost += 1;
				i += 1;
				continue;
			};
			if parent == DEAD {
				self.skip_under_dead(lineno, indent);
				i += 1;
				continue;
			}
			let scan = match scan_path(content) {
				Ok(s) => s,
				Err(reason) => {
					self.err(
						lineno,
						"E014",
						format!("malformed line skipped: {}", reason),
					);
					// Content-malformed at any position - retained as trivia,
					// same rationale (and same BOM exception) as the bad '*'
					// line above.
					if rest.starts_with('\u{feff}') {
						self.lost += 1;
					} else {
						self.pending.push(Pend {
							text: trim_wsp_end(rest).to_string(),
							indent: indent.to_string(),
							blank_before: had_blank,
							ceiling: indent.len(),
						});
					}
					self.stack.push((indent.to_string(), DEAD));
					i += 1;
					continue;
				}
			};
			let mut next = i + 1;
			// The verbatim value span, kept for reads' `raw` (only the plain
			// scalar/inline-array case has a one-line source spelling).
			let mut src_text: Option<&str> = None;
			let value = match &scan.value_text {
				None => {
					if let Some(body) = bracket_array_body(content) {
						if split_unquoted_commas(body).len() > 1 {
							// Two or more elements folded into one string. The
							// brackets never survive the load, so a rewrite
							// would bake the changed value in and the file
							// would check clean forever after. Count it lost so
							// the save gate stops that.
							self.err(
								lineno,
								"E019",
								"bracket array syntax; an array is comma-separated, without brackets",
							);
							self.lost += 1;
						} else {
							// One element reads as the selector the scanner made
							// of it - `[Boston]` is `Boston` - and `field:[disc]`
							// is documented sugar, so nothing is lost. A hint,
							// for the JSON habit; same code, like E022.
							self.diag(Diagnostic {
								line: lineno,
								severity: Severity::Hint,
								message: "bracket array syntax; read as a selector, the same value without the brackets".into(),
								code: "E019",
							});
						}
					} else {
						// A clean path with no colon is the one defined repair:
						// the obvious intent is that path with an empty value.
						self.err(lineno, "E015", "missing colon; repaired as an empty value");
					}
					Value::Empty
				}
				Some(v) if v.is_empty() => Value::Empty,
				Some(v) => {
					if let Some(fence) = fence_open(v) {
						// Same-line fence spelling.
						let (val, n) = self.consume_raw(&lines, i + 1, lineno, indent, fence);
						next = n;
						val
					} else {
						// Element cap: the whole line is refused, so a capped
						// load never holds a truncated array that would read
						// as the document's value. Counted before anything
						// splits the value (the quote check does too), or
						// the cap would bound nothing.
						if self.max_elements != 0 && cell_exceeds(v, self.max_elements) {
							self.err(
								lineno,
								"E021",
								format!(
									"array longer than {} elements; line skipped",
									self.max_elements
								),
							);
							self.lost += 1;
							self.stack.push((indent.to_string(), DEAD));
							i = next;
							continue;
						}
						if unterminated_quote(v) {
							self.err(lineno, "E017", "unterminated quote in value");
						}
						src_text = Some(v);
						parse_cell(v)
					}
				}
			};
			// Record only when the bound node holds exactly this line's value
			// (a merge into an equal-valued node keeps the first line's span;
			// a value dropped after a last-segment selector records nothing).
			let vkey = src_text.as_ref().map(|_| value_hash(&value));
			if let Some(node) = self.attach_path(parent, &scan.segments, value, lineno) {
				if let (Some(s), Some(k)) = (src_text, vkey)
					&& !self.arena[node].src_set
					&& value_hash(&self.arena[node].value) == k
				{
					self.arena[node].src_set = true;
					if !src_matches_display(&self.arena[node].value, s) {
						self.arena[node].src = Some(s.to_string());
					}
				}
				if had_blank {
					self.arena[node].blank_before = true;
				}
				self.attach_trivia(node, comment);
				self.stack.push((indent.to_string(), node));
			} else {
				self.stack.push((indent.to_string(), DEAD));
			}
			i = next;
		}
		// A cap crossed on the document's last line still reports, with nothing
		// left to skip.
		if !node_capped && self.max_nodes != 0 && self.arena.len() - 1 > self.max_nodes {
			self.err(
				lines.len(),
				"E020",
				format!("node cap of {} exceeded; parse stopped", self.max_nodes),
			);
		}
		self.star_flush();
		self.fold_late_dups();
		self.emit_repeated_leaf_hints();
		// Indented tail comments keep their block; only top-level ones orphan.
		self.hang_deeper_pending("");
		let mut orphans: Vec<Lead> = self
			.pending
			.drain(..)
			.map(|p| Lead {
				text: p.text,
				blank_before: p.blank_before,
			})
			.collect();
		// The emitter drops a blank before the first thing it prints, so a
		// document that kept one there would not survive its own canonical
		// form: `load(emit(load(x)))` and `load(x)` would differ on that bit,
		// and a merge - where the line is no longer first - would place a blank
		// the author never wrote. Clear it here, once, wherever output starts.
		if let Some(&first) = self.arena[ROOT].children.first() {
			let n = &mut self.arena[first];
			match n.trivia.as_mut().and_then(|t| t.leading.first_mut()) {
				Some(c) => c.blank_before = false,
				None => n.blank_before = false,
			}
		} else if let Some(c) = orphans.first_mut() {
			c.blank_before = false;
		}
		// The one entry past the cap: what was not listed, and whether any
		// of it was an error, so a consumer scanning the list for errors
		// still finds one and a Strict load still fails.
		let (errors, hints) = self.unlisted;
		if errors + hints > 0 {
			self.diags.push(Diagnostic {
				line: 0, // about the list, not a line
				severity: if errors > 0 {
					Severity::Error
				} else {
					Severity::Hint
				},
				code: "E022",
				message: format!(
					"diagnostic cap of {} reached; {} more not listed, {} of them errors",
					self.max_diags,
					errors + hints,
					errors
				),
			});
		}
		Document {
			arena: self.arena,
			diags: self.diags,
			strictness,
			orphans,
			lost: self.lost,
			index: std::sync::OnceLock::new(),
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

	/// Parse at a chosen strictness. Only Strict can fail (any error diagnostic);
	/// the error still carries the parsed document alongside the diagnostics.
	// The Err carries the whole document by design (recover-and-continue);
	// boxing it would change the public shape for a value built once per load.
	#[allow(clippy::result_large_err)]
	pub fn parse_with(text: &str, strictness: Strictness) -> Result<Document, LoadError> {
		let doc = Parser::new().parse(text, strictness);
		if strictness == Strictness::Strict
			&& doc.diags.iter().any(|d| d.severity == Severity::Error)
		{
			return Err(LoadError {
				diagnostics: doc.diags.clone(),
				document: doc,
			});
		}
		Ok(doc)
	}

	/// Parse with resource caps beside the strictness, for input the consumer
	/// does not control: a document amplifies to many times its byte size in
	/// memory, so a size cap alone cannot bound what a load allocates.
	/// `max_nodes` stops the parse once a line takes the node count past it -
	/// one `E020` error, and the unparsed remainder counts as lost so a save
	/// cannot silently truncate. `max_elements` refuses any line whose array
	/// would hold more elements (`E021`, that line alone is skipped).
	/// `max_diags` bounds the diagnostics list itself - a document of nothing
	/// but bad lines costs a diagnostic per line - by listing the first that
	/// many and ending with one `E022` that counts the rest; error_count and
	/// the Strict gate see that tail as an error whenever an unlisted one was.
	/// 0 disables a cap, making this parse_with. The caps are parse-time only;
	/// the write API is the consumer's own arithmetic. As everywhere, the Err
	/// fires only at Strict, and a cap diagnostic is an error, so a capped
	/// Strict load fails; at other levels the parsed part stays readable.
	#[allow(clippy::result_large_err)]
	pub fn parse_limited(
		text: &str,
		strictness: Strictness,
		max_nodes: usize,
		max_elements: usize,
		max_diags: usize,
	) -> Result<Document, LoadError> {
		let doc = Parser::limited(max_nodes, max_elements, max_diags).parse(text, strictness);
		if strictness == Strictness::Strict
			&& doc.diags.iter().any(|d| d.severity == Severity::Error)
		{
			return Err(LoadError {
				diagnostics: doc.diags.clone(),
				document: doc,
			});
		}
		Ok(doc)
	}

	/// Everything the load recorded (after load_and_validate, validation
	/// findings too).
	pub fn diagnostics(&self) -> &[Diagnostic] {
		&self.diags
	}

	/// How many lines or values parsing dropped that canonical output cannot
	/// re-emit - bad indentation, an unusable selector, a line past the depth
	/// cap. Content-malformed lines do NOT count: those are retained as trivia
	/// and survive a save. Nonzero means a save_file would delete hand-written
	/// content, so save_file refuses then (save_file_lossy overrides).
	pub fn lost_count(&self) -> usize {
		self.lost
	}

	/// How many error-severity diagnostics the document carries - the "did
	/// this file have errors?" predicate, so recover-and-continue can't read
	/// as success by accident. Counts whatever diagnostics() holds (after
	/// load_and_validate, that includes validation errors).
	pub fn error_count(&self) -> usize {
		self.diags
			.iter()
			.filter(|d| d.severity == Severity::Error)
			.count()
	}

	/// One-shot load-and-validate: parse at a strictness, validate against a
	/// schema, and hand back the document carrying ONE combined diagnostics
	/// list (parse first, then validation - the order `check --schema`
	/// prints), so half the errors can't vanish because a caller forgot one
	/// of the two lists. Never fails: a strict-failing document comes back as
	/// the document plus its diagnostics (error_count() answers "did it
	/// fail"). An empty schema text skips validation entirely. H001 hints the
	/// schema disavows (a declared repeat upper bound above 1) are dropped.
	pub fn load_and_validate(text: &str, schema_text: &str, strictness: Strictness) -> Document {
		let mut doc = Parser::new().parse(text, strictness);
		if !schema_text.trim().is_empty() {
			let schema = Document::parse(schema_text);
			// A schema that did not load would silently drop the constraints on
			// its broken lines, or report every field as unknown - either way
			// blaming the document for the schema. Say so instead, as `check`
			// does, and validate nothing.
			if schema.diags.iter().any(|d| d.severity == Severity::Error) {
				doc.diags.push(Diagnostic {
					line: 0,
					severity: Severity::Error,
					code: "V099",
					message: "schema failed to load".to_string(),
				});
				return doc;
			}
			let vdiags = doc.validate(&schema);
			doc.diags.extend(vdiags);
			suppress_declared_repeats(&schema, &mut doc.diags);
			suppress_declared_reopens(&schema, &mut doc.diags);
		}
		doc
	}

	/// The level the document was loaded at.
	pub fn strictness(&self) -> Strictness {
		self.strictness
	}

	/// File tier, load half: read and parse PATH at Standard. Never fails -
	/// the document always comes back usable (empty when the file could not
	/// be read), and the status separates the four cases consumers otherwise
	/// confuse: absent, present-but-unreadable, parsed with errors, clean.
	pub fn load_file(path: &str) -> (Document, FileStatus) {
		Document::load_file_with(path, Strictness::Standard)
	}

	/// load_file at a chosen strictness. A strict-failing file reports
	/// HadErrors; the recover-and-continue document still comes back.
	pub fn load_file_with(path: &str, level: Strictness) -> (Document, FileStatus) {
		let text = match read_file(path, 0) {
			Ok(t) => t,
			Err(st) => return (Parser::new().parse("", level), st),
		};
		let doc = Parser::new().parse(&text, level);
		let st = if doc.diags.iter().any(|d| d.severity == Severity::Error) {
			FileStatus::HadErrors
		} else {
			FileStatus::Clean
		};
		(doc, st)
	}

	/// File tier, save half: write this document's canonical text to PATH
	/// through a temp file in the same directory plus a rename, so an
	/// interrupted save can never truncate the config it rewrites - the same
	/// mechanics the CLI's `--write` uses. Refuses when parsing lost content
	/// that a save would silently delete (see lost_count); save_file_lossy
	/// writes anyway. The Err tells the two apart without matching on prose:
	/// SaveError::Refused is the gate, SaveError::Io is the write failing.
	pub fn save_file(&self, path: &str) -> Result<(), SaveError> {
		if self.lost > 0 {
			return Err(SaveError::Refused {
				path: path.to_string(),
				lost: self.lost,
			});
		}
		write_file_atomic(path, &self.to_canonical()).map_err(SaveError::Io)
	}

	/// save_file without the lost-content gate: writes even when parsing
	/// dropped lines this save deletes. The caller owns that choice. Only ever
	/// Errs with SaveError::Io - the gate is the one thing it skips.
	pub fn save_file_lossy(&self, path: &str) -> Result<(), SaveError> {
		write_file_atomic(path, &self.to_canonical()).map_err(SaveError::Io)
	}

	/// Canonical form: block layout, tabs, insertion order, minimal quoting,
	/// redundancy collapsed, comments re-emitted as attached trivia. Scalar
	/// text is never rewritten.
	pub fn to_canonical(&self) -> String {
		let mut out = String::with_capacity(self.arena.len() * 24);
		self.emit_children(&self.arena[ROOT].children, 0, &mut out);
		// Comments that never found a following line re-emit at the end.
		for c in &self.orphans {
			if c.blank_before && !out.is_empty() {
				out.push('\n');
			}
			out.push_str(&c.text);
			out.push('\n');
		}
		out
	}

	/// Emit a sibling run. The parent walk already knows whether an earlier
	/// same-name sibling is empty (the raw same-line-fence hazard), so one
	/// seen-empties set here replaces a per-child rescan of the whole run.
	fn emit_children(&self, kids: &[usize], depth: usize, out: &mut String) {
		let mut empties: std::collections::HashSet<&str> = std::collections::HashSet::new();
		for &c in kids {
			let n = &self.arena[c];
			let wm = matches!(n.value, Value::Raw { .. }) && empties.contains(n.name.as_str());
			if n.value.is_empty() {
				empties.insert(n.name.as_str());
			}
			self.emit_node(c, depth, wm, out);
		}
	}

	fn emit_node(&self, idx: usize, depth: usize, would_merge: bool, out: &mut String) {
		let node = &self.arena[idx];
		let pad: String = "\t".repeat(depth);
		// Same-line fence spelling can't carry an inline comment (an unbalanced
		// quote in the info-string could hide the `#` on reparse), so its
		// trailing comment joins the leading lines instead; the flag comes from
		// the parent's walk. Each blank rides its own comment (or the binding
		// line), never as the first output line.
		for c in node.leading() {
			if c.blank_before && !out.is_empty() {
				out.push('\n');
			}
			out.push_str(&pad);
			out.push_str(&c.text);
			out.push('\n');
		}
		if node.blank_before && !out.is_empty() {
			out.push('\n');
		}
		if would_merge && !node.trailing().is_empty() {
			out.push_str(&pad);
			out.push_str(node.trailing());
			out.push('\n');
		}
		out.push_str(&pad);
		out.push_str(&emit_name(&node.name));
		out.push(':');
		match &node.value {
			Value::Empty => {
				push_trailing(out, node.trailing());
				out.push('\n');
			}
			Value::Cell(els) => {
				out.push(' ');
				for (i, e) in els.iter().enumerate() {
					if i > 0 {
						out.push_str(", ");
					}
					out.push_str(&emit_element(e));
				}
				push_trailing(out, node.trailing());
				out.push('\n');
			}
			Value::Raw(r) => {
				let (content, info, fence_char, fence_len) =
					(&r.content, &r.info, &r.fence_char, &r.fence_len);
				// Child-indent spelling is canonical: bare name line, fenced
				// block one level deeper, verbatim content. Exception: if an
				// earlier same-name sibling is empty, the bare `name:` header
				// would merge into it on reparse and the fence would fill that
				// instance instead - so use the same-line spelling there.
				if would_merge {
					out.push(' ');
				} else {
					push_trailing(out, node.trailing());
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
		self.emit_children(&self.arena[idx].children, depth + 1, out);
		// Comments this block owns with no child to carry them, one deeper.
		let ipad: String = "\t".repeat(depth + 1);
		for c in self.arena[idx].inside() {
			if c.blank_before && !out.is_empty() {
				out.push('\n');
			}
			out.push_str(&ipad);
			out.push_str(&c.text);
			out.push('\n');
		}
		// Comments that hung on this block after its last child.
		for c in self.arena[idx].after() {
			if c.blank_before && !out.is_empty() {
				out.push('\n');
			}
			out.push_str(&pad);
			out.push_str(&c.text);
			out.push('\n');
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

/// Emit a stored (escape-resolved) name in a spelling that reads back as the
/// same name: bare when it can be, else quoted with the escapes `apply_escapes`
/// undoes. This is a true inverse of the name parse, which `quote_text` is not -
/// that one picks a quote style to AVOID escaping and never escapes a
/// backslash, which is right for a value (stored in its escaped spelling) and
/// wrong for a name (stored resolved).
fn escape_name(name: &str) -> String {
	if !name.is_empty() && name.chars().all(is_bare_name_char) {
		return name.to_string();
	}
	let mut out = String::with_capacity(name.len() + 2);
	out.push('"');
	for c in name.chars() {
		match c {
			'\\' => out.push_str("\\\\"),
			'"' => out.push_str("\\\""),
			'\t' => out.push_str("\\t"),
			'\n' => out.push_str("\\n"),
			_ => out.push(c),
		}
	}
	out.push('"');
	out
}

fn emit_name(name: &str) -> String {
	escape_name(name)
}

/// Render a float the way the writer and the CLI do: shortest round-trip
/// decimal, never scientific notation, `inf`/`-inf`/`NaN` spelled out. Rust's
/// own Display already does exactly that, which is why this is a wrapper - the
/// other three bindings have to implement it, and a consumer building canonical
/// text by hand should not have to know which of the four they are reading.
#[must_use]
pub fn format_f64(v: f64) -> String {
	let s = format!("{v}");
	if !v.is_finite() || v == 0.0 {
		return s;
	}
	// The shortest spelling that reads back is the same in every binding
	// except on an exact tie between two spellings of that length, where core
	// rounds away from zero and Go, Python and C round to even. The correctly
	// rounded spelling of the same length is the to-even one, so use it when
	// it reads back; when it does not (a lopsided interval at a power of
	// two), the shortest one is the only choice and all four agree already.
	let sig = s
		.trim_start_matches('-')
		.replace('.', "")
		.trim_start_matches('0')
		.trim_end_matches('0')
		.len()
		.max(1);
	let e = format!("{v:.*e}", sig - 1);
	if e.parse::<f64>() != Ok(v) {
		return s;
	}
	let (mant, exp) = e.split_once('e').unwrap_or((&e, "0"));
	let digits: String = mant.chars().filter(char::is_ascii_digit).collect();
	let point = exp.parse::<i64>().unwrap_or(0) + 1;
	let mut out = String::new();
	if v.is_sign_negative() {
		out.push('-');
	}
	if point <= 0 {
		out.push_str("0.");
		out.extend(std::iter::repeat_n('0', (-point) as usize));
		out.push_str(&digits);
	} else if point as usize >= digits.len() {
		out.push_str(&digits);
		out.extend(std::iter::repeat_n('0', point as usize - digits.len()));
	} else {
		out.push_str(&digits[..point as usize]);
		out.push('.');
		out.push_str(&digits[point as usize..]);
	}
	out
}

/// Quote one path segment so it can be spliced into a lookup path: a bare name
/// passes through, anything else comes back quoted and escaped in the form the
/// path scanner accepts. Splicing user-typed text into a path without this is
/// path injection - a dotted name silently reads as nesting. Same spelling
/// `paths()` and the canonical emitter produce.
pub fn quote_segment(name: &str) -> String {
	emit_name(name)
}

/// The file tier's read half on its own: the text of PATH, or the status that
/// says why not - `NotFound`, or `Unreadable` for everything else (permissions,
/// a directory, bad encoding, or a file past MAX_BYTES; 0 is no cap).
/// `load_file` is this plus a parse. A consumer that needs the exact bytes it
/// last saw - to tell its own save coming back as a change notification from
/// somebody else's edit - or a bound on how much it will read before a parse,
/// calls this and parses the text itself.
pub fn read_file(path: &str, max_bytes: usize) -> Result<String, FileStatus> {
	use std::io::Read;
	let f = std::fs::File::open(path).map_err(|e| {
		if e.kind() == std::io::ErrorKind::NotFound {
			FileStatus::NotFound
		} else {
			FileStatus::Unreadable
		}
	})?;
	// One byte past the cap is read, so a file exactly at it passes and one
	// over is caught without trusting a length from metadata.
	let limit = if max_bytes == 0 {
		u64::MAX
	} else {
		(max_bytes as u64).saturating_add(1)
	};
	let mut bytes = Vec::new();
	f.take(limit)
		.read_to_end(&mut bytes)
		.map_err(|_| FileStatus::Unreadable)?;
	if max_bytes != 0 && bytes.len() > max_bytes {
		return Err(FileStatus::Unreadable);
	}
	String::from_utf8(bytes).map_err(|_| FileStatus::Unreadable)
}

/// The file tier's write mechanism (also what the CLI's `--write` uses): a
/// temp file in the same dir, then a rename over the target,
/// so an interrupted write can never truncate the config it rewrites. The data
/// is synced before the rename so a crash cannot publish an empty file.
///
/// A rename publishes a new inode, so the target is resolved through symlinks
/// first (otherwise a linked-in config gets replaced by a regular file and the
/// real one is left stale) and the original's mode is copied onto the temp file
/// (otherwise a 600 config comes back at whatever the umask allows). Other hard
/// links to the old inode cannot survive a rename and keep the old content.
pub fn write_file_atomic(file: &str, data: &str) -> Result<(), String> {
	use std::io::Write;
	if names_a_directory(file) {
		return Err(format!("{}: Is a directory", file));
	}
	let target = resolve_target(file).map_err(|e| format!("{}: {}", file, e))?;
	let dir = match target.parent() {
		Some(d) if !d.as_os_str().is_empty() => d,
		_ => std::path::Path::new("."),
	};
	let base = target
		.file_name()
		.map(|b| b.to_string_lossy().into_owned())
		.unwrap_or_else(|| file.to_string());
	// Exclusive create: the name is predictable, so anything already sitting
	// there - including a symlink someone else planted - must make this fail
	// rather than be written through. Retry past a stale collision, then give
	// up; refusing to write beats writing somewhere unintended.
	// A file that already exists keeps its own mode, so its temp is born private
	// and the real mode goes on below - the copy is never briefly readable to
	// anyone the original was not. A file that does not exist yet has no mode to
	// preserve, so it takes the one an ordinary create would: 0666 narrowed by
	// the umask, like every other file the user's tools produce.
	let existing = std::fs::metadata(&target).ok();
	// Windows: a read-only file cannot be replaced, and a read-only temp cannot
	// be removed after a failure, so the attribute comes off the target for the
	// publish and goes back on the new file after it - the same outcome as
	// POSIX, where the rename never needed the file writable. Hidden and system
	// ride back the same way: ReplaceFile's documented preserve list does not
	// include the basic attributes, and the rename fallback carries nothing, so
	// a hidden config came back visible.
	#[cfg(windows)]
	let read_only = existing
		.as_ref()
		.is_some_and(|m| m.permissions().readonly());
	#[cfg(windows)]
	let carried = {
		use std::os::windows::fs::MetadataExt;
		existing.as_ref().map_or(0, |m| {
			m.file_attributes() & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)
		})
	};
	let mut file_handle = None;
	let mut tmp = std::path::PathBuf::new();
	let mut last = String::new();
	for attempt in 0..8 {
		tmp = dir.join(format!(".{}.tmp{}.{}", base, std::process::id(), attempt));
		let mut opts = std::fs::OpenOptions::new();
		opts.write(true).create_new(true);
		#[cfg(unix)]
		{
			use std::os::unix::fs::OpenOptionsExt;
			opts.mode(if existing.is_some() { 0o600 } else { 0o666 });
		}
		match opts.open(&tmp) {
			Ok(f) => {
				file_handle = Some(f);
				break;
			}
			Err(e) => last = e.to_string(),
		}
	}
	let Some(mut f) = file_handle else {
		return Err(format!("{}: cannot create temporary file: {}", file, last));
	};
	let res = (|| -> std::io::Result<()> {
		f.write_all(data.as_bytes())?;
		f.sync_all()?;
		// On the handle, so umask cannot narrow it the way it narrows a create
		// mode, and after the data, because a write by anyone but root clears
		// setuid/setgid. Best effort: a filesystem that cannot carry the mode
		// is not a reason to fail a write that otherwise succeeded. The whole
		// mode goes, setuid/setgid/sticky included, as an editor's rewrite
		// would carry it.
		#[cfg(unix)]
		if let Some(m) = &existing {
			let _ = f.set_permissions(m.permissions());
		}
		Ok(())
	})();
	if let Err(e) = res {
		let _ = std::fs::remove_file(&tmp);
		return Err(format!("{}: {}", file, e));
	}
	drop(f);
	#[cfg(windows)]
	if read_only {
		set_read_only(&target, false);
	}
	let published = publish_file(&tmp, &target);
	#[cfg(windows)]
	if carried != 0 {
		set_attributes(&target, carried); // whether or not the publish went through
	}
	#[cfg(windows)]
	if read_only {
		set_read_only(&target, true); // whether or not the publish went through
	}
	published.map_err(|e| {
		let _ = std::fs::remove_file(&tmp);
		format!("{}: {}", file, e)
	})?;
	sync_dir(dir);
	Ok(())
}

/// The path a save actually rewrites. A symlink is followed so the write goes
/// through it; canonicalize does that but needs the target to exist, so a
/// dangling link is walked by hand and the file is created where it points.
/// A path that is no link at all is a plain create at the path as given.
/// A link cycle is an error: silently creating a regular file in its place
/// would be the exact replacement the symlink walk exists to avoid.
/// A path that names a directory rather than a file: it ends in a separator, or
/// its last component is `.` or `..`. POSIX refuses to open such a path as a
/// regular file, but a canonicalize drops the trailing separator first, so a
/// save through `f/` used to rewrite `f`.
fn names_a_directory(file: &str) -> bool {
	let sep = |c: char| c == '/' || (cfg!(windows) && c == '\\');
	if file.is_empty() {
		return false;
	}
	if file.ends_with(sep) {
		return true;
	}
	let last = file.rsplit(sep).next().unwrap_or("");
	last == "." || last == ".."
}

fn resolve_target(file: &str) -> Result<std::path::PathBuf, String> {
	if let Ok(p) = std::fs::canonicalize(file) {
		return Ok(p);
	}
	let mut p = std::path::PathBuf::from(file);
	for _ in 0..40 {
		let Ok(next) = std::fs::read_link(&p) else {
			break;
		};
		p = if next.is_absolute() {
			next
		} else {
			p.parent()
				.filter(|d| !d.as_os_str().is_empty())
				.unwrap_or(std::path::Path::new("."))
				.join(next)
		};
	}
	if std::fs::read_link(&p).is_ok() {
		return Err("too many levels of symbolic links".to_string());
	}
	Ok(match (p.parent(), p.file_name()) {
		(Some(d), Some(n)) if !d.as_os_str().is_empty() => {
			std::fs::canonicalize(d).map(|d| d.join(n)).unwrap_or(p)
		}
		_ => p,
	})
}

#[cfg(windows)]
const FILE_ATTRIBUTE_HIDDEN: u32 = 0x2;
#[cfg(windows)]
const FILE_ATTRIBUTE_SYSTEM: u32 = 0x4;

#[cfg(windows)]
fn set_read_only(path: &std::path::Path, on: bool) {
	if let Ok(m) = std::fs::metadata(path) {
		let mut perms = m.permissions();
		perms.set_readonly(on);
		let _ = std::fs::set_permissions(path, perms);
	}
}

/// Turn attribute bits back on after a publish. std has no setter for the basic
/// attributes, so this is a few lines of FFI beside the ReplaceFile ones.
#[cfg(windows)]
fn set_attributes(path: &std::path::Path, bits: u32) {
	use std::os::windows::ffi::OsStrExt;
	use std::os::windows::fs::MetadataExt;
	#[link(name = "kernel32")]
	unsafe extern "system" {
		fn SetFileAttributesW(name: *const u16, attrs: u32) -> i32;
	}
	let Ok(m) = std::fs::metadata(path) else {
		return;
	};
	let wide: Vec<u16> = path
		.as_os_str()
		.encode_wide()
		.chain(std::iter::once(0))
		.collect();
	unsafe {
		SetFileAttributesW(wide.as_ptr(), m.file_attributes() | bits);
	}
}

/// Move the finished temp file over the target. On windows that means
/// ReplaceFile rather than a rename: a rename publishes a brand-new file and
/// leaves the destination's ACLs, security attributes and named streams behind,
/// which ReplaceFile carries onto the replacement instead. What it does not
/// carry is the basic attributes - hidden and system - which the save re-applies
/// by hand. It needs the destination to exist, and it fails rather than skip a
/// merge it cannot do (no WRITE_DAC, say), so a create and any failure fall back
/// to the rename. WRITE_THROUGH is asked for and documented as unsupported by
/// ReplaceFile, so the durability here rests on the file's own fsync.
fn publish_file(tmp: &std::path::Path, target: &std::path::Path) -> std::io::Result<()> {
	#[cfg(windows)]
	if target.exists() && windows_replace_file(tmp, target) {
		return Ok(());
	}
	std::fs::rename(tmp, target)
}

#[cfg(windows)]
fn windows_replace_file(tmp: &std::path::Path, target: &std::path::Path) -> bool {
	use std::os::windows::ffi::OsStrExt;
	const REPLACEFILE_WRITE_THROUGH: u32 = 0x1;
	// Declared here rather than pulled from a crate: this file has no
	// dependencies and is not about to grow one for six lines of FFI.
	#[link(name = "kernel32")]
	unsafe extern "system" {
		fn ReplaceFileW(
			replaced: *const u16,
			replacement: *const u16,
			backup: *const u16,
			flags: u32,
			exclude: *mut core::ffi::c_void,
			reserved: *mut core::ffi::c_void,
		) -> i32;
	}
	fn wide(p: &std::path::Path) -> Vec<u16> {
		p.as_os_str()
			.encode_wide()
			.chain(std::iter::once(0))
			.collect()
	}
	let (replaced, replacement) = (wide(target), wide(tmp));
	unsafe {
		ReplaceFileW(
			replaced.as_ptr(),
			replacement.as_ptr(),
			std::ptr::null(),
			REPLACEFILE_WRITE_THROUGH,
			std::ptr::null_mut(),
			std::ptr::null_mut(),
		) != 0
	}
}

/// fsync the directory a save published into. The fsync on the file only
/// covered the file; the rename is a directory change, so without this a power
/// cut right after a save can lose the publish and leave the old content. Best
/// effort - windows has no directory fsync, and a filesystem that refuses one
/// is not a reason to fail a write that already succeeded.
fn sync_dir(dir: &std::path::Path) {
	#[cfg(unix)]
	if let Ok(d) = std::fs::File::open(dir) {
		let _ = d.sync_all();
	}
	#[cfg(not(unix))]
	let _ = dir;
}

/// The single H001 wording site: the hint builder and the schema suppressor
/// both come here, so the suppressor matches the exact head the builder
/// emitted - never a re-parse of free prose. (The leaf name cannot ride on
/// Diagnostic itself: consumers build Diagnostic literals, so its field set
/// is frozen.)
fn h001_head(name: &str) -> String {
	format!(
		"'{}' repeats as a bare leaf - did you mean '{}: ",
		name, name
	)
}

/// Drop the H001 hints a schema disavows: a field whose declared repeat upper
/// bound is above 1 repeats BY DESIGN (repetition is its instance mechanism),
/// so the repeated-bare-leaf hint is structurally a false positive there and
/// trains users to ignore hints. Matching is by leaf name - the filter
/// consumers were hand-rolling - which errs toward quiet, for a hint. Used by
/// `check --schema` and load_and_validate; call it wherever doc diagnostics
/// and a schema meet.
pub fn suppress_declared_repeats(schema: &Document, diags: &mut Vec<Diagnostic>) {
	let names = disavowed_names(schema, |c| c.repeat.is_some_and(|(_, hi)| hi > 1));
	if names.is_empty() {
		return;
	}
	let heads: Vec<String> = names.iter().map(|n| h001_head(n)).collect();
	diags.retain(|d| d.code != "H001" || !heads.iter().any(|h| d.message.starts_with(h.as_str())));
}

/// Leaf names of the schema entries `pick` accepts, top-level fields and every
/// fragment's fields alike. Read through the built schema, so the names are
/// the ones validation will use (escapes resolved) and an entry whose key
/// faulted disavows nothing.
fn disavowed_names(schema: &Document, pick: impl Fn(&Constraint) -> bool) -> Vec<String> {
	let (def, _) = build_schema(schema);
	let mut names: Vec<String> = Vec::new();
	let frags = def.frags.values().flat_map(|v| v.iter());
	for c in def.cons.iter().chain(frags) {
		if !pick(c) {
			continue;
		}
		// Name wildcard: no single leaf name to disavow.
		if let Some(seg) = c.segs.last().filter(|seg| !seg.star) {
			names.push(seg.name.clone());
		}
	}
	names
}

/// The single H002 wording site: the merge hint and the schema suppressor
/// both come here, same discipline as h001_head.
fn h002_head(name: &str) -> String {
	format!("merged with '{}' at ", name)
}

/// Drop the H002 hints a schema disavows: a section whose entry declares
/// `reopen: true` is MEANT to be written in parts, so the merge hint is
/// structurally a false positive there. Matching is by leaf name, same as the
/// H001 suppressor, and it errs toward quiet, for a hint. Used by
/// `check --schema` and load_and_validate; call it wherever doc diagnostics
/// and a schema meet.
pub fn suppress_declared_reopens(schema: &Document, diags: &mut Vec<Diagnostic>) {
	let names = disavowed_names(schema, |c| c.reopen);
	if names.is_empty() {
		return;
	}
	let heads: Vec<String> = names.iter().map(|n| h002_head(n)).collect();
	diags.retain(|d| d.code != "H002" || !heads.iter().any(|h| d.message.starts_with(h.as_str())));
}

/// Minimal quoting: bare unless a reserved character (or lookalike hazard) forces it.
/// One addition: an author-quoted element keeps its quotes unless the text reads as
/// one of SHCL's own data formats - quoting those is just spelling (readers type the
/// value either way), but quoting a plain string is the escape and must survive
/// canonicalization. This clause only ever adds quoting, so a bare emit stays safe.
fn emit_element(e: &Element) -> String {
	let t = &e.text;
	// Edge whitespace beyond the space/tab above still has to force quotes: the
	// parser trims the full White_Space set, so a bare NBSP (or VT, FF, NEL,
	// ideographic space) at either end would not survive the reload. Edges only
	// - interior whitespace is never trimmed and quoting it would move bytes.
	let needs = t.is_empty()
		|| t.chars()
			.any(|c| matches!(c, ' ' | '\t' | ',' | ':' | '#' | '"' | '\'' | '[' | ']'))
		|| t.starts_with(char::is_whitespace)
		|| t.ends_with(char::is_whitespace)
		|| fence_open(t).is_some()
		|| (e.quoted && !is_data_format(e));
	if needs { quote_text(t) } else { t.clone() }
}

/// True when the text reads as an int, float, bool, or datetime at standard
/// strictness - fixed there deliberately, so canonical form cannot vary with
/// the load strictness.
fn is_data_format(e: &Element) -> bool {
	// One pass over the bytes before any coercion. At Standard the int, float
	// and datetime forms all require at least one ASCII digit; the only formats
	// that do not are the boolean words, and the longest of those is "false".
	// An ordinary quoted string fails both tests, so emit stops running four
	// full coercions on every quoted element it writes.
	let t = e.text.trim();
	if t.bytes().any(|b| b.is_ascii_digit()) {
		return parse_int_text(e, Strictness::Standard).is_some()
			|| parse_float_text(e, Strictness::Standard).is_some()
			|| parse_datetime(&e.text).is_some()
			|| parse_bool_text(t, Strictness::Standard).is_some();
	}
	t.len() <= 5 && parse_bool_text(t, Strictness::Standard).is_some()
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
	// A dangling trailing backslash would turn the closing quote into an
	// escape pair - the scanner reads the path back wrong, or not at all.
	// Store the doubled spelling (identical on string read), the same rule
	// the element parser applies to bare text.
	let normalized;
	let t = if t.ends_with('\\') {
		normalized = normalize_dangling_backslash(t.to_string());
		normalized.as_str()
	} else {
		t
	};
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
	fn name_index(&self) -> &NameIndex {
		self.index.get_or_init(|| {
			// Boxed so the document stays small on the stack (it rides inside
			// LoadError by value).
			let mut idx = NameIndex {
				first: HashMap::new(),
				last: HashMap::new(),
				next_same: vec![NIL; self.arena.len()],
			};
			// From the root, not across the arena: a removed subtree's nodes
			// are still there with their child lists intact, so an arena walk
			// indexes every node the document ever held. Chains stay in file
			// order - a chain is one parent's same-named children, and each
			// parent's are appended in its own list order.
			let mut stack = vec![ROOT];
			while let Some(p) = stack.pop() {
				for &c in &self.arena[p].children {
					idx.append(name_key(p, &self.arena[c].name), c);
					stack.push(c);
				}
			}
			Box::new(idx)
		})
	}

	fn children_named(&self, parent: usize, name: &str) -> Vec<usize> {
		let idx = self.name_index();
		let mut out = Vec::new();
		let mut c = idx
			.first
			.get(&name_key(parent, name))
			.copied()
			.unwrap_or(NIL);
		while c != NIL {
			if self.arena[c].name == name && self.arena[c].parent == parent {
				out.push(c);
			}
			c = idx.next_same[c];
		}
		out
	}

	fn resolve_from(&self, start: &[usize], segs: &[Segment]) -> Resolved {
		let mut cur: Vec<usize> = start.to_vec();
		for (i, seg) in segs.iter().enumerate() {
			let mut next: Vec<usize> = Vec::new();
			for &n in &cur {
				if seg.star {
					next.extend(self.arena[n].children.iter().copied());
				} else {
					next.extend(self.children_named(n, &seg.name));
				}
			}
			if seg.star {
				// Name wildcard: same per-slot split as `[*]`, over every child.
				let rest = &segs[i + 1..];
				let mut slots: Vec<Result<usize, Status>> = Vec::new();
				for inst in next {
					if rest.is_empty() {
						slots.push(Ok(inst));
					} else {
						match self.resolve_from(&[inst], rest) {
							Resolved::One(x) => slots.push(Ok(x)),
							Resolved::None => slots.push(Err(Status::NotFound)),
							// A wildcard after a wildcard: the inner slots join the
							// outer list, so the two compose into one flat run of
							// leaves rather than one unreadable slot.
							Resolved::Slots(inner) => slots.extend(inner),
							_ => slots.push(Err(Status::Multiple)),
						}
					}
				}
				return Resolved::Slots(slots);
			}
			match &seg.selector {
				None => cur = next,
				Some(Selector::ByValue { text, quoted }) => {
					let want = apply_escapes(text);
					cur = next
						.into_iter()
						.filter(|&c| {
							disp_key(&self.arena[c].value) == want
								&& (!quoted || single_scalar(&self.arena[c].value))
						})
						.collect();
				}
				Some(Selector::ByIndex(k)) => {
					cur = index_usize(*k)
						.and_then(|i| next.get(i))
						.map(|&c| vec![c])
						.unwrap_or_default();
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
								// A wildcard after a wildcard: the inner slots join the
								// outer list, so the two compose into one flat run of
								// leaves rather than one unreadable slot.
								Resolved::Slots(inner) => slots.extend(inner),
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
		let scan = scan_lookup(path).map_err(|_| Status::NotFound)?;
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

	/// Every field path in the document, in file order, deduplicated - a query
	/// recipe for tooling (the differential harness derives reads over the fuzz
	/// set from it). A segment that is not bare-name-safe is emitted quoted and
	/// escaped - the form the path scanner accepts - so each path is a
	/// well-formed lookup path and nothing in the document is hidden.
	pub fn paths(&self) -> Vec<String> {
		let mut out = Vec::new();
		let mut seen = std::collections::HashSet::new();
		let mut stack: Vec<(usize, String)> = self.arena[ROOT]
			.children
			.iter()
			.rev()
			.map(|&c| (c, String::new()))
			.collect();
		while let Some((node, prefix)) = stack.pop() {
			let seg = emit_name(&self.arena[node].name);
			let path = if prefix.is_empty() {
				seg
			} else {
				format!("{}.{}", prefix, seg)
			};
			if seen.insert(path.clone()) {
				out.push(path.clone());
			}
			for &c in self.arena[node].children.iter().rev() {
				stack.push((c, path.clone()));
			}
		}
		out
	}

	/// 1-based source line of the binding at a path, for consumer checks the
	/// schema cannot express. 0 when the path does not resolve to exactly one
	/// node, or the node was writer-built. Merged instances cite the first
	/// binding's line, matching diagnostics.
	pub fn line(&self, path: &str) -> usize {
		match self.resolve(path) {
			Ok(Resolved::One(n)) => self.arena[n].line,
			_ => 0,
		}
	}

	/// The field name at a path exactly as the author spelled it (case
	/// unfolded, outer quotes stripped), so a message can echo `SYMBOLS` when
	/// the file said SYMBOLS. Escape sequences stay as written too: a name is
	/// stored, compared and emitted with its escapes RESOLVED, so this is the
	/// one call that hands the source spelling back - which is what an
	/// as-authored accessor is for. Resolution mirrors line(): empty when the path
	/// does not resolve to exactly one node. Merged instances keep the first
	/// binding's spelling; a writer-built node keeps the spelling the setter's
	/// path used.
	pub fn authored_name(&self, path: &str) -> String {
		match self.resolve(path) {
			Ok(Resolved::One(n)) => self.arena[n].authored().to_string(),
			_ => String::new(),
		}
	}

	/// The plural line(): 1-based source lines at a path, in file order, so a
	/// repeated field - the case that most wants a citable line - yields every
	/// binding's. Wildcard slots that did not resolve stay in the list as 0,
	/// and a writer-built node is 0, so indices keep matching count().
	pub fn lines(&self, path: &str) -> Vec<usize> {
		match self.resolve(path) {
			Ok(Resolved::One(n)) => vec![self.arena[n].line],
			Ok(Resolved::Many(v)) => v.iter().map(|&n| self.arena[n].line).collect(),
			Ok(Resolved::Slots(s)) => s
				.into_iter()
				.map(|r| match r {
					Ok(n) => self.arena[n].line,
					Err(_) => 0,
				})
				.collect(),
			_ => Vec::new(),
		}
	}

	/// Child field names under a path, in file order, duplicates included -
	/// the "what keys are in this section?" question paths() (deduplicated,
	/// path-shaped) cannot answer. "" enumerates the top level. Names come
	/// back as stored; quote_segment() makes one splice-safe in a path.
	pub fn children(&self, path: &str) -> Vec<String> {
		let node = if path.trim().is_empty() {
			ROOT
		} else {
			match self.resolve(path) {
				Ok(Resolved::One(n)) => n,
				_ => return Vec::new(),
			}
		};
		self.arena[node]
			.children
			.iter()
			.map(|&c| self.arena[c].name.clone())
			.collect()
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

/// Read text as the value half of a line, for the setters that take value
/// syntax rather than data. Rejects what could not have come off one line: a
/// line break, or a quote that never closes. An unquoted `#` ends the value
/// here exactly as it would in a file. Bracket-array text is refused too: in
/// a file it is E019 and the line is lost, so writing it as a two-element
/// array holding `[1` and `2]` would be a different wrong answer.
fn literal_value(text: &str) -> Option<Value> {
	if text.contains('\n') || text.contains('\r') {
		return None;
	}
	let (v, _) = split_value_comment(text);
	let v = trim_wsp(v);
	if unterminated_quote(v) || (v.starts_with('[') && v.ends_with(']')) {
		return None;
	}
	Some(parse_cell(v))
}

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
	// The emitter escapes a bare double quote when both quote kinds appear, so
	// a reparse of the written line stores the escaped spelling. Store it here
	// too, or `instances` and a read's raw text differ between a written
	// document and its own reload - the one place `set(x)` and
	// `load(emit(set(x)))` disagreed.
	let (dq, sq) = bare_quote_counts(&out);
	if dq > 0 && sq > 0 {
		let mut esc = String::with_capacity(out.len() + dq);
		let mut it = out.chars();
		while let Some(c) = it.next() {
			match c {
				'\\' => {
					esc.push(c);
					if let Some(n) = it.next() {
						esc.push(n);
					}
				}
				'"' => esc.push_str("\\\""),
				_ => esc.push(c),
			}
		}
		return esc;
	}
	out
}

/// Pick a backtick fence long enough that no content line closes it early.
fn choose_fence(content: &str) -> (u8, usize) {
	let mut maxrun = 0usize;
	for line in content.split('\n') {
		let t = trim_wsp(line);
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

	fn new_child(&mut self, parent: usize, name: &str, name_src: &str, value: Value) -> usize {
		let idx = self.arena.len();
		self.arena.push(NodeData {
			name: name.to_string(),
			name_src: spelled(name, name_src),
			value,
			children: Vec::new(),
			parent,
			line: 0,
			star_list: false,
			star_mixed: false,
			trivia: None,
			// Hand-written files separate top-level sections with a blank line;
			// writer-built ones do the same (the emitter never blanks line 1).
			blank_before: parent == ROOT,
			src_set: false,
			src: None,
		});
		self.arena[parent].children.push(idx);
		if let Some(ix) = self.index.get_mut() {
			ix.append(name_key(parent, name), idx);
		}
		idx
	}

	/// Why a write at this path would fail - the reason behind a setter's bare
	/// `false`, so a consumer's error message need not guess. `Writable` means
	/// the same validation `place()` runs would pass; nothing is created.
	pub fn write_reason(&self, path: &str) -> WriteReason {
		let Ok(scan) = scan_lookup(path) else {
			return WriteReason::BadPath;
		};
		self.probe_write(&scan, &mut Vec::new())
	}

	/// The validation walk `write_reason` and `place` share. `trail` collects
	/// where each segment landed - `None` from the point the path falls off the
	/// existing tree - so `place` can create from exactly there instead of
	/// scanning the path and walking the tree a second time.
	fn probe_write(&self, scan: &PathScan, trail: &mut Vec<Option<usize>>) -> WriteReason {
		trail.clear();
		if scan.value_text.is_some() {
			return WriteReason::ValueInPath;
		}
		if scan.segments.is_empty() {
			return WriteReason::BadPath;
		}
		// Writer side of the load-time nesting cap: never create deeper.
		if scan.segments.len() > MAX_DEPTH {
			return WriteReason::TooDeep;
		}
		// The probe walk place() validates with: once it falls off the existing
		// tree, a later `[#k]` can never match (fresh intermediates are created
		// childless), so an index segment past that point is unresolvable.
		let mut probe = Some(ROOT);
		for seg in &scan.segments {
			if seg.star {
				return WriteReason::Wildcard;
			}
			// A newline in a SELECTOR has no one-line spelling, so the emitted
			// binding would split across two lines and reparse as neither. The
			// selector stores its path text raw and the value emitter never
			// escapes a line break, so nothing downstream can rescue it - and
			// the reload loses nothing it can count, so the save gate would not
			// catch it either. A newline in a NAME is fine: names are stored
			// escape-resolved and emitted through the name escaper, which spells
			// a line break `\n` and reads it back as one.
			if matches!(&seg.selector, Some(Selector::ByValue { text, .. }) if text.contains('\n'))
			{
				return WriteReason::BadPath;
			}
			match &seg.selector {
				Some(Selector::Wildcard) => return WriteReason::Wildcard,
				Some(Selector::ByIndex(k)) => {
					let Some(c) = probe else {
						return WriteReason::NoSuchIndex;
					};
					let matches = self.children_named(c, &seg.name);
					match index_usize(*k).and_then(|i| matches.get(i)) {
						Some(&m) => probe = Some(m),
						None => return WriteReason::NoSuchIndex,
					}
				}
				Some(Selector::ByValue { text, quoted }) => {
					let want = apply_escapes(text);
					probe = probe.and_then(|c| {
						self.children_named(c, &seg.name).into_iter().find(|&n| {
							disp_key(&self.arena[n].value) == want
								&& (!quoted || single_scalar(&self.arena[n].value))
						})
					});
				}
				None => {
					probe = probe.and_then(|c| self.children_named(c, &seg.name).first().copied());
				}
			}
			trail.push(probe);
		}
		WriteReason::Writable
	}

	/// Walk (creating as needed) to the node a write targets. A trailing name
	/// with no selector hits the first same-named instance (or a new one); a
	/// `[value]` selector selects the matching instance or creates it; `[#k]`
	/// must already exist. None = path unusable for a write (write_reason()
	/// says why). Validation runs first, so a doomed path leaves no
	/// half-created intermediates behind.
	fn place(&mut self, path: &str) -> Option<usize> {
		let scan = scan_lookup(path).ok()?;
		let mut trail: Vec<Option<usize>> = Vec::new();
		if self.probe_write(&scan, &mut trail) != WriteReason::Writable {
			return None;
		}
		let mut cur = ROOT;
		for (i, seg) in scan.segments.iter().enumerate() {
			// The probe already resolved every segment that exists; only the
			// tail it fell off has anything to create.
			if let Some(found) = trail[i] {
				cur = found;
				continue;
			}
			cur = match &seg.selector {
				None => self.new_child(cur, &seg.name, &seg.name_src, Value::Empty),
				Some(Selector::ByValue { text, .. }) => {
					self.new_child(cur, &seg.name, &seg.name_src, cell_of(text.clone()))
				}
				// Both are unreachable: probe_write refuses a wildcard outright
				// and an unresolvable index, so neither reaches an empty trail
				// slot. Belt only.
				Some(Selector::ByIndex(_)) | Some(Selector::Wildcard) => return None,
			};
		}
		Some(cur)
	}

	fn set_value(&mut self, path: &str, value: Value) -> bool {
		match self.place(path) {
			Some(node) => {
				self.arena[node].value = value;
				self.arena[node].src = None; // written value has no source spelling
				self.collapse_dup(node);
				true
			}
			None => false,
		}
	}

	/// A written value may now collide with a same-named sibling under the
	/// in-file merge rule; fold the pair the way a reparse would (earlier
	/// sibling survives, later one folds children and trivia in) so Writer
	/// output stays a formatter fixpoint.
	fn collapse_dup(&mut self, node: usize) {
		let parent = self.arena[node].parent;
		let me = &self.arena[node];
		let Some(other) = self
			.children_named(parent, &me.name)
			.into_iter()
			.find(|&c| {
				let o = &self.arena[c];
				c != node && merge_eq(&o.name, &o.value, &me.name, &me.value)
			})
		else {
			return;
		};
		let pos = |n: usize| {
			self.arena[parent]
				.children
				.iter()
				.position(|&c| c == n)
				.unwrap_or(usize::MAX)
		};
		let (survivor, loser) = if pos(other) < pos(node) {
			(other, node)
		} else {
			(node, other)
		};
		let moved: Vec<usize> = self.arena[loser].children.clone();
		fold_node_into(&mut self.arena, survivor, loser);
		self.arena[parent].children.retain(|&c| c != loser);
		if let Some(ix) = self.index.get_mut() {
			ix.unlink(name_key(parent, &self.arena[loser].name), loser);
			for &k in &moved {
				let name = &self.arena[k].name;
				ix.unlink(name_key(loser, name), k);
				ix.append(name_key(survivor, name), k);
			}
		}
		self.fold_dups_below(survivor);
	}

	/// Folding moves the loser's children up a level, where they can collide
	/// with the survivor's own. The parser's fold is depth-first for the same
	/// reason; only a node that just received children can hold a new pair.
	fn fold_dups_below(&mut self, start: usize) {
		let mut stack = vec![start];
		while let Some(parent) = stack.pop() {
			let kids = std::mem::take(&mut self.arena[parent].children);
			let mut first: HashMap<u64, Slot> = HashMap::new();
			let mut keep: Vec<usize> = Vec::with_capacity(kids.len());
			for c in kids {
				let h = merge_hash(&self.arena[c].name, &self.arena[c].value);
				let survivor = first.get(&h).and_then(|s| {
					s.first_match(|x| {
						merge_eq(
							&self.arena[x].name,
							&self.arena[x].value,
							&self.arena[c].name,
							&self.arena[c].value,
						)
					})
				});
				match survivor {
					Some(s) => {
						let moved: Vec<usize> = self.arena[c].children.clone();
						fold_node_into(&mut self.arena, s, c);
						if let Some(ix) = self.index.get_mut() {
							ix.unlink(name_key(parent, &self.arena[c].name), c);
							for &k in &moved {
								let name = &self.arena[k].name;
								ix.unlink(name_key(c, name), k);
								ix.append(name_key(s, name), k);
							}
						}
						stack.push(s);
					}
					None => {
						first
							.entry(h)
							.and_modify(|s| s.push(c))
							.or_insert(Slot::One(c));
						keep.push(c);
					}
				}
			}
			self.arena[parent].children = keep;
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
			if let Some(ix) = self.index.get_mut() {
				ix.unlink(name_key(p, &self.arena[t].name), t);
			}
		}
		targets.len()
	}

	/// Attach a leading comment line to the node at a path (creating an empty
	/// node if it does not exist yet, so a section can be annotated). A missing
	/// `#` is added; only the first line is kept (a comment is one line), and
	/// trailing whitespace comes off the way the load takes it, so text that is
	/// blank leaves a bare `#`.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_comment(&mut self, path: &str, text: &str) -> bool {
		match self.place(path) {
			Some(node) => {
				let line = text.split('\n').next().unwrap_or("");
				let c = if line.starts_with('#') {
					line.to_string()
				} else {
					format!("# {}", line)
				};
				// Without this the load trims what was written and the writer's
				// output stops being a fmt fixpoint.
				let c = trim_wsp_end(&c).to_string();
				// The node's own blank moves above its first comment; otherwise
				// the blank would separate the comment from what it annotates.
				// Above the first one already there, when there is one.
				let nd = &mut self.arena[node];
				let mut lead = Lead::plain(c);
				if nd.blank_before {
					nd.blank_before = false;
					match nd.triv_mut().leading.first_mut() {
						Some(first) => first.blank_before = true,
						None => lead.blank_before = true,
					}
				}
				nd.triv_mut().leading.push(lead);
				true
			}
			None => false,
		}
	}

	/// Bind an integer at a path, creating the path as needed; false = path not
	/// writable (write_reason says why - same for every setter). The setters are
	/// must_use because an ignored false means the save that follows writes a
	/// document missing the edit, and reports success doing it.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_int(&mut self, path: &str, v: i64) -> bool {
		self.set_value(path, cell_of(v.to_string()))
	}
	/// Bind a float at a path, in the canonical shortest spelling. An
	/// infinity or a NaN has no spelling the reader accepts, so it fails the
	/// write rather than binding a value that cannot read back.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_float(&mut self, path: &str, v: f64) -> bool {
		if !v.is_finite() {
			return false;
		}
		self.set_value(path, cell_of(format_f64(v)))
	}
	/// Bind true/false at a path.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_bool(&mut self, path: &str, v: bool) -> bool {
		self.set_value(path, cell_of(if v { "true" } else { "false" }.to_string()))
	}
	/// Bind a string at a path, escaped so it reads back exactly.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_string(&mut self, path: &str, v: &str) -> bool {
		self.set_value(path, cell_of(encode_string(v)))
	}
	/// Bind a datetime at a path, in its canonical spelling. The struct's
	/// fields are public and carry no invariant, so a value the reader would
	/// refuse (month 13, a fraction with no seconds, an empty struct) fails
	/// the write rather than binding text that cannot read back.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_datetime(&mut self, path: &str, v: &ShclDateTime) -> bool {
		if !datetime_reads_back(v) {
			return false;
		}
		self.set_value(path, cell_of(v.to_string()))
	}
	/// Bind a raw block at a path, picking a fence longer than any content line.
	/// The info-string is stored as a fence line would read it back (trimmed);
	/// one holding a line break or an unquoted `#` has no fence-line spelling
	/// (the `#` would read back as a comment) and fails the write. A body line
	/// ending in CR fails for the same reason: the load takes the whole
	/// trailing CR run off every line, so it would not read back.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_raw(&mut self, path: &str, content: &str, info: &str) -> bool {
		if info.contains('\n') || info.contains('\r') || split_comment(info).1.is_some() {
			return false;
		}
		if content.split('\n').any(|line| line.ends_with('\r')) {
			return false;
		}
		let info = info.trim();
		let (fence_char, fence_len) = choose_fence(content);
		self.set_value(
			path,
			Value::Raw(Box::new(RawVal {
				content: content.to_string(),
				info: info.to_string(),
				fence_char,
				fence_len,
			})),
		)
	}
	/// Bind an empty value at a path (distinct from the empty string).
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_empty(&mut self, path: &str) -> bool {
		self.set_value(path, Value::Empty)
	}

	/// Bind an inline integer array at a path.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_int_array(&mut self, path: &str, v: &[i64]) -> bool {
		self.set_value(path, array_cell(v.iter().map(|x| x.to_string()).collect()))
	}
	/// Bind an inline float array at a path.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_float_array(&mut self, path: &str, v: &[f64]) -> bool {
		if !v.iter().all(|x| x.is_finite()) {
			return false;
		}
		self.set_value(path, array_cell(v.iter().map(|x| format_f64(*x)).collect()))
	}
	/// Bind an inline bool array at a path.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_bool_array(&mut self, path: &str, v: &[bool]) -> bool {
		self.set_value(
			path,
			array_cell(
				v.iter()
					.map(|x| if *x { "true" } else { "false" }.to_string())
					.collect(),
			),
		)
	}
	/// Bind an inline string array at a path, per-element escaped.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_string_array(&mut self, path: &str, v: &[&str]) -> bool {
		self.set_value(
			path,
			array_cell(v.iter().map(|x| encode_string(x)).collect()),
		)
	}
	/// Bind an inline datetime array at a path.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_datetime_array(&mut self, path: &str, v: &[ShclDateTime]) -> bool {
		if !v.iter().all(datetime_reads_back) {
			return false;
		}
		self.set_value(path, array_cell(v.iter().map(|x| x.to_string()).collect()))
	}

	// Default (only-if-absent) forms - the "emit defaults" half of the Writer.
	// A path that already resolves reports what a write there would, so a
	// wildcard is refused whether or not its slots happen to resolve.
	/// `set_int` only when the path has no node yet.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_int_default(&mut self, path: &str, v: i64) -> bool {
		if !self.exists(path) {
			return self.set_int(path, v);
		}
		self.write_reason(path) == WriteReason::Writable
	}
	/// `set_float` only when the path has no node yet.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_float_default(&mut self, path: &str, v: f64) -> bool {
		if !self.exists(path) {
			return self.set_float(path, v);
		}
		self.write_reason(path) == WriteReason::Writable
	}
	/// `set_bool` only when the path has no node yet.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_bool_default(&mut self, path: &str, v: bool) -> bool {
		if !self.exists(path) {
			return self.set_bool(path, v);
		}
		self.write_reason(path) == WriteReason::Writable
	}
	/// `set_string` only when the path has no node yet.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_string_default(&mut self, path: &str, v: &str) -> bool {
		if !self.exists(path) {
			return self.set_string(path, v);
		}
		self.write_reason(path) == WriteReason::Writable
	}
	/// Write TEXT as value syntax rather than as data: `80, 443` becomes a
	/// two-element array where `set_string` would store one string that has to
	/// be quoted. This is how a caller holding value text - a config line, a
	/// user's `--set` argument - writes it without knowing its shape first.
	/// Fails on text that could not be one line's value (see `literal_value`).
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_literal(&mut self, path: &str, text: &str) -> bool {
		match literal_value(text) {
			Some(v) => self.set_value(path, v),
			None => false,
		}
	}
	/// `set_literal` only when the path has no node yet.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_literal_default(&mut self, path: &str, text: &str) -> bool {
		if !self.exists(path) {
			return self.set_literal(path, text);
		}
		self.write_reason(path) == WriteReason::Writable
	}
	/// `set_datetime` only when the path has no node yet.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_datetime_default(&mut self, path: &str, v: &ShclDateTime) -> bool {
		if !self.exists(path) {
			return self.set_datetime(path, v);
		}
		self.write_reason(path) == WriteReason::Writable
	}
	/// `set_raw` only when the path has no node yet.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_raw_default(&mut self, path: &str, content: &str, info: &str) -> bool {
		if !self.exists(path) {
			return self.set_raw(path, content, info);
		}
		self.write_reason(path) == WriteReason::Writable
	}
	/// `set_int_array` only when the path has no node yet.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_int_array_default(&mut self, path: &str, v: &[i64]) -> bool {
		if !self.exists(path) {
			return self.set_int_array(path, v);
		}
		self.write_reason(path) == WriteReason::Writable
	}
	/// `set_float_array` only when the path has no node yet.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_float_array_default(&mut self, path: &str, v: &[f64]) -> bool {
		if !self.exists(path) {
			return self.set_float_array(path, v);
		}
		self.write_reason(path) == WriteReason::Writable
	}
	/// `set_bool_array` only when the path has no node yet.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_bool_array_default(&mut self, path: &str, v: &[bool]) -> bool {
		if !self.exists(path) {
			return self.set_bool_array(path, v);
		}
		self.write_reason(path) == WriteReason::Writable
	}
	/// `set_string_array` only when the path has no node yet.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_string_array_default(&mut self, path: &str, v: &[&str]) -> bool {
		if !self.exists(path) {
			return self.set_string_array(path, v);
		}
		self.write_reason(path) == WriteReason::Writable
	}
	/// `set_datetime_array` only when the path has no node yet.
	#[must_use = "a setter reports whether the write applied; an unusable path writes nothing (see write_reason)"]
	pub fn set_datetime_array_default(&mut self, path: &str, v: &[ShclDateTime]) -> bool {
		if !self.exists(path) {
			return self.set_datetime_array(path, v);
		}
		self.write_reason(path) == WriteReason::Writable
	}
}

// ---------------------------------------------------------------------------
// Layered loading: overlay a higher-priority document onto a lower one.
// ---------------------------------------------------------------------------

impl Document {
	/// Overlay `over` (a higher-priority layer) onto self (the lower one).
	/// Container instances merge by `(name, value)` exactly like the in-file
	/// rule; a leaf name present in `over` *replaces* self's same-named children
	/// at that scope - provided those base children are leaves too - so scalars,
	/// arrays, and raw blocks get real override while a bare section header
	/// merges instead of wiping. over-only nodes are appended. Comment trivia
	/// rides with each node.
	/// `Load(defaults, site, user)` is a left fold of this: each later file
	/// overlaid on the accumulation of the earlier ones.
	/// The fold is not associative: `(A+B)+C` and `A+(B+C)` differ where a bare
	/// header meets an overridden leaf, so a cached upper pair is not the same
	/// document the CLI's left fold produces. self keeps its own strictness,
	/// so a value from a stricter layer reads with self's coercion. And a
	/// replaced node is kept until the document is dropped: this costs a pass
	/// over the touched scopes plus an index rebuild on the next read.
	pub fn merge(&mut self, over: &Document) {
		self.index.take();
		self.lost += over.lost;
		self.overlay(ROOT, over, ROOT);
		// Layers commonly share a footer; keeping one copy of each keeps a
		// stack of files from repeating it once per layer. Only the lines
		// already here count: a layer's own repeats are its content.
		let had = self.orphans.len();
		for o in &over.orphans {
			if !self.orphans[..had].iter().any(|e| e.text == o.text) {
				self.orphans.push(o.clone());
			}
		}
	}

	// One grouping pass over each side, then a single children rebuild: the
	// old shape re-filtered the over side per distinct name and re-scanned
	// (and re-keyed) the base side per over node - three O(K^2) terms at one
	// parent, plus a full vector rebuild per replaced name.
	/// A matched instance keeps the base node, so the over side's comments have
	/// to move onto it or they are lost. Same rule as an in-file merge: leading
	/// concatenates in layer order, first trailing wins.
	fn adopt_trivia(&mut self, base: usize, over: &Document, ok: usize) {
		let Some(st) = over.arena[ok].trivia.as_deref() else {
			return;
		};
		let bt = self.arena[base].triv_mut();
		bt.leading.extend_from_slice(&st.leading);
		if !st.trailing.is_empty() {
			if bt.trailing.is_empty() {
				bt.trailing = st.trailing.clone();
			} else {
				bt.leading.push(Lead::plain(st.trailing.clone()));
			}
		}
		bt.after.extend_from_slice(&st.after);
		bt.inside.extend_from_slice(&st.inside);
	}

	fn overlay(&mut self, base_parent: usize, over: &Document, over_parent: usize) {
		let over_kids = &over.arena[over_parent].children;
		// Over side: name -> node bucket, in first-appearance order.
		let mut order: Vec<String> = Vec::new();
		let mut groups: HashMap<String, Vec<(usize, usize)>> = HashMap::new();
		for (pos, &k) in over_kids.iter().enumerate() {
			let n = &over.arena[k].name;
			groups
				.entry(n.clone())
				.or_insert_with(|| {
					order.push(n.clone());
					Vec::new()
				})
				.push((pos, k));
		}
		// Base side, one pass: does the name have a container instance, and
		// which child carries each (name, key) - every key computed once. The
		// list is cloned because the splices below rewrite it as they go.
		let base_kids = self.arena[base_parent].children.clone();
		let mut has_container: HashMap<String, bool> = HashMap::new();
		let mut by_key: HashMap<(String, String), usize> = HashMap::new();
		for &b in &base_kids {
			let name = self.arena[b].name.clone();
			let e = has_container.entry(name.clone()).or_insert(false);
			*e = *e || !self.arena[b].children.is_empty();
			by_key.entry((name, self.arena[b].value.key())).or_insert(b);
		}
		// Decide per name. A name whose over-side nodes are all leaves is an
		// override - but only when the base side of the group is leaf-shaped
		// too. Against a base container, a childless over-node is a wrapper
		// mention, not a leaf, so it falls through to the instance merge: a
		// bare section header in a higher layer never wipes the subtree below.
		// Replaced groups splice in the rebuild; everything appended (unmatched
		// instances, and replaced names base never had) keeps the over file's
		// order, which the per-name pass here would otherwise regroup.
		let mut replace: HashMap<String, Vec<usize>> = HashMap::new();
		let mut appended: Vec<(usize, usize)> = Vec::new();
		let empty_key = Value::Empty.key();
		for name in &order {
			let group = &groups[name];
			let over_leafy = group
				.iter()
				.all(|&(_, k)| over.arena[k].children.is_empty());
			let in_base = has_container.contains_key(name);
			let base_container = has_container.get(name).copied().unwrap_or(false);
			if over_leafy && !base_container {
				let clones: Vec<(usize, usize)> = group
					.iter()
					.map(|&(pos, ok)| (pos, self.clone_subtree(over, ok, base_parent)))
					.collect();
				if in_base {
					// The replaced leaf's comments go with it, which the spec
					// allows; a content-malformed line retained on it is
					// content the parser promised to keep, so those move
					// onto the replacement. A comment starts with `#`, a
					// retained line never does.
					let mut kept: Vec<Lead> = Vec::new();
					for &b in base_kids.iter().filter(|&&b| self.arena[b].name == *name) {
						let nd = &self.arena[b];
						for l in nd.leading().iter().chain(nd.inside()).chain(nd.after()) {
							if !l.text.starts_with('#') {
								kept.push(l.clone());
							}
						}
					}
					if !kept.is_empty() {
						let t = self.arena[clones[0].1].triv_mut();
						kept.append(&mut t.leading);
						t.leading = kept;
					}
					replace.insert(name.clone(), clones.into_iter().map(|(_, c)| c).collect());
				} else {
					appended.extend(clones);
				}
			} else {
				for &(pos, ok) in group {
					let okey = over.arena[ok].value.key();
					// A raw block in the higher layer fills a same-named empty
					// binding below, exactly as a fence line fills one inside a
					// single file. Without it, merging two documents and parsing
					// them run together disagree: both bindings survive here and
					// fold there, so the merged output is not a formatter fixpoint.
					let mut target = by_key.get(&(name.clone(), okey.clone())).copied();
					if target.is_none() && matches!(over.arena[ok].value, Value::Raw { .. }) {
						let empty = (name.clone(), empty_key.clone());
						let hit = by_key.get(&empty).copied();
						if let Some(b) = hit {
							self.arena[b].value = over.arena[ok].value.clone();
							by_key.remove(&empty);
							by_key.entry((name.clone(), okey)).or_insert(b);
							target = Some(b);
						}
					}
					match target {
						Some(b) => {
							self.adopt_trivia(b, over, ok);
							self.overlay(b, over, ok);
						}
						None => {
							let c = self.clone_subtree(over, ok, base_parent);
							appended.push((pos, c));
						}
					}
				}
			}
		}
		if replace.is_empty() && appended.is_empty() {
			return;
		}
		// Rebuild once: each replaced group lands at its name's first original
		// position (dropped nodes stay in the arena, unreferenced - reads and
		// emit walk children from the root), appends go at the end.
		let mut newkids: Vec<usize> = Vec::with_capacity(base_kids.len() + appended.len());
		let mut spliced: std::collections::HashSet<&str> = std::collections::HashSet::new();
		for &b in &base_kids {
			let name = self.arena[b].name.as_str();
			match replace.get(name) {
				Some(clones) => {
					if spliced.insert(name) {
						newkids.extend(clones.iter().copied());
					}
				}
				None => newkids.push(b),
			}
		}
		appended.sort_by_key(|&(pos, _)| pos);
		newkids.extend(appended.iter().map(|&(_, c)| c));
		self.arena[base_parent].children = newkids;
	}

	/// Deep-copy `over`'s subtree at `oi` into self's arena under `parent`.
	fn clone_subtree(&mut self, over: &Document, oi: usize, parent: usize) -> usize {
		let src = &over.arena[oi];
		let node = NodeData {
			name: src.name.clone(),
			value: src.value.clone(),
			children: Vec::new(),
			parent,
			line: src.line,
			star_list: src.star_list,
			star_mixed: src.star_mixed,
			trivia: src.trivia.clone(),
			blank_before: src.blank_before,
			src_set: src.src_set,
			src: src.src.clone(),
			name_src: src.name_src.clone(),
		};
		let idx = self.arena.len();
		self.arena.push(node);
		for &ok in &over.arena[oi].children {
			let c = self.clone_subtree(over, ok, idx);
			self.arena[idx].children.push(c);
		}
		idx
	}
}

impl Default for Document {
	fn default() -> Document {
		Document::new()
	}
}

impl std::str::FromStr for Document {
	/// Parsing at Standard cannot fail - a malformed line becomes a diagnostic,
	/// not a refusal - so `"a: 1".parse::<Document>()` is infallible. Use
	/// `parse_with(text, Strictness::Strict)` for the load that can fail.
	type Err = std::convert::Infallible;
	fn from_str(text: &str) -> Result<Document, Self::Err> {
		Ok(Document::parse(text))
	}
}

impl std::fmt::Display for Document {
	/// The canonical form, so `println!("{doc}")` prints the document the way
	/// `fmt` would write it.
	fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
		f.write_str(&self.to_canonical())
	}
}

// ---------------------------------------------------------------------------
// Coercion ("intelligent but safe"; Loose re-admits a closed list of tricks)
// ---------------------------------------------------------------------------

const CURRENCY: &[char] = &[
	'$', '¢', '£', '¤', '¥', '₩', '₪', '₫', '€', '₭', '₮', '₱', '₲', '₴', '₹', '₺', '₼', '₽', '₾',
	'₿',
];

/// The remainder after a leading currency symbol, with the space a person
/// writes after one taken off - `$ 1200` reached the int path's thousands
/// branch, which trims, and the float path's shape test, which does not.
fn strip_currency(t: &str) -> &str {
	let mut it = t.chars();
	match it.next() {
		Some(c) if CURRENCY.contains(&c) => it.as_str().trim_start(),
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
		// Parse the magnitude as u64, then range-check against the sign, so the
		// negative i64::MIN magnitude (0x8000000000000000) reads like its decimal
		// spelling instead of overflowing an i64 parse.
		let m = u64::from_str_radix(h, 16).ok()?;
		return if neg {
			if m == (i64::MAX as u64) + 1 {
				Some(i64::MIN)
			} else if m <= i64::MAX as u64 {
				Some(-(m as i64))
			} else {
				None
			}
		} else if m <= i64::MAX as u64 {
			Some(m as i64)
		} else {
			None
		};
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
		// i64::MAX has no exact f64 - the cast rounds it up to 2^63 - so a
		// `<=` against it lets 2^63 in and the cast below saturates to a value
		// the text never said. Bound on 2^63 itself, exclusively. i64::MIN is
		// exact, so the low end needs no such care.
		if r >= i64::MIN as f64 && r < 9_223_372_036_854_775_808.0 {
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
		// A literal past the double range parses as an infinity, which no
		// double holds and no setter can write back: BadType, like a text
		// that is not a number at all.
		t.parse::<f64>().ok().filter(|v| v.is_finite())?
	} else {
		// An integer is a valid float on read (incl. hex and quoted thousands).
		let el = Element {
			text: t.to_string(),
			quoted: e.quoted,
		};
		match parse_int_text_no_loose(&el) {
			Some(i) => i as f64,
			None => parse_int_text_wide(&el)?,
		}
	};
	Some(if percent { v / 100.0 } else { v })
}

/// Integer forms only (no Loose float fallback) - used by the float path so the
/// two can't recurse into each other.
fn parse_int_text_no_loose(e: &Element) -> Option<i64> {
	parse_int_text(e, Strictness::Standard)
}

/// The two integer spellings the plain float parse does not read - hex, and
/// quoted thousands - past the i64 range, as a double: a float read is bounded
/// by the double, not by the integer type. Hex goes in digit by digit in the
/// double, so every binding rounds the same way; the spellings mirror
/// parse_int_text.
fn parse_int_text_wide(e: &Element) -> Option<f64> {
	let t = e.text.trim();
	let (neg, body) = match t.strip_prefix('-') {
		Some(r) => (true, r),
		None => (false, t.strip_prefix('+').unwrap_or(t)),
	};
	let v = if let Some(h) = body.strip_prefix("0x").or_else(|| body.strip_prefix("0X"))
		&& !h.is_empty()
		&& h.bytes().all(|b| b.is_ascii_hexdigit())
	{
		h.bytes().fold(0.0f64, |v, b| {
			v * 16.0 + f64::from((b as char).to_digit(16).unwrap_or(0))
		})
	} else if e.quoted && body.contains(',') {
		let groups: Vec<&str> = body.split(',').collect();
		let well_formed = groups.len() > 1
			&& !groups[0].is_empty()
			&& groups[0].len() <= 3
			&& groups[0].bytes().all(|b| b.is_ascii_digit())
			&& groups[1..]
				.iter()
				.all(|g| g.len() == 3 && g.bytes().all(|b| b.is_ascii_digit()));
		if !well_formed {
			return None;
		}
		body.replace(',', "").parse::<f64>().ok()?
	} else {
		return None;
	};
	if !v.is_finite() {
		return None;
	}
	Some(if neg { -v } else { v })
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
		// The day is DD, like every other form's: a plain integer parse takes a
		// leading '+' and any number of leading zeros, which the whitelist does
		// not list and the delimited spellings already refuse.
		if let Some(m) = month_from_name(toks[0]) {
			let day_tok = toks[1].strip_suffix(',').unwrap_or(toks[1]);
			let d: u32 = parse_num2(day_tok)?;
			let y: i32 = parse_year4(toks[2])?;
			return valid_date(y, m, d).then_some((y, m, d));
		}
		if let Some(m) = month_from_name(toks[1]) {
			let d: u32 = parse_num2(toks[0])?;
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

/// Whether a datetime's canonical spelling reads back as the same value: the
/// setter's inverse-of-the-read promise, checked by making the round trip.
fn datetime_reads_back(v: &ShclDateTime) -> bool {
	parse_datetime(&v.to_string()).as_ref() == Some(v)
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
	/// Single node at a path, or the failing status.
	fn node_at(&self, path: &str) -> Result<usize, Status> {
		match self.resolve(path)? {
			Resolved::None => Err(Status::NotFound),
			Resolved::Many(_) | Resolved::Slots(_) => Err(Status::Multiple),
			Resolved::One(n) => Ok(n),
		}
	}

	/// A read's `raw`: the verbatim source value text when the value came from
	/// one source line, else the display form (writer-built, stacked list, raw
	/// block - shapes with no one-line source spelling).
	fn raw_of(&self, n: usize) -> String {
		match &self.arena[n].src {
			Some(s) => s.clone(),
			None => self.arena[n].value.display(),
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
		let node = match self.node_at(path) {
			Ok(n) => n,
			Err(st) => return Read::new(T::default(), st, None),
		};
		let value = &self.arena[node].value;
		let raw = Some(self.raw_of(node));
		let line = self.arena[node].line;
		match self.scalar_element(value) {
			Ok(el) => match coerce(el) {
				Some(v) => Read::new(v, Status::Good, raw).at(line, el.quoted),
				None => Read::new(T::default(), Status::BadType, raw).at(line, el.quoted),
			},
			Err(st) => Read::new(T::default(), st, raw).at(line, false),
		}
	}

	/// Full-tier integer read at a path, coerced per the document's strictness.
	pub fn read_int(&self, path: &str) -> Read<i64> {
		let lvl = self.strictness;
		self.read_scalar(path, |e| parse_int_text(e, lvl))
	}

	/// Full-tier float read at a path, coerced per the document's strictness.
	pub fn read_float(&self, path: &str) -> Read<f64> {
		let lvl = self.strictness;
		self.read_scalar(path, |e| parse_float_text(e, lvl))
	}

	/// Full-tier bool read at a path, coerced per the document's strictness.
	pub fn read_bool(&self, path: &str) -> Read<bool> {
		let lvl = self.strictness;
		self.read_scalar(path, |e| parse_bool_text(&e.text, lvl))
	}

	/// Full-tier datetime read at a path.
	pub fn read_datetime(&self, path: &str) -> Read<ShclDateTime> {
		self.read_scalar(path, |e| parse_datetime(&e.text))
	}

	/// Any value reads as a string: a raw block yields its content, an array its
	/// canonical inline text. Escapes are applied.
	pub fn read_string(&self, path: &str) -> Read<String> {
		let node = match self.node_at(path) {
			Ok(n) => n,
			Err(st) => return Read::new(String::new(), st, None),
		};
		let value = &self.arena[node].value;
		let raw = Some(self.raw_of(node));
		let line = self.arena[node].line;
		match value {
			Value::Empty => Read::new(String::new(), Status::Empty, raw).at(line, false),
			Value::Raw(r) => Read::new(r.content.clone(), Status::Good, raw).at(line, false),
			Value::Cell(els) if els.len() == 1 => {
				Read::new(apply_escapes(&els[0].text), Status::Good, raw).at(line, els[0].quoted)
			}
			// Canonical inline form (quoting + escapes intact), so the string
			// re-parses to the same array - not the bare display join.
			Value::Cell(els) => Read::new(
				els.iter().map(emit_element).collect::<Vec<_>>().join(", "),
				Status::Good,
				raw,
			)
			.at(line, false),
		}
	}

	/// Raw-block content (verbatim). Non-block values are BadType.
	pub fn read_raw(&self, path: &str) -> Read<String> {
		let node = match self.node_at(path) {
			Ok(n) => n,
			Err(st) => return Read::new(String::new(), st, None),
		};
		let value = &self.arena[node].value;
		let raw = Some(self.raw_of(node));
		let line = self.arena[node].line;
		match value {
			Value::Raw(r) => Read::new(r.content.clone(), Status::Good, raw).at(line, false),
			Value::Empty => Read::new(String::new(), Status::Empty, raw).at(line, false),
			Value::Cell(_) => Read::new(String::new(), Status::BadType, raw).at(line, false),
		}
	}

	/// The advisory info-string of a raw block ("" when absent).
	pub fn read_raw_info(&self, path: &str) -> Read<String> {
		let node = match self.node_at(path) {
			Ok(n) => n,
			Err(st) => return Read::new(String::new(), st, None),
		};
		let raw = Some(self.raw_of(node));
		let line = self.arena[node].line;
		match &self.arena[node].value {
			Value::Raw(r) => Read::new(r.info.clone(), Status::Good, raw).at(line, false),
			_ => Read::new(String::new(), Status::BadType, raw).at(line, false),
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
							Ok(el) => {
								let (v, st) = coerced(&coerce, el);
								out.push(v);
								sts.push(st);
							}
							Err(st) => {
								out.push(T::default());
								sts.push(st);
							}
						},
					}
				}
				// No slots at all means the wildcard's parent is not there, so
				// the path did not resolve - Empty is for a node that is.
				let status = if sts.is_empty() {
					Status::NotFound
				} else {
					sts.iter().copied().max().unwrap_or(Status::Good)
				};
				Read::with_slots(out, status, None, sts)
			}
			Ok(Resolved::None) => Read::new(Vec::new(), Status::NotFound, None),
			Ok(Resolved::Many(_)) => Read::new(Vec::new(), Status::Multiple, None),
			Ok(Resolved::One(n)) => {
				let value = &self.arena[n].value;
				let raw = Some(self.raw_of(n));
				let line = self.arena[n].line;
				match value {
					Value::Empty => Read::new(Vec::new(), Status::Empty, raw).at(line, false),
					Value::Raw { .. } => {
						Read::new(Vec::new(), Status::BadType, raw).at(line, false)
					}
					Value::Cell(els) => {
						let mut out = Vec::with_capacity(els.len());
						let mut sts = Vec::with_capacity(els.len());
						for el in els {
							let (v, st) = coerced(&coerce, el);
							out.push(v);
							sts.push(st);
						}
						let status = sts.iter().copied().max().unwrap_or(Status::Good);
						// A one-element cell has a single scalar element, so
						// the flag means the same thing here as on the scalar
						// read of the same node.
						let quoted = els.len() == 1 && els[0].quoted;
						Read::with_slots(out, status, raw, sts).at(line, quoted)
					}
				}
			}
		}
	}

	/// Full-tier integer-array read at a path (per-slot statuses in `slots`).
	pub fn read_int_array(&self, path: &str) -> Read<Vec<i64>> {
		let lvl = self.strictness;
		self.read_array(path, |e| parse_int_text(e, lvl))
	}

	/// Full-tier float-array read at a path.
	pub fn read_float_array(&self, path: &str) -> Read<Vec<f64>> {
		let lvl = self.strictness;
		self.read_array(path, |e| parse_float_text(e, lvl))
	}

	/// Full-tier bool-array read at a path.
	pub fn read_bool_array(&self, path: &str) -> Read<Vec<bool>> {
		let lvl = self.strictness;
		self.read_array(path, |e| parse_bool_text(&e.text, lvl))
	}

	/// Full-tier datetime-array read at a path.
	pub fn read_datetime_array(&self, path: &str) -> Read<Vec<ShclDateTime>> {
		self.read_array(path, |e| parse_datetime(&e.text))
	}

	/// Full-tier string-array read at a path, escapes applied per element.
	pub fn read_string_array(&self, path: &str) -> Read<Vec<String>> {
		self.read_array(path, |e| Some(apply_escapes(&e.text)))
	}

	// Full tier, Result form: Ok(value) on Good; the sentinel otherwise. Empty
	// still comes back as Err(Empty) here; use read_* to also get the empty value.

	/// `read_int` reduced to a `Result`.
	pub fn get_int(&self, path: &str) -> Result<i64, Status> {
		let r = self.read_int(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	/// `read_float` reduced to a `Result`.
	pub fn get_float(&self, path: &str) -> Result<f64, Status> {
		let r = self.read_float(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	/// `read_bool` reduced to a `Result`.
	pub fn get_bool(&self, path: &str) -> Result<bool, Status> {
		let r = self.read_bool(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	/// `read_string` reduced to a `Result`.
	pub fn get_string(&self, path: &str) -> Result<String, Status> {
		let r = self.read_string(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	/// `read_raw` reduced to a `Result`.
	pub fn get_raw(&self, path: &str) -> Result<String, Status> {
		let r = self.read_raw(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	/// `read_raw_info` reduced to a `Result`.
	pub fn get_raw_info(&self, path: &str) -> Result<String, Status> {
		let r = self.read_raw_info(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	/// `read_datetime` reduced to a `Result`.
	pub fn get_datetime(&self, path: &str) -> Result<ShclDateTime, Status> {
		let r = self.read_datetime(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	// Array get-tier: Ok only when the whole read is Good, so `.unwrap_or(def)`
	// gives the convenience "the array, or this fallback array" - the array
	// analogue of the scalar get_*. Per-slot substitution is the full read_*
	// tier (its `slots`) or the CLI's --default, not this.
	/// `read_int_array` reduced to a `Result`.
	pub fn get_int_array(&self, path: &str) -> Result<Vec<i64>, Status> {
		let r = self.read_int_array(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	/// `read_float_array` reduced to a `Result`.
	pub fn get_float_array(&self, path: &str) -> Result<Vec<f64>, Status> {
		let r = self.read_float_array(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	/// `read_bool_array` reduced to a `Result`.
	pub fn get_bool_array(&self, path: &str) -> Result<Vec<bool>, Status> {
		let r = self.read_bool_array(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	/// `read_string_array` reduced to a `Result`.
	pub fn get_string_array(&self, path: &str) -> Result<Vec<String>, Status> {
		let r = self.read_string_array(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	/// `read_datetime_array` reduced to a `Result`.
	pub fn get_datetime_array(&self, path: &str) -> Result<Vec<ShclDateTime>, Status> {
		let r = self.read_datetime_array(path);
		if r.status == Status::Good {
			Ok(r.value)
		} else {
			Err(r.status)
		}
	}

	// Convenience tier: the value, or the call-site fallback unless the read is
	// Good. `get_int(p).unwrap_or(0)` says the same thing and still works; these
	// exist so the spelling that means "with a fallback" is `_or` in every
	// binding, and a routine ported between two of them cannot keep the call
	// name while changing which tier it lands on.

	/// The integer at a path, or `def` when the read is not Good.
	pub fn get_int_or(&self, path: &str, def: i64) -> i64 {
		self.get_int(path).unwrap_or(def)
	}

	/// The float at a path, or `def` when the read is not Good.
	pub fn get_float_or(&self, path: &str, def: f64) -> f64 {
		self.get_float(path).unwrap_or(def)
	}

	/// The bool at a path, or `def` when the read is not Good.
	pub fn get_bool_or(&self, path: &str, def: bool) -> bool {
		self.get_bool(path).unwrap_or(def)
	}

	/// The string at a path, or `def` when the read is not Good.
	pub fn get_string_or(&self, path: &str, def: String) -> String {
		self.get_string(path).unwrap_or(def)
	}

	/// The raw-block content at a path, or `def` when the read is not Good.
	pub fn get_raw_or(&self, path: &str, def: String) -> String {
		self.get_raw(path).unwrap_or(def)
	}

	/// The raw block's info-string at a path, or `def` when the read is not Good.
	pub fn get_raw_info_or(&self, path: &str, def: String) -> String {
		self.get_raw_info(path).unwrap_or(def)
	}

	/// The datetime at a path, or `def` when the read is not Good.
	pub fn get_datetime_or(&self, path: &str, def: ShclDateTime) -> ShclDateTime {
		self.get_datetime(path).unwrap_or(def)
	}

	/// The integer array at a path, or `def` when the read is not Good.
	pub fn get_int_array_or(&self, path: &str, def: Vec<i64>) -> Vec<i64> {
		self.get_int_array(path).unwrap_or(def)
	}

	/// The float array at a path, or `def` when the read is not Good.
	pub fn get_float_array_or(&self, path: &str, def: Vec<f64>) -> Vec<f64> {
		self.get_float_array(path).unwrap_or(def)
	}

	/// The bool array at a path, or `def` when the read is not Good.
	pub fn get_bool_array_or(&self, path: &str, def: Vec<bool>) -> Vec<bool> {
		self.get_bool_array(path).unwrap_or(def)
	}

	/// The string array at a path, or `def` when the read is not Good.
	pub fn get_string_array_or(&self, path: &str, def: Vec<String>) -> Vec<String> {
		self.get_string_array(path).unwrap_or(def)
	}

	/// The datetime array at a path, or `def` when the read is not Good.
	pub fn get_datetime_array_or(&self, path: &str, def: Vec<ShclDateTime>) -> Vec<ShclDateTime> {
		self.get_datetime_array(path).unwrap_or(def)
	}
}

// ---------------------------------------------------------------------------
// Validator: schema-as-SHCL
// ---------------------------------------------------------------------------
// The schema is an ordinary parsed document: a flat list of `field: <path>`
// instances whose children are the constraints (closed vocabulary - see
// spec.md "Schema validation"). Validation reuses the accessor's path scan and
// the typed coercions, so document strictness composes for free. Schema faults
// (V09x) come first and the surviving constraints still check the document;
// the unknown-field sweep skips only when a fault cost a path spelling. One
// line-number space per result.

const SCHEMA_TYPES: [&str; 11] = [
	"int",
	"float",
	"bool",
	"string",
	"datetime",
	"raw",
	"int-array",
	"float-array",
	"bool-array",
	"string-array",
	"datetime-array",
];

// The allowed set, pre-coerced at schema-build time into the constraint's type
// space so per-node checks are a plain contains().
#[derive(Clone)]
enum AllowedSet {
	Ints(Vec<i64>),
	Floats(Vec<f64>),
	Bools(Vec<bool>),
	Dates(Vec<ShclDateTime>),
	Strings(Vec<String>),
}

#[derive(Clone)]
struct Constraint {
	path: String, // as written in the schema; message text only
	segs: Vec<Segment>,
	ty: Option<String>, // member of SCHEMA_TYPES
	required: bool,
	allowed: Option<AllowedSet>,
	min_i: Option<i64>,
	max_i: Option<i64>,
	min_f: Option<f64>,
	max_f: Option<f64>,
	repeat: Option<(u64, u64)>,
	reopen: bool,             // H002 suppressor only; validation ignores it
	inherits: Option<String>, // fragment mounted at this path (subtree shape)
	inherits_line: usize,     // schema line of the `inherits` key, for V095
	// Generator-only (`shcl init`): validation ignores both.
	desc: Option<String>,         // `desc`, a one-line description
	default_text: Option<String>, // `default`, emitted as an inline value
}

/// An interpreted schema: the top-level constraints plus the named fragments
/// their `inherits` keys can mount.
struct SchemaDef {
	cons: Vec<Constraint>,
	frags: HashMap<String, Vec<Constraint>>,
	// False when a fault cost the schema a path spelling (unreadable `field:`
	// path, or a mount naming no declared fragment). Key-level faults keep
	// their entry's chain, so only these two classes can turn declared fields
	// into false unknowns - the sweep runs unless one of them happened.
	paths_complete: bool,
}

/// One slot of an array read: the coerced value, or the type's default with
/// BadType.
fn coerced<T: Default>(coerce: &impl Fn(&Element) -> Option<T>, el: &Element) -> (T, Status) {
	match coerce(el) {
		Some(v) => (v, Status::Good),
		None => (T::default(), Status::BadType),
	}
}

fn vdiag(out: &mut Vec<Diagnostic>, line: usize, code: &'static str, msg: String) {
	out.push(Diagnostic {
		line,
		severity: Severity::Error,
		message: msg,
		code,
	});
}

/// One scalar constraint value (escapes applied), or None for anything else.
fn single_text(v: &Value) -> Option<String> {
	match v {
		Value::Cell(els) if els.len() == 1 => Some(apply_escapes(&els[0].text)),
		_ => None,
	}
}

/// Interpret a parsed schema document into constraints and fragments, plus
/// any schema faults (V09x, schema-file lines). Whatever parsed cleanly is
/// kept even when faults are present - a broken key drops that key, a broken
/// field drops that field - so a caller can still check the document against
/// the surviving constraints.
fn build_schema(schema: &Document) -> (SchemaDef, Vec<Diagnostic>) {
	let mut faults: Vec<Diagnostic> = Vec::new();
	let mut cons: Vec<Constraint> = Vec::new();
	let mut frags: HashMap<String, Vec<Constraint>> = HashMap::new();
	let mut paths_complete = true;
	for &f in &schema.arena[ROOT].children {
		let node = &schema.arena[f];
		match node.name.as_str() {
			"field" => {
				if let Some(c) = parse_field(schema, f, &mut faults) {
					cons.push(c);
				} else {
					paths_complete = false;
				}
			}
			"fragment" => {
				let name = single_text(&node.value).filter(|n| !n.is_empty());
				let Some(name) = name else {
					vdiag(
						&mut faults,
						node.line,
						"V094",
						"bad schema fragment".to_string(),
					);
					continue;
				};
				if frags.contains_key(&name) {
					vdiag(
						&mut faults,
						node.line,
						"V094",
						format!("bad schema fragment '{}': duplicate", name),
					);
					continue;
				}
				let mut fcs: Vec<Constraint> = Vec::new();
				for &k in &schema.arena[f].children {
					let kid = &schema.arena[k];
					if kid.name == "field" {
						if let Some(c) = parse_field(schema, k, &mut faults) {
							fcs.push(c);
						} else {
							paths_complete = false;
						}
					} else {
						vdiag(
							&mut faults,
							kid.line,
							"V094",
							format!("bad schema fragment '{}': unknown key '{}'", name, kid.name),
						);
					}
				}
				frags.insert(name, fcs);
			}
			other => {
				vdiag(
					&mut faults,
					node.line,
					"V090",
					format!("unknown schema key '{}'", other),
				);
			}
		}
	}
	// Every mount must name a declared fragment; cycles (self or mutual) are
	// legal - expansion is demand-driven against a finite document.
	for c in cons.iter().chain(frags.values().flatten()) {
		if let Some(fr) = &c.inherits
			&& !frags.contains_key(fr)
		{
			vdiag(
				&mut faults,
				c.inherits_line,
				"V095",
				format!("unknown schema fragment '{}'", fr),
			);
			paths_complete = false;
		}
	}
	// One constraint per line in practice, so line order = file order.
	faults.sort_by_key(|d| d.line);
	(
		SchemaDef {
			cons,
			frags,
			paths_complete,
		},
		faults,
	)
}

/// One `field:` instance (top-level or inside a fragment) -> a Constraint.
/// None = faults were reported and the constraint is dropped.
fn parse_field(schema: &Document, f: usize, faults: &mut Vec<Diagnostic>) -> Option<Constraint> {
	let node = &schema.arena[f];
	let Some(path) = single_text(&node.value) else {
		vdiag(faults, node.line, "V093", "bad schema path".to_string());
		return None;
	};
	let segs = match scan_lookup(&path) {
		Ok(s) if s.value_text.is_none() => s.segments,
		_ => {
			vdiag(
				faults,
				node.line,
				"V093",
				format!("bad schema path: {}", path),
			);
			return None;
		}
	};
	let mut c = Constraint {
		path,
		segs,
		ty: None,
		required: false,
		allowed: None,
		min_i: None,
		max_i: None,
		min_f: None,
		max_f: None,
		repeat: None,
		reopen: false,
		inherits: None,
		inherits_line: 0,
		desc: None,
		default_text: None,
	};
	// Deferred so `min: 1` may precede `type: int` in the file.
	let mut required: Option<bool> = None;
	let mut reopen_seen = false;
	let mut allowed_at: Option<usize> = None;
	let mut default_at: Option<usize> = None;
	let mut min_at: Option<usize> = None;
	let mut max_at: Option<usize> = None;
	for &k in &schema.arena[f].children {
		let kid = &schema.arena[k];
		if kid.value.is_empty() {
			continue; // dangling key: treated as absent
		}
		match kid.name.as_str() {
			"type" => match single_text(&kid.value).map(|t| t.to_ascii_lowercase()) {
				Some(t) if SCHEMA_TYPES.contains(&t.as_str()) => {
					if c.ty.is_some() {
						vdiag(
							faults,
							kid.line,
							"V092",
							"bad schema constraint 'type'".to_string(),
						);
					} else {
						c.ty = Some(t);
					}
				}
				Some(t) => {
					vdiag(
						faults,
						kid.line,
						"V091",
						format!("unknown schema type '{}'", t),
					);
				}
				None => vdiag(
					faults,
					kid.line,
					"V092",
					"bad schema constraint 'type'".to_string(),
				),
			},
			"required" => {
				let v =
					single_text(&kid.value).and_then(|t| parse_bool_text(&t, Strictness::Standard));
				match v {
					Some(b) if required.is_none() => required = Some(b),
					_ => vdiag(
						faults,
						kid.line,
						"V092",
						"bad schema constraint 'required'".to_string(),
					),
				}
			}
			// Consumed by the H002 suppressor; validation itself ignores it,
			// but a bad value still faults so a typo cannot silently disavow
			// nothing.
			"reopen" => {
				let v =
					single_text(&kid.value).and_then(|t| parse_bool_text(&t, Strictness::Standard));
				match v {
					Some(b) if !reopen_seen => {
						reopen_seen = true;
						c.reopen = b;
					}
					_ => vdiag(
						faults,
						kid.line,
						"V092",
						"bad schema constraint 'reopen'".to_string(),
					),
				}
			}
			"allowed" => match &kid.value {
				Value::Cell(_) if allowed_at.is_none() => allowed_at = Some(k),
				_ => vdiag(
					faults,
					kid.line,
					"V092",
					"bad schema constraint 'allowed'".to_string(),
				),
			},
			"min" => match &kid.value {
				Value::Cell(els) if els.len() == 1 && min_at.is_none() => min_at = Some(k),
				_ => vdiag(
					faults,
					kid.line,
					"V092",
					"bad schema constraint 'min'".to_string(),
				),
			},
			"max" => match &kid.value {
				Value::Cell(els) if els.len() == 1 && max_at.is_none() => max_at = Some(k),
				_ => vdiag(
					faults,
					kid.line,
					"V092",
					"bad schema constraint 'max'".to_string(),
				),
			},
			"repeat" => match &kid.value {
				Value::Cell(els) if c.repeat.is_none() && matches!(els.len(), 1 | 2) => {
					let lo = els[0].text.parse::<u64>().ok();
					let hi = els.last().and_then(|e| e.text.parse::<u64>().ok());
					match (lo, hi) {
						(Some(a), Some(b)) if a <= b => c.repeat = Some((a, b)),
						_ => vdiag(
							faults,
							kid.line,
							"V092",
							"bad schema constraint 'repeat'".to_string(),
						),
					}
				}
				_ => vdiag(
					faults,
					kid.line,
					"V092",
					"bad schema constraint 'repeat'".to_string(),
				),
			},
			"inherits" => match single_text(&kid.value).filter(|t| !t.is_empty()) {
				Some(t) if c.inherits.is_none() => {
					c.inherits = Some(t);
					c.inherits_line = kid.line;
				}
				_ => vdiag(
					faults,
					kid.line,
					"V092",
					"bad schema constraint 'inherits'".to_string(),
				),
			},
			// Generator-only (`shcl init`); validation ignores both. First
			// occurrence wins (a merged schema could carry two).
			"desc" => {
				// A comma in a sentence makes the value several elements, and
				// the comment is prose: take them all, spelled as written.
				if c.desc.is_none() {
					c.desc = match &kid.value {
						Value::Cell(els) => Some(
							els.iter()
								.map(|e| apply_escapes(&e.text))
								.collect::<Vec<_>>()
								.join(", "),
						),
						_ => None,
					};
				}
			}
			"default" => {
				if c.default_text.is_none() {
					c.default_text = emit_value_inline(&kid.value);
					default_at = Some(k);
				}
			}
			other => vdiag(
				faults,
				kid.line,
				"V090",
				format!("unknown schema key '{}'", other),
			),
		}
	}
	c.required = required.unwrap_or(false);
	// A raw block has no inline spelling, so a `default` that is one cannot
	// reach a generated line - it used to be dropped and the field emitted with
	// no value at all - and a `default` under `type: raw` goes out inline and
	// then fails its own type check.
	if let Some(dk) = default_at {
		let kid = &schema.arena[dk];
		let raw_default = matches!(kid.value, Value::Raw(_));
		let raw_typed = c.ty.as_deref().is_some_and(|t| t == "raw");
		if raw_default || (raw_typed && c.default_text.is_some()) {
			vdiag(
				faults,
				kid.line,
				"V092",
				"bad schema constraint 'default'".to_string(),
			);
		}
	}
	let base =
		c.ty.as_deref()
			.map(|t| t.strip_suffix("-array").unwrap_or(t))
			.unwrap_or("string");
	if let Some(a) = allowed_at {
		let kid = &schema.arena[a];
		// allowed_at is only ever set for a Cell; if that invariant slips,
		// skip the constraint rather than abort the consumer.
		let Value::Cell(els) = &kid.value else {
			return None;
		};
		// Schema values are read at Standard; only the document's values
		// coerce at the document's strictness.
		let set = match base {
			"int" => els
				.iter()
				.map(|e| parse_int_text(e, Strictness::Standard))
				.collect::<Option<Vec<_>>>()
				.map(AllowedSet::Ints),
			"float" => els
				.iter()
				.map(|e| parse_float_text(e, Strictness::Standard))
				.collect::<Option<Vec<_>>>()
				.map(AllowedSet::Floats),
			"bool" => els
				.iter()
				.map(|e| parse_bool_text(&e.text, Strictness::Standard))
				.collect::<Option<Vec<_>>>()
				.map(AllowedSet::Bools),
			"datetime" => els
				.iter()
				.map(|e| parse_datetime(&e.text))
				.collect::<Option<Vec<_>>>()
				.map(AllowedSet::Dates),
			"raw" => None, // a raw body has no element space to enumerate
			_ => Some(AllowedSet::Strings(
				els.iter().map(|e| apply_escapes(&e.text)).collect(),
			)),
		};
		match set {
			Some(s) => c.allowed = Some(s),
			None => vdiag(
				faults,
				kid.line,
				"V092",
				"bad schema constraint 'allowed'".to_string(),
			),
		}
	}
	for (at, is_min) in [(min_at, true), (max_at, false)] {
		let Some(m) = at else { continue };
		let kid = &schema.arena[m];
		// min/max is only ever a one-element Cell; if that invariant slips,
		// skip the constraint rather than abort the consumer.
		let el = match &kid.value {
			Value::Cell(els) if els.len() == 1 => &els[0],
			_ => continue,
		};
		let key = if is_min { "min" } else { "max" };
		match base {
			"int" => match parse_int_text(el, Strictness::Standard) {
				Some(v) if is_min => c.min_i = Some(v),
				Some(v) => c.max_i = Some(v),
				None => vdiag(
					faults,
					kid.line,
					"V092",
					format!("bad schema constraint '{}'", key),
				),
			},
			"float" => match parse_float_text(el, Strictness::Standard) {
				Some(v) if is_min => c.min_f = Some(v),
				Some(v) => c.max_f = Some(v),
				None => vdiag(
					faults,
					kid.line,
					"V092",
					format!("bad schema constraint '{}'", key),
				),
			},
			_ => vdiag(
				faults,
				kid.line,
				"V092",
				format!("bad schema constraint '{}'", key),
			),
		}
	}
	// A lower bound above the upper one admits nothing, so every value fails
	// twice and the schema, not the config, is what has to change. Reported at
	// the `max` line, which is the one a reader has to look at with `min`.
	let crossed = matches!((c.min_i, c.max_i), (Some(lo), Some(hi)) if lo > hi)
		|| matches!((c.min_f, c.max_f), (Some(lo), Some(hi)) if lo > hi);
	if crossed {
		vdiag(
			faults,
			max_at.map_or(schema.arena[f].line, |m| schema.arena[m].line),
			"V092",
			"bad schema constraint 'max'".to_string(),
		);
		// The range goes, not the field: a key-level fault keeps its entry, so
		// the path still legalizes its name chain for the unknown-field sweep,
		// and the document is not told off twice per value for a range it
		// could never have satisfied.
		c.min_i = None;
		c.max_i = None;
		c.min_f = None;
		c.max_f = None;
	}
	Some(c)
}

/// A schema `default`/`allowed` value re-emitted as an inline value (minimal
/// quoting, array elements joined by ", "). None for empty or raw - neither has
/// a usable one-line form. Used by the generator, not the validator.
fn emit_value_inline(v: &Value) -> Option<String> {
	match v {
		Value::Cell(els) => Some(els.iter().map(emit_element).collect::<Vec<_>>().join(", ")),
		_ => None,
	}
}

// ---------------------------------------------------------------------------
// Schema-driven generation: a schema + the Writer -> a commented starter config.
// ---------------------------------------------------------------------------

fn allowed_join(a: &AllowedSet) -> String {
	match a {
		AllowedSet::Ints(v) => v
			.iter()
			.map(|x| x.to_string())
			.collect::<Vec<_>>()
			.join(", "),
		AllowedSet::Floats(v) => v
			.iter()
			.map(|x| format_f64(*x))
			.collect::<Vec<_>>()
			.join(", "),
		AllowedSet::Bools(v) => v
			.iter()
			.map(|x| if *x { "true" } else { "false" }.to_string())
			.collect::<Vec<_>>()
			.join(", "),
		AllowedSet::Dates(v) => v
			.iter()
			.map(|x| x.to_string())
			.collect::<Vec<_>>()
			.join(", "),
		AllowedSet::Strings(v) => v.join(", "),
	}
}

/// The `# type, ...` annotation line summarizing a constraint, ASCII only.
fn gen_annotation(c: &Constraint, tyname: &str) -> String {
	let mut parts: Vec<String> = vec![tyname.to_string()];
	if let Some(a) = &c.allowed {
		parts.push(format!("one of: {}", allowed_join(a)));
	} else if c.min_i.is_some() || c.max_i.is_some() {
		parts.push(match (c.min_i, c.max_i) {
			(Some(lo), Some(hi)) => format!("{}-{}", lo, hi),
			(Some(lo), None) => format!(">= {}", lo),
			(None, Some(hi)) => format!("<= {}", hi),
			(None, None) => String::new(), // guarded above; keep the map total
		});
	} else if c.min_f.is_some() || c.max_f.is_some() {
		parts.push(match (c.min_f, c.max_f) {
			(Some(lo), Some(hi)) => format!("{}-{}", format_f64(lo), format_f64(hi)),
			(Some(lo), None) => format!(">= {}", format_f64(lo)),
			(None, Some(hi)) => format!("<= {}", format_f64(hi)),
			(None, None) => String::new(), // guarded above; keep the map total
		});
	}
	if let Some((lo, hi)) = c.repeat {
		parts.push(if lo == hi {
			format!("repeat {}", lo)
		} else {
			format!("repeat {}-{}", lo, hi)
		});
	}
	if c.required {
		parts.push("required".to_string());
	}
	parts.join(", ")
}

/// A default carrying a literal newline cannot sit on a value line; the quoted
/// escaped spelling reads back to the same string.
fn gen_default_text(v: &str) -> String {
	if !v.contains('\n') {
		return v.to_string();
	}
	let mut s = String::from("\"");
	for ch in v.chars() {
		match ch {
			'\\' => s.push_str("\\\\"),
			'"' => s.push_str("\\\""),
			'\n' => s.push_str("\\n"),
			'\t' => s.push_str("\\t"),
			c => s.push(c),
		}
	}
	s.push('"');
	s
}

/// Whether a V007 from the self-check is the sanctioned kind: its message
/// ends `: N not in LO..HI`, and LO is 2 or more.
fn v007_sanctioned(message: &str) -> bool {
	message
		.rsplit(" not in ")
		.next()
		.and_then(|range| range.split("..").next())
		.and_then(|lo| lo.parse::<u64>().ok())
		.is_some_and(|lo| lo >= 2)
}

/// Emit a commented, typed starter config from a schema (`shcl init --schema`).
/// Paths that must exist (required, or a repeat lower bound of 1+) are live
/// (their `default`, or an empty value); optional paths are commented out so
/// the file is valid and minimal as-is. A must-exist wildcard path whose
/// parent gets materialized by another live line is generated too, in dotted
/// form - otherwise the file would fail the very schema that produced it -
/// and remaining wildcard or `[#N]` paths (which cannot be materialized) are
/// listed in a trailing comment block. The output always loads clean and
/// validates clean against its schema, except a repeat lower bound of 2+
/// (identical generated lines would merge, so the shortfall is reported).
/// The promise is checked against the finished text, so a schema whose own
/// `default` breaks its field's constraints is a fault (`V097`) instead of a
/// starter config that fails the first time it is checked.
/// A footer naming the format and pointing at the spec is written last unless
/// `no_banner`; the flag is negative so leaving it alone writes the footer.
/// Err = schema faults (V09x), same as `validate`/`check --schema`.
pub fn generate(schema: &Document, no_banner: bool) -> Result<String, Vec<Diagnostic>> {
	// Generation lays the whole schema out, so unlike validation it has no
	// safe partial mode: any fault fails it.
	let (def, faults) = build_schema(schema);
	if !faults.is_empty() {
		return Err(faults);
	}
	let (cons, cuts) = expand_mounts(&def);
	if cons.len() > GEN_MAX_FIELDS {
		return Err(vec![Diagnostic {
			line: 0,
			severity: Severity::Error,
			code: "V096",
			message: format!(
				"schema expands past {} fields; fragments mounted at more than one path multiply",
				GEN_MAX_FIELDS
			),
		}]);
	}
	let must_exist = |c: &Constraint| c.required || matches!(c.repeat, Some((lo, _)) if lo >= 1);
	let has_wild = |c: &Constraint| {
		c.segs
			.iter()
			.any(|s| matches!(s.selector, Some(Selector::Wildcard)))
	};
	// `[#N]` needs a pre-existing instance and its `#` would start a comment on
	// a binding line; a newline inside a selector has no one-line spelling,
	// since the value emitter never escapes one. Both go to the trailing note
	// instead of emitting a broken line. A path deeper than a document may nest
	// cannot be generated either: the line would draw E016 on the way back in.
	// A newline in a NAME is writable: names are stored escape-resolved and the
	// name escaper spells one `\n`.
	let unwritable = |c: &Constraint| {
		c.segs.len() > MAX_DEPTH
			|| c.segs.iter().any(|s| {
				matches!(s.selector, Some(Selector::ByIndex(_)))
					|| s.star || matches!(&s.selector, Some(Selector::ByValue { text, .. }) if text.contains('\n'))
			})
	};
	// Live concrete paths materialize instances; decide which must-exist
	// wildcards get filled (their first-wildcard parent chain is a prefix of
	// some live path). Fixpoint: a fill can materialize another's parent.
	let mut live: Vec<Vec<&str>> = cons
		.iter()
		.filter(|c| !has_wild(c) && !unwritable(c) && must_exist(c))
		.map(|c| names_of(&c.segs))
		.collect();
	let mut fill = vec![false; cons.len()];
	loop {
		let mut changed = false;
		for (i, c) in cons.iter().enumerate() {
			if fill[i] || !has_wild(c) || unwritable(c) || !must_exist(c) {
				continue;
			}
			let Some(k) = c
				.segs
				.iter()
				.position(|s| matches!(s.selector, Some(Selector::Wildcard)))
			else {
				continue;
			};
			let parent = names_of(&c.segs[..k + 1]);
			// A wildcard in the last segment needs no other line to
			// materialize its parent: the line generated from it is that
			// instance.
			if k + 1 == c.segs.len()
				|| live
					.iter()
					.any(|p| p.len() >= parent.len() && p[..parent.len()] == parent[..])
			{
				fill[i] = true;
				live.push(names_of(&c.segs));
				changed = true;
			}
		}
		if !changed {
			break;
		}
	}
	// A live line with a value materializes an instance carrying that value,
	// and a dotted child names the empty-valued instance instead - so `srv:
	// web` followed by `srv.port:` is two `srv` nodes, and the child never
	// lands where the schema looks. Any line under such a parent selects it
	// by its value: `srv[web].port:`.
	// A filled wildcard emits a valued line of its own, so it belongs here too.
	let parent_values: HashMap<Vec<&str>, &str> = cons
		.iter()
		.enumerate()
		.filter(|(i, c)| (!has_wild(c) || fill[*i]) && !unwritable(c) && must_exist(c))
		.filter_map(|(_, c)| c.default_text.as_deref().map(|d| (names_of(&c.segs), d)))
		.collect();
	// A path that cannot be written at all belongs in the trailing note, but one
	// that must exist can never be satisfied from there: the self-check would
	// then report the document as missing a path, which points at the config
	// rather than at the schema line that cannot be generated.
	// A repeat lower bound of 2 or more is the one documented shortfall - the
	// line is emitted once and the count reported - so it is not this fault.
	let cannot_satisfy =
		|c: &Constraint| c.required || matches!(c.repeat, Some((lo, _)) if lo == 1);
	if let Some(c) = cons
		.iter()
		.find(|c| cannot_satisfy(c) && unwritable(c) && !has_wild(c))
	{
		return Err(vec![Diagnostic {
			line: 0,
			severity: Severity::Error,
			code: "V097",
			message: format!(
				"required path cannot be generated: {}",
				c.path.replace('\n', "\\n")
			),
		}]);
	}
	let mut out = String::new();
	let mut wild: Vec<(String, String)> = Vec::new();
	// Dropping a trailing `[*]` can render the same line a concrete sibling
	// already wrote; the first spelling wins.
	let mut emitted: HashSet<String> = HashSet::new();
	let mut first = true;
	for (i, c) in cons.iter().enumerate() {
		let tyname = c.ty.clone().unwrap_or_else(|| "any".to_string());
		if unwritable(c) || (has_wild(c) && !fill[i]) {
			wild.push((c.path.replace('\n', "\\n"), tyname));
			continue;
		}
		// A filled wildcard emits in dotted form, targeting the materialized
		// instance - by its value when the materializing line carries one.
		// Rebuilt from the parsed segments, not by cutting text out of the
		// path: the same path can be written several ways, and only the
		// segments say what it means. Otherwise the schema's own spelling.
		let under_valued_parent = (1..c.segs.len()).any(|k| {
			c.segs[k - 1].selector.is_none() && parent_values.contains_key(&names_of(&c.segs[..k]))
		});
		// A name carrying a newline has no verbatim spelling on a binding line;
		// the segment renderer escapes it, so such a path goes through there
		// whether or not it was filled.
		let path = if fill[i] || under_valued_parent || c.path.contains('\n') {
			gen_path_text(&c.segs, &parent_values)
		} else {
			c.path.clone()
		};
		if !emitted.insert(path.clone()) {
			continue;
		}
		if !first {
			out.push('\n');
		}
		first = false;
		if let Some(d) = &c.desc {
			for line in d.split('\n') {
				out.push_str("# ");
				out.push_str(line);
				out.push('\n');
			}
		}
		out.push_str("# ");
		// The annotation is a comment: a newline smuggled in via an allowed
		// string value must not break out of it.
		out.push_str(&gen_annotation(c, &tyname).replace('\n', "\\n"));
		out.push('\n');
		let prefix = if must_exist(c) { "" } else { "#" };
		match &c.default_text {
			Some(v) => out.push_str(&format!("{}{}: {}\n", prefix, path, gen_default_text(v))),
			None => out.push_str(&format!("{}{}:\n", prefix, path)),
		}
	}
	// Cycle-cut mounts last: their "type" column names the fragment that
	// belongs at the path.
	wild.extend(cuts);
	if !wild.is_empty() {
		if !first {
			out.push('\n');
		}
		out.push_str("# Paths needing an instance name (not generated):\n");
		for (p, t) in &wild {
			out.push_str(&format!("#   {}   {}\n", p, t));
		}
	}
	if !no_banner {
		if !out.is_empty() {
			out.push('\n');
		}
		out.push_str(GEN_BANNER);
	}
	// The output promises to validate clean against the schema that produced
	// it, so check that here rather than trusting each branch above. A
	// `default` outside its own field's constraints is the schema's fault, and
	// the author should hear about it instead of getting a starter config that
	// fails the first time it is checked. The one sanctioned shortfall is a
	// V007 for a repeat lower bound of 2+: generating that many identical
	// lines merges them into one. A lower bound of 1 is a must-exist path
	// like any other, so its V007 is a fault.
	let bad: Vec<Diagnostic> = Document::parse(&out)
		.validate(schema)
		.into_iter()
		.filter(|d| {
			d.severity == Severity::Error && !(d.code == "V007" && v007_sanctioned(&d.message))
		})
		.map(|d| Diagnostic {
			line: 0,
			severity: Severity::Error,
			code: "V097",
			message: format!(
				"generated value fails the schema that produced it: {}",
				d.message
			),
		})
		.collect();
	if !bad.is_empty() {
		return Err(bad);
	}
	Ok(out)
}

/// Ceiling on how many fields one schema may expand to. Fragments that mount
/// each other at more than one path multiply, so a short schema can otherwise
/// ask for more output than the machine can hold; past this the generator
/// reports a schema fault rather than running until something breaks.
const GEN_MAX_FIELDS: usize = 10_000;

/// Footer telling whoever opens the generated file what the format is and
/// where its spec lives. It is output, so every binding emits these bytes
/// exactly; the Legal line names SHCL as its subject so it cannot be read as
/// a claim over the config it sits in.
const GEN_BANNER: &str = "\
#
# This config file format is SHCL.
# \"Simple Hierarchical Config Language\"
#    Home     https://github.com/jim-collier/shcl
#    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md
#    Legal    SHCL is Copyright © 2026 Jim Collier. License: MIT. No warranty.
#
";

/// Render parsed segments back as a dotted path, dropping wildcard selectors
/// (a generated line targets the one instance it materializes) and quoting a
/// name that needs it, so the result is a path the scanner reads back the same.
/// A segment whose prefix names a live line carrying a value selects that
/// instance by the value, in place of a wildcard or a bare name.
fn gen_path_text(segs: &[Segment], parent_values: &HashMap<Vec<&str>, &str>) -> String {
	let mut out = String::new();
	for (i, s) in segs.iter().enumerate() {
		if i > 0 {
			out.push('.');
		}
		if s.star {
			out.push('*');
		} else {
			out.push_str(&emit_name(&s.name));
		}
		if i + 1 < segs.len()
			&& !matches!(s.selector, Some(Selector::ByValue { .. }))
			&& let Some(v) = parent_values.get(&names_of(&segs[..=i]))
		{
			out.push('[');
			out.push_str(&gen_selector_text(v));
			out.push(']');
			continue;
		}
		match &s.selector {
			Some(Selector::ByValue { text, quoted }) => {
				out.push('[');
				if *quoted {
					out.push_str(&quote_text(text));
				} else {
					out.push_str(text);
				}
				out.push(']');
			}
			Some(Selector::ByIndex(k)) => {
				out.push_str(&format!("[#{}]", k));
			}
			Some(Selector::Wildcard) | None => {}
		}
	}
	out
}

/// A default's spelling inside a `[value]` selector: the value-side text as
/// is, except that a bracket or a backslash would end or escape the selector,
/// so those go quoted (the selector matches on the escaped display, so the
/// quoted spelling finds the bare value).
fn gen_selector_text(v: &str) -> String {
	if v.contains('\n') {
		return gen_default_text(v);
	}
	// The scanner reads a bare selector body as an index when it is all digits
	// (with an optional sign or `#`), and as a wildcard when it is `*`, so a
	// default of that shape has to be quoted or the line names an instance
	// that is not there.
	let body = v.trim();
	let reads_as_selector = body == "*"
		|| body.parse::<u64>().is_ok()
		|| body
			.strip_prefix('#')
			.is_some_and(|d| d.parse::<u64>().is_ok());
	if (v.contains(['[', ']', '\\']) || reads_as_selector) && !quoted_shape(v) {
		return quote_text(v);
	}
	v.to_string()
}

fn names_of(segs: &[Segment]) -> Vec<&str> {
	segs.iter().map(|s| s.name.as_str()).collect()
}

/// Inline every fragment mount into a flat constraint list, depth-first in
/// schema order, each field's path and segments prefixed by its mount's. A
/// mount whose fragment is already expanding (a cycle) stops there and is
/// returned as (path, fragment name) for the trailing not-generated block.
fn expand_mounts(def: &SchemaDef) -> (Vec<Constraint>, Vec<(String, String)>) {
	fn go(
		list: &[Constraint],
		def: &SchemaDef,
		at: Option<(&str, &[Segment])>,
		stack: &mut Vec<String>,
		out: &mut Vec<Constraint>,
		cuts: &mut Vec<(String, String)>,
	) {
		for c in list {
			let mut cc = c.clone();
			if let Some((p, s)) = at {
				cc.path = format!("{}.{}", p, c.path);
				let mut segs = s.to_vec();
				segs.extend(c.segs.iter().cloned());
				cc.segs = segs;
			}
			let path = cc.path.clone();
			let segs = cc.segs.clone();
			if out.len() > GEN_MAX_FIELDS {
				return;
			}
			out.push(cc);
			if let Some(fr) = &c.inherits {
				// A chain long enough to outrun the stack, or a mount that
				// re-enters, stops here and is noted instead of expanded.
				if stack.iter().any(|x| x == fr) || stack.len() >= MAX_DEPTH {
					cuts.push((path.replace('\n', "\\n"), fr.clone()));
				} else if let Some(fcs) = def.frags.get(fr) {
					stack.push(fr.clone());
					go(fcs, def, Some((&path, &segs)), stack, out, cuts);
					stack.pop();
				}
			}
		}
	}
	let mut out: Vec<Constraint> = Vec::new();
	let mut cuts: Vec<(String, String)> = Vec::new();
	let mut stack: Vec<String> = Vec::new();
	go(&def.cons, def, None, &mut stack, &mut out, &mut cuts);
	(out, cuts)
}

/// Levenshtein distance capped at `cap`, for the "did you mean" prose (never
/// the code): anything past the cap comes back as `cap + 1`. Only the band
/// `|i - j| <= cap` of the table is computed, so a pair costs linear time in
/// the names' length, and a length gap past the cap needs no table at all.
fn edit_distance(a: &str, b: &str, cap: usize) -> usize {
	let a: Vec<char> = a.chars().collect();
	let b: Vec<char> = b.chars().collect();
	let inf = cap + 1;
	if a.len().abs_diff(b.len()) > cap {
		return inf;
	}
	let mut prev: Vec<usize> = (0..=b.len()).map(|j| j.min(inf)).collect();
	let mut cur = vec![inf; b.len() + 1];
	for i in 1..=a.len() {
		cur[0] = i.min(inf);
		let lo = i.saturating_sub(cap).max(1);
		let hi = (i + cap).min(b.len());
		if lo > 1 {
			cur[lo - 1] = inf;
		}
		let mut row_min = cur[0];
		for j in lo..=hi {
			let cost = if a[i - 1] == b[j - 1] { 0 } else { 1 };
			cur[j] = (prev[j] + 1)
				.min(cur[j - 1] + 1)
				.min(prev[j - 1] + cost)
				.min(inf);
			row_min = row_min.min(cur[j]);
		}
		if hi < b.len() {
			cur[hi + 1] = inf;
		}
		// No cell in a later row can come back under this row's minimum.
		if row_min > cap {
			return inf;
		}
		std::mem::swap(&mut prev, &mut cur);
	}
	prev[b.len()]
}

impl Document {
	/// Validate this document against a schema document (itself plain SHCL -
	/// spec.md "Schema validation"). Empty result = the document conforms.
	/// Diagnostic lines are document lines (0 = document scope); schema faults
	/// (V09x, schema-file lines) come first, and the surviving constraints
	/// still check the document. The unknown-field sweep runs too, unless a
	/// fault cost the schema a path spelling (an unreadable `field:` path, or
	/// a mount naming no declared fragment) - only those can turn declared
	/// fields into false unknowns; a key-level fault keeps its entry's chain.
	/// The H001/H002 hints a schema disavows are NOT dropped by this call: they
	/// live on the parse's diagnostics, which validation does not touch. Parse
	/// then validate and they are still there - apply
	/// `suppress_declared_repeats`/`suppress_declared_reopens` yourself, or use
	/// `load_and_validate`, which runs both for you.
	pub fn validate(&self, schema: &Document) -> Vec<Diagnostic> {
		let (def, faults) = build_schema(schema);
		let mut out = faults;
		// One mount set for the whole schema: two top-level paths can resolve
		// to the same node and mount the same fragment there, and the spec
		// says each fragment runs once per node.
		let mut mounted = std::collections::HashSet::new();
		for c in &def.cons {
			self.v_check_from(c, &def, ROOT, 0, &mut out, &mut mounted);
		}
		if def.paths_complete {
			self.v_unknown(&def, &mut out);
		}
		out
	}

	// Resolution contexts: the whole document for a plain path; each enclosing
	// instance for the part of a path after a wildcard. required/repeat evaluate
	// per context (anchor line 0 = document scope), so `server[*].port` +
	// required means a port under EACH server - vacuously true with no servers.
	fn v_contexts(
		&self,
		start: Vec<usize>,
		segs: &[Segment],
		anchor: usize,
		out: &mut Vec<(usize, Vec<usize>)>,
	) {
		let mut cur = start;
		for (i, seg) in segs.iter().enumerate() {
			let mut next: Vec<usize> = Vec::new();
			for &n in &cur {
				if seg.star {
					next.extend(self.arena[n].children.iter().copied());
				} else {
					next.extend(self.children_named(n, &seg.name));
				}
			}
			if seg.star {
				// Name wildcard: same per-instance split as `[*]`, any child name.
				let rest = &segs[i + 1..];
				if rest.is_empty() {
					out.push((anchor, next));
				} else {
					for inst in next {
						let line = self.arena[inst].line;
						self.v_contexts(vec![inst], rest, line, out);
					}
				}
				return;
			}
			match &seg.selector {
				None => cur = next,
				Some(Selector::ByValue { text, quoted }) => {
					let want = apply_escapes(text);
					cur = next
						.into_iter()
						.filter(|&c| {
							disp_key(&self.arena[c].value) == want
								&& (!quoted || single_scalar(&self.arena[c].value))
						})
						.collect();
				}
				Some(Selector::ByIndex(k)) => {
					cur = index_usize(*k)
						.and_then(|i| next.get(i))
						.map(|&c| vec![c])
						.unwrap_or_default();
				}
				Some(Selector::Wildcard) => {
					let rest = &segs[i + 1..];
					if rest.is_empty() {
						out.push((anchor, next));
					} else {
						for inst in next {
							let line = self.arena[inst].line;
							self.v_contexts(vec![inst], rest, line, out);
						}
					}
					return;
				}
			}
		}
		out.push((anchor, cur));
	}

	// A mounted fragment's fields run per resolved node, right after that
	// node's own checks, in fragment order - depth-first, so diagnostic order
	// stays derivable. Termination is structural: every mount descends at
	// least one document level, and the document is finite.
	fn v_check_from(
		&self,
		c: &Constraint,
		def: &SchemaDef,
		start: usize,
		anchor0: usize,
		out: &mut Vec<Diagnostic>,
		mounted: &mut std::collections::HashSet<(String, usize)>,
	) {
		let mut ctxs: Vec<(usize, Vec<usize>)> = Vec::new();
		self.v_contexts(vec![start], &c.segs, anchor0, &mut ctxs);
		for (anchor, found) in &ctxs {
			if c.required && found.is_empty() {
				vdiag(
					out,
					*anchor,
					"V002",
					format!("required path missing: {}", c.path),
				);
			}
			if let Some((lo, hi)) = c.repeat {
				let n = found.len() as u64;
				if n < lo || n > hi {
					vdiag(
						out,
						*anchor,
						"V007",
						format!(
							"instance count out of bounds at '{}': {} not in {}..{}",
							c.path, n, lo, hi
						),
					);
				}
			}
			for &n in found {
				self.v_node(c, n, out);
				if let Some(fr) = &c.inherits
					&& let Some(fcs) = def.frags.get(fr)
				{
					// Two constraints can resolve to the same node and mount the
					// same fragment there. The second mount would repeat the
					// first's work and its diagnostics, and repeating it per
					// level is what makes a recursive schema cost double per
					// document level, so each pair is done once.
					if mounted.insert((fr.clone(), n)) {
						for fc in fcs {
							self.v_check_from(fc, def, n, self.arena[n].line, out, mounted);
						}
					}
				}
			}
		}
	}

	fn v_node(&self, c: &Constraint, n: usize, out: &mut Vec<Diagnostic>) {
		let node = &self.arena[n];
		let line = node.line;
		let kind = c.ty.as_deref();
		let base = kind
			.map(|t| t.strip_suffix("-array").unwrap_or(t))
			.unwrap_or("string");
		let is_array = kind.is_some_and(|t| t.ends_with("-array"));
		let wrong = |out: &mut Vec<Diagnostic>| {
			vdiag(
				out,
				line,
				"V003",
				format!(
					"wrong type at '{}': value is not a valid {}",
					c.path,
					kind.unwrap_or("string")
				),
			);
		};
		match &node.value {
			// Empty passes everything; required already counted it as present.
			Value::Empty => {}
			Value::Raw(r) => {
				let content = &r.content;
				// A raw block satisfies `raw` and scalar `string` (any value
				// reads as a string); every other kind is a type miss.
				if kind.is_some() && (base != "raw" && base != "string" || is_array) {
					wrong(out);
					return;
				}
				if let Some(AllowedSet::Strings(set)) = &c.allowed
					&& !set.contains(content)
				{
					vdiag(
						out,
						line,
						"V004",
						format!("value not allowed at '{}': {}", c.path, one_line(content)),
					);
				}
			}
			Value::Cell(els) => {
				if base == "raw" {
					wrong(out);
					return;
				}
				// A scalar kind on a multi-element value is the array-where-one-
				// scalar-expected miss - except string, which reads arrays.
				if kind.is_some() && !is_array && base != "string" && els.len() > 1 {
					wrong(out);
					return;
				}
				match base {
					"int" => {
						let parsed: Option<Vec<i64>> = els
							.iter()
							.map(|e| parse_int_text(e, self.strictness))
							.collect();
						let Some(vals) = parsed else {
							wrong(out);
							return;
						};
						if let Some(AllowedSet::Ints(set)) = &c.allowed
							&& let Some(i) = vals.iter().position(|v| !set.contains(v))
						{
							vdiag(
								out,
								line,
								"V004",
								format!(
									"value not allowed at '{}': {}",
									c.path,
									one_line(&els[i].text)
								),
							);
						}
						if let Some(lo) = c.min_i
							&& vals.iter().any(|v| *v < lo)
						{
							vdiag(
								out,
								line,
								"V005",
								format!("value below min at '{}'", c.path),
							);
						}
						if let Some(hi) = c.max_i
							&& vals.iter().any(|v| *v > hi)
						{
							vdiag(
								out,
								line,
								"V006",
								format!("value above max at '{}'", c.path),
							);
						}
					}
					"float" => {
						let parsed: Option<Vec<f64>> = els
							.iter()
							.map(|e| parse_float_text(e, self.strictness))
							.collect();
						let Some(vals) = parsed else {
							wrong(out);
							return;
						};
						if let Some(AllowedSet::Floats(set)) = &c.allowed
							&& let Some(i) = vals.iter().position(|v| !set.contains(v))
						{
							vdiag(
								out,
								line,
								"V004",
								format!(
									"value not allowed at '{}': {}",
									c.path,
									one_line(&els[i].text)
								),
							);
						}
						if let Some(lo) = c.min_f
							&& vals.iter().any(|v| *v < lo)
						{
							vdiag(
								out,
								line,
								"V005",
								format!("value below min at '{}'", c.path),
							);
						}
						if let Some(hi) = c.max_f
							&& vals.iter().any(|v| *v > hi)
						{
							vdiag(
								out,
								line,
								"V006",
								format!("value above max at '{}'", c.path),
							);
						}
					}
					"bool" => {
						let parsed: Option<Vec<bool>> = els
							.iter()
							.map(|e| parse_bool_text(&e.text, self.strictness))
							.collect();
						let Some(vals) = parsed else {
							wrong(out);
							return;
						};
						if let Some(AllowedSet::Bools(set)) = &c.allowed
							&& let Some(i) = vals.iter().position(|v| !set.contains(v))
						{
							vdiag(
								out,
								line,
								"V004",
								format!(
									"value not allowed at '{}': {}",
									c.path,
									one_line(&els[i].text)
								),
							);
						}
					}
					"datetime" => {
						let parsed: Option<Vec<ShclDateTime>> =
							els.iter().map(|e| parse_datetime(&e.text)).collect();
						let Some(vals) = parsed else {
							wrong(out);
							return;
						};
						if let Some(AllowedSet::Dates(set)) = &c.allowed
							&& let Some(i) = vals
								.iter()
								.position(|v| !set.iter().any(|s| same_moment(s, v)))
						{
							vdiag(
								out,
								line,
								"V004",
								format!(
									"value not allowed at '{}': {}",
									c.path,
									one_line(&els[i].text)
								),
							);
						}
					}
					// string kind or untyped: every element coerces; only the
					// allowed set can fail, in logical-string space.
					_ => {
						if let Some(AllowedSet::Strings(set)) = &c.allowed {
							let bad = els
								.iter()
								.map(|e| apply_escapes(&e.text))
								.find(|s| !set.contains(s));
							if let Some(b) = bad {
								vdiag(
									out,
									line,
									"V004",
									format!("value not allowed at '{}': {}", c.path, one_line(&b)),
								);
							}
						}
					}
				}
			}
		}
	}

	// Unknown-field sweep: a schema path legalizes its name chain and every
	// prefix (selectors ignored). Only the topmost unknown node is reported;
	// its subtree is implied unknown and skipped.
	fn v_unknown(&self, def: &SchemaDef, out: &mut Vec<Diagnostic>) {
		let cons = &def.cons;
		// Chains below a fragment mount only match by descending the mounts.
		let has_mounts = cons.iter().any(|c| c.inherits.is_some());
		let mut legal: std::collections::HashSet<String> = std::collections::HashSet::new();
		// Sibling names per parent chain, built once (schema order): v_suggest
		// used to rebuild every chain per unknown field, which bit hardest on
		// the wholesale-unmatched documents the feature exists for.
		let mut siblings: HashMap<String, Vec<String>> = HashMap::new();
		// Paths with a `*` segment can't live in the exact-chain hash; they
		// match element-wise (a star matches any one name, prefixes included).
		let mut star_pats: Vec<&[Segment]> = Vec::new();
		for c in cons {
			if c.segs.iter().any(|s| s.star) {
				star_pats.push(&c.segs);
			}
			let mut chain = String::new();
			for s in &c.segs {
				if s.star {
					break; // no sibling entry for '*'; deeper chains are pattern-only
				}
				siblings
					.entry(chain.clone())
					.or_default()
					.push(s.name.clone());
				chain_push(&mut chain, &s.name);
				legal.insert(chain.clone());
			}
		}
		let mut stack: Vec<(usize, String, String)> = self.arena[ROOT]
			.children
			.iter()
			.rev()
			.map(|&c| (c, String::new(), String::new()))
			.collect();
		while let Some((n, pchain, pshown)) = stack.pop() {
			let node = &self.arena[n];
			let mut chain = pchain.clone();
			chain_push(&mut chain, &node.name);
			let shown = if pshown.is_empty() {
				node.name.clone()
			} else {
				format!("{}.{}", pshown, node.name)
			};
			let known = legal.contains(&chain)
				|| star_legal(&star_pats, &chain)
				|| (has_mounts && chain_legal(cons, &def.frags, &chain));
			if !known {
				let hint = v_suggest(&siblings, &pchain, &node.name);
				vdiag(
					out,
					node.line,
					"V001",
					format!("unknown field '{}'{}", shown, hint),
				);
				continue;
			}
			for &k in node.children.iter().rev() {
				stack.push((k, chain.clone(), shown.clone()));
			}
		}
	}
}

/// Chain keys join segments length-prefixed (`<len>:<name>`), not with a bare
/// NUL: NUL is legal in a quoted name, so a single field named "x\0y" would
/// impersonate the two-segment path x.y. Same injectivity reasoning as
/// Value::key's cell encoding - and like it, the length unit is each
/// binding's native one (bytes here), because only injectivity matters.
fn chain_push(chain: &mut String, name: &str) {
	chain.push_str(&name.len().to_string());
	chain.push(':');
	chain.push_str(name);
}

/// Decode a chain key back into its segments. Total: bails at the first
/// shape the encoder can't have produced.
fn chain_parts(chain: &str) -> Vec<&str> {
	let mut parts = Vec::new();
	let b = chain.as_bytes();
	let mut i = 0;
	while i < b.len() {
		let mut n = 0usize;
		while i < b.len() && b[i].is_ascii_digit() {
			n = n * 10 + (b[i] - b'0') as usize;
			i += 1;
		}
		if i >= b.len() || b[i] != b':' || i + 1 + n > b.len() {
			break;
		}
		i += 1;
		parts.push(&chain[i..i + n]);
		i += n;
	}
	parts
}

/// Element-wise chain match against the star-bearing schema paths: a `*`
/// segment matches any one name, and every prefix of a path is legal.
fn star_legal(pats: &[&[Segment]], chain: &str) -> bool {
	if pats.is_empty() {
		return false;
	}
	let parts: Vec<&str> = chain_parts(chain);
	pats.iter().any(|p| {
		p.len() >= parts.len()
			&& parts
				.iter()
				.enumerate()
				.all(|(i, seg)| p[i].star || p[i].name == *seg)
	})
}

/// Chain legality through fragment mounts: the general matcher - element-wise
/// like star_legal (stars wild, prefixes legal), and when a mount's whole path
/// matched with chain left over, the remainder is retried against the mounted
/// fragment's fields. Terminates: every descent consumes >= 1 part. A state
/// is (fragment, parts consumed), and one that has failed is not walked again:
/// two mounts of the same fragment at the same depth used to be walked both,
/// which is 2^depth on a chain that ends unknown.
fn chain_legal(cons: &[Constraint], frags: &HashMap<String, Vec<Constraint>>, chain: &str) -> bool {
	let parts: Vec<&str> = chain_parts(chain);
	let mut dead: std::collections::HashSet<(&str, usize)> = std::collections::HashSet::new();
	chain_parts_legal(cons, "", frags, &parts, 0, &mut dead)
}

fn chain_parts_legal<'a>(
	cons: &'a [Constraint],
	set: &'a str,
	frags: &'a HashMap<String, Vec<Constraint>>,
	parts: &[&str],
	at: usize,
	dead: &mut std::collections::HashSet<(&'a str, usize)>,
) -> bool {
	if dead.contains(&(set, at)) {
		return false;
	}
	let rest = &parts[at..];
	for c in cons {
		let n = c.segs.len();
		let k = rest.len().min(n);
		if (0..k).all(|i| c.segs[i].star || c.segs[i].name == rest[i]) {
			if rest.len() <= n {
				return true;
			}
			if let Some(fr) = &c.inherits
				&& let Some(fcs) = frags.get(fr)
				&& chain_parts_legal(fcs, fr, frags, parts, at + n, dead)
			{
				return true;
			}
		}
	}
	dead.insert((set, at));
	false
}

/// Closest legal sibling name (same parent chain, schema order, edit distance
/// <= 2) as "; did you mean 'x'?" - or nothing. Prose only, never contract.
fn v_suggest(siblings: &HashMap<String, Vec<String>>, parent_chain: &str, name: &str) -> String {
	let mut best: Option<(usize, &str)> = None;
	if let Some(names) = siblings.get(parent_chain) {
		for s in names {
			let dist = edit_distance(name, s, 2);
			if dist <= 2 && best.is_none_or(|(bd, _)| dist < bd) {
				best = Some((dist, s.as_str()));
			}
		}
	}
	match best {
		Some((_, n)) => format!("; did you mean '{}'?", n),
		None => String::new(),
	}
}
