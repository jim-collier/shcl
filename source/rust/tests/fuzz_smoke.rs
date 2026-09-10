// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]

//! Deterministic fuzz smoke: mutate the corpus inputs (and some synthetic soup)
//! with a fixed-seed PRNG and assert the invariants that must hold for ANY input:
//! no panic at any strictness, and the canonical formatter is a fixpoint.
//! Iteration count scales via SHCL_FUZZ_ITERS (cicd raises it; default is quick).

use shcl::{Document, Piece, Quote, Rules, SegTok, Strictness, Tokens, tokenize};

/// Small deterministic PRNG (xorshift64*); no external crates, stable across runs.
struct Rng(u64);

impl Rng {
	fn next(&mut self) -> u64 {
		let mut x = self.0;
		x ^= x >> 12;
		x ^= x << 25;
		x ^= x >> 27;
		self.0 = x;
		x.wrapping_mul(0x2545F4914F6CDD1D)
	}
	fn below(&mut self, n: usize) -> usize {
		(self.next() % n as u64) as usize
	}
}

// The characters the mutator splices in. The whitespace tail past space and tab
// is why it is worth listing them out: the parser trims the whole Unicode
// White_Space set while the emitter quotes from a much shorter list, and a value
// whose edge lands in that gap used to be truncated on reload. With none of
// these in the set, the fuzzer could not reach it - a corpus case had to.
const INTERESTING: &[char] = &[
	':', '[', ']', ',', '#', '"', '\'', '*', '~', '`', '\t', '\n', ' ', '.', '-', '\\', '%', '$',
	'0', '9', 'a', 'Z', '_', 'é', '\u{feff}',
	'\r',       // carriage return: round-trips, but only if nothing eats it
	'\u{0b}',   // vertical tab
	'\u{0c}',   // form feed
	'\u{85}',   // next line
	'\u{a0}',   // no-break space
	'\u{2028}', // line separator
	'\u{3000}', // ideographic space
];

fn mutate(rng: &mut Rng, base: &str) -> String {
	let mut chars: Vec<char> = base.chars().collect();
	let edits = 1 + rng.below(8);
	for _ in 0..edits {
		let kind = rng.below(3);
		let pick = INTERESTING[rng.below(INTERESTING.len())];
		if chars.is_empty() {
			chars.push(pick);
			continue;
		}
		let at = rng.below(chars.len());
		match kind {
			0 => chars.insert(at, pick),
			1 => {
				chars.remove(at);
			}
			_ => chars[at] = pick,
		}
	}
	chars.into_iter().collect()
}

// Line-level shapes the character mutator almost never builds: duplicate keys
// with children under them, a refused line with content beneath it, bracket
// arrays, mixed and staircase indent, comments at every depth, stacked
// elements against fields. Every defect all four bindings shared in the last
// three rounds was one of these, so half the soup is built from them.
fn structural(rng: &mut Rng) -> String {
	const NAMES: &[&str] = &["a", "b", "c", "srv", "\"q.k\"", "*"];
	let mut out = String::new();
	let mut depth = 0usize;
	for _ in 0..(1 + rng.below(16)) {
		// Indent: usually the current or next level, sometimes a jump back,
		// sometimes a space in place of a tab, sometimes one level too deep.
		depth = match rng.below(8) {
			0 => 0,
			1 => depth + 2,
			2 => depth.saturating_sub(1),
			3 | 4 => depth + 1,
			_ => depth,
		};
		let unit = if rng.below(6) == 0 { " " } else { "\t" };
		let indent = unit.repeat(depth);
		let name = NAMES[rng.below(NAMES.len())];
		let sel = match rng.below(6) {
			0 => "[x]",
			1 => "[*]",
			2 => "[#1]",
			_ => "",
		};
		// Shapes 11 to 13 carry a `# k` comment behind a selector holding a
		// quote, a backslash or a quoted `]`; see comments_behind_selectors.
		let line = match rng.below(24) {
			0 => format!("{indent}# comment {}", rng.below(3)),
			1 => String::new(),
			2 => format!("{indent}no colon here"),
			3 => format!("{indent}* {}", rng.below(4)),
			4 => format!("{indent}{name}: [{}, {}]", rng.below(9), rng.below(9)),
			5 => format!("{indent}{name}{sel}:"),
			6 => format!("{indent}{name}.{name}{sel}: {}", rng.below(9)),
			7 => format!("{indent}{name}: ```\n{indent}\tbody\n{indent}```"),
			8 => format!("{indent}{name}: \"open"),
			9 => format!("{indent}{name}: 1, , 2 # trailing"),
			10 => format!("{indent}\u{feff}{name}: 1"),
			11 => format!("{indent}{name}[O'x].{name}: {}  # k", rng.below(9)),
			12 => format!("{indent}{name}[C:\\].{name}: it's  # k"),
			13 => format!("{indent}{name}[ \"q]v\" ].{name}: {}  # k", rng.below(9)),
			// One bracketed value, and the sugar spelling of a selector: neither
			// loses anything, unlike shape 4.
			14 => format!(
				"{indent}{name}:{}[v{}]",
				if rng.below(2) == 0 { " " } else { "" },
				rng.below(3)
			),
			// The 3.0 shapes: a glued `#`, an index selector, bare and
			// single-quoted backslashes, a value right after the colon, a
			// quote that closes with text after it.
			15 => format!("{indent}{name}: x#y  # k"),
			16 => format!("{indent}{name}[#{}].{name}: {}", rng.below(2), rng.below(9)),
			17 => format!("{indent}{name}: C:\\dir, a\\tb, 'it\\'s'"),
			18 => format!("{indent}{name}:#x"),
			19 => format!("{indent}{name}: \"a\" b, c  # k"),
			20 => format!("{indent}{name}['x].{name}: {}  # k", rng.below(9)),
			_ => format!("{indent}{name}{sel}: {}", rng.below(9)),
		};
		out.push_str(&line);
		out.push('\n');
	}
	out
}

/// A short string over the interesting characters: what a setter is handed
/// when a config value comes from somewhere other than a keyboard.
fn soup_text(rng: &mut Rng, max: usize) -> String {
	let len = rng.below(max);
	(0..len)
		.map(|_| INTERESTING[rng.below(INTERESTING.len())])
		.collect()
}

fn seed_texts() -> Vec<String> {
	let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../project/conformance");
	let mut seeds: Vec<String> = Vec::new();
	// A missing corpus must fail loudly: three synthetic seeds are not a fuzz run.
	let entries =
		std::fs::read_dir(&dir).unwrap_or_else(|e| panic!("corpus dir {}: {}", dir.display(), e));
	for entry in entries.flatten() {
		let p = entry.path().join("input.shcl");
		if let Ok(t) = std::fs::read_to_string(&p) {
			seeds.push(t);
		}
	}
	// read_dir order is unspecified, and the seed order drives every mutation
	// the PRNG makes. Sort so a run is actually reproducible.
	seeds.sort();
	seeds.push("a: 1\n\tb: 2\n".to_string());
	seeds.push("x:\n\t* one\n\t* two\n".to_string());
	seeds.push("r:\n\t~~~\n\tbody\n\t~~~\n".to_string());
	seeds
}

#[test]
fn mutated_inputs_never_panic_and_format_is_fixpoint() {
	let iters: usize = std::env::var("SHCL_FUZZ_ITERS")
		.ok()
		.and_then(|v| v.parse().ok())
		.unwrap_or(300);
	let seeds = seed_texts();
	let mut rng = Rng(0x5EED_CAFE_F00D_0001);
	// SHCL_FUZZ_DUMP: also write the generated inputs out (capped), so the cicd
	// cross-binding check can replay the same soup through every binding's CLI.
	let dump_dir = std::env::var("SHCL_FUZZ_DUMP").ok();
	let dump_max: usize = std::env::var("SHCL_FUZZ_DUMP_MAX")
		.ok()
		.and_then(|v| v.parse().ok())
		.unwrap_or(500);
	for i in 0..iters {
		let text = if i % 2 == 0 {
			let base = &seeds[rng.below(seeds.len())];
			mutate(&mut rng, base)
		} else {
			structural(&mut rng)
		};
		if let Some(dir) = &dump_dir
			&& i < dump_max
		{
			let _ = std::fs::write(format!("{}/fuzz_{:05}.shcl", dir, i), &text);
			// Also emit a derived reads.tsv for a small subset, so the differential
			// check exercises the accessor surface (coercion, levels, arrays) over
			// fuzz soup, not just fmt. Capped so the cross-binding replay stays cheap.
			if i < 30 {
				let paths = Document::parse(&text).paths();
				if !paths.is_empty() {
					let mut tsv = String::from("query\ttype\texpected\tstatus\tlevel\n");
					// Quoted segments may carry a literal tab; those cannot ride
					// a tab-separated row, so leave them to the native runners.
					for p in paths.iter().filter(|p| !p.contains('\t')).take(3) {
						for (ty, lvl) in [
							("string", ""),
							("int", "loose"),
							("bool", "strict"),
							("string[]", ""),
						] {
							tsv.push_str(&format!("{}\t{}\t-\t-\t{}\n", p, ty, lvl));
						}
					}
					let _ = std::fs::write(format!("{}/fuzz_{:05}.reads.tsv", dir, i), tsv);
				}
			}
		}
		// Must never panic at any strictness; Strict may (validly) refuse the load.
		let _ = Document::parse_with(&text, Strictness::Loose);
		let _ = Document::parse_with(&text, Strictness::Strict);
		let doc = Document::parse(&text);
		// A few reads over mutated soup must not panic either.
		let _ = doc.read_int("a.b");
		let _ = doc.read_string_array("x");
		let _ = doc.count("r");
		// The formatter must be a fixpoint on its own output.
		let once = doc.to_canonical();
		let twice = Document::parse(&once).to_canonical();
		assert_eq!(
			twice, once,
			"formatter not idempotent at iteration {} for mutated input:\n{}",
			i, text
		);
	}
}

/// A write on structural soup must leave a formatter fixpoint: the corpus
/// pins this for a handful of documents, and the one fold defect that broke it
/// (duplicates folded one level and no deeper) was invisible to every
/// value-level check because reads did not change.
#[test]
fn writes_on_structural_soup_stay_fixpoint() {
	let iters: usize = std::env::var("SHCL_FUZZ_ITERS")
		.ok()
		.and_then(|v| v.parse().ok())
		.unwrap_or(300);
	let mut rng = Rng(0x5EED_57A7_1C00_0002);
	for i in 0..iters {
		let text = structural(&mut rng);
		let mut doc = Document::parse(&text);
		let paths = doc.paths();
		let path = if paths.is_empty() || rng.below(4) == 0 {
			format!("new{}.k", rng.below(3))
		} else {
			paths[rng.below(paths.len())].clone()
		};
		// The values come off the soup too, so a setter meets the same text the
		// parser does. Every setter answers false either for a path it could
		// not write - a wildcard or a missing instance among the enumerated
		// paths - or for text it could not write back.
		let before = doc.to_canonical();
		let v = soup_text(&mut rng, 7);
		let applied = match rng.below(8) {
			0 => doc.set_int(&path, 7),
			1 => doc.set_string(&path, &v),
			2 => doc.remove(&path) > 0,
			3 => doc.set_int_default(&path, 1),
			4 => doc.set_empty(&path),
			5 => doc.set_comment(&path, &v),
			6 => doc.set_literal(&path, &v),
			_ => doc.set_raw(&path, &v, &soup_text(&mut rng, 7)),
		};
		if !applied {
			assert_eq!(
				doc.to_canonical(),
				before,
				"a refused write changed the document at iteration {} (path {:?}, value {:?})",
				i,
				path,
				v
			);
		}
		let once = doc.to_canonical();
		let twice = Document::parse(&once).to_canonical();
		assert_eq!(
			twice, once,
			"write on structural soup not a fixpoint at iteration {} (path {:?}):\n{}",
			i, path, text
		);
	}
}

/// A comment behind a selector stays a comment. A quote, a backslash or a
/// quoted `]` in a selector used to leave the name-half scan in the wrong
/// state, so the `#` after it was read as value text with zero diagnostics,
/// and the write that followed was a fixpoint, so nothing else could see it.
/// The structural shapes that carry `# k` are the only source of that text; a
/// swallowed one comes back quoted (`port: "8080  # k"`), so a canonical line
/// holding it has to end with it.
#[test]
fn comments_behind_selectors_stay_comments() {
	let iters: usize = std::env::var("SHCL_FUZZ_ITERS")
		.ok()
		.and_then(|v| v.parse().ok())
		.unwrap_or(300);
	let mut rng = Rng(0x5EED_57A7_1C00_0003);
	let mut seen = 0usize;
	for i in 0..iters {
		let text = structural(&mut rng);
		let canon = Document::parse(&text).to_canonical();
		for line in canon.lines().filter(|l| l.contains("# k")) {
			seen += 1;
			assert!(
				line.ends_with("# k"),
				"comment read as value at iteration {}: {:?}\n{}",
				i,
				line,
				text
			);
		}
	}
	assert!(
		seen > iters / 4,
		"the soup carried only {} commented lines",
		seen
	);
}

/// The lost count follows from the diagnostics alone: each code has one
/// outcome (the table in design.md), and lost is the number of dropped or
/// value-dropped lines, so it is zero exactly when no diagnostic says a line
/// was dropped. Nine review items were an arm that counted without saying so
/// or said so without counting; both show here. The retained half is checked
/// by content: a retained line's text comes back in the canonical output.
#[test]
fn lost_count_follows_the_outcome_table() {
	let iters: usize = std::env::var("SHCL_FUZZ_ITERS")
		.ok()
		.and_then(|v| v.parse().ok())
		.unwrap_or(300);
	let mut rng = Rng(0x5EED_57A7_1C00_0004);
	let (mut lost_seen, mut kept_seen) = (0usize, 0usize);
	for i in 0..iters {
		let text = structural(&mut rng);
		let doc = Document::parse(&text);
		// The parser strips a file-start BOM before it sees line 1; the
		// exception is for a BOM the strip does not reach.
		let lines: Vec<&str> = text
			.strip_prefix('\u{feff}')
			.unwrap_or(&text)
			.lines()
			.collect();
		let canon = doc.to_canonical();
		let mut want = 0usize;
		for d in doc.diagnostics() {
			let src = lines.get(d.line.wrapping_sub(1)).copied().unwrap_or("");
			match d.code {
				"E002" | "E003" | "E004" | "E006" | "E007" | "E008" | "E009" | "E010" | "E011"
				| "E012" | "E016" | "E018" | "E021" => want += 1,
				"E014" if src.trim_start_matches([' ', '\t']).starts_with('\u{feff}') => want += 1,
				"E013" | "E014" | "E019" => {
					kept_seen += 1;
					let kept = src.trim_matches([' ', '\t']);
					assert!(
						canon.lines().any(|l| l.trim_matches([' ', '\t']) == kept),
						"retained line {} not written back at iteration {}: {:?}\n{}",
						d.line,
						i,
						kept,
						text
					);
				}
				_ => {}
			}
		}
		assert_eq!(
			doc.lost_count(),
			want,
			"lost count disagrees with the diagnostics at iteration {}:\n{}",
			i,
			text
		);
		lost_seen += want;
	}
	assert!(
		lost_seen > iters,
		"the soup dropped only {} lines",
		lost_seen
	);
	assert!(
		kept_seen > iters / 8,
		"the soup retained only {} lines",
		kept_seen
	);
}

/// Layered merge over mutated soup: overlaying one document on another must
/// never panic and the merged result must be a formatter fixpoint - the same
/// guarantee `fmt` gives, now for the composed document.
#[test]
fn merge_never_panics_and_stays_fixpoint() {
	let iters: usize = std::env::var("SHCL_FUZZ_ITERS")
		.ok()
		.and_then(|v| v.parse().ok())
		.unwrap_or(300);
	let seeds = seed_texts();
	let mut rng = Rng(0x5EED_CAFE_F00D_0007);
	for i in 0..iters {
		let a_i = rng.below(seeds.len());
		let a = mutate(&mut rng, &seeds[a_i]);
		let b_i = rng.below(seeds.len());
		let b = mutate(&mut rng, &seeds[b_i]);
		let mut doc = Document::parse(&a);
		doc.merge(&Document::parse(&b));
		let once = doc.to_canonical();
		let twice = Document::parse(&once).to_canonical();
		assert_eq!(
			twice, once,
			"merged output not idempotent at iteration {} for:\nA:\n{}\nB:\n{}",
			i, a, b
		);
		// Onto an empty base a merge is the identity: nothing to match, so
		// every node and every footer line comes across in file order.
		let mut empty = Document::new();
		empty.merge(&Document::parse(&b));
		assert_eq!(
			empty.to_canonical(),
			Document::parse(&b).to_canonical(),
			"merge onto empty base is not the identity at iteration {} for:\n{}",
			i,
			b
		);
		// Reads answered by the merged document itself, not just its text. A
		// merged arena holds dropped nodes, a rebuilt index and cloned child
		// lists, and only a read walks those; the text compare above cannot
		// see them. Every eighth iteration, since it walks every path.
		//
		// `instances` is left out on purpose: it hands back the SOURCE
		// spelling, and canonical output legitimately respells a value -
		// escaping a quote to keep it on one line, say - so the two differ for
		// a reason that has nothing to do with merging.
		if i % 8 == 0 {
			let back = Document::parse(&once);
			assert_eq!(doc.paths(), back.paths(), "paths differ at iteration {}", i);
			for p in doc.paths() {
				assert_eq!(doc.count(&p), back.count(&p), "count {:?} at {}", p, i);
				assert_eq!(
					doc.children(&p),
					back.children(&p),
					"children {:?} at {}",
					p,
					i
				);
				let (x, y) = (doc.read_string(&p), back.read_string(&p));
				assert_eq!(
					(x.value, x.status),
					(y.value, y.status),
					"read {:?} at {}",
					p,
					i
				);
				let (x, y) = (doc.read_string_array(&p), back.read_string_array(&p));
				assert_eq!(
					(x.value, x.status, x.slots),
					(y.value, y.status, y.slots),
					"array read {:?} at {}",
					p,
					i
				);
			}
		}
		// A layer and its canonical form must merge the same: a load that
		// keeps a bit its own emitter cannot re-emit makes the fold depend on
		// whether the caller formatted the layer first.
		let mut from_text = Document::parse(&a);
		from_text.merge(&Document::parse(&b));
		let mut from_canon = Document::parse(&a);
		from_canon.merge(&Document::parse(&Document::parse(&b).to_canonical()));
		assert_eq!(
			from_canon.to_canonical(),
			from_text.to_canonical(),
			"merging a layer differs from merging its canonical form at iteration {} for:\nA:\n{}\nB:\n{}",
			i,
			a,
			b
		);
	}
}

/// Writer round-trip: a set_string value must read back verbatim (encode is the
/// exact inverse of the string read), survive emit + reparse, and leave the
/// document a formatter fixpoint - even for the reserved/escape/fence hazards.
#[test]
fn writer_roundtrips_and_stays_fixpoint() {
	let iters: usize = std::env::var("SHCL_FUZZ_ITERS")
		.ok()
		.and_then(|v| v.parse().ok())
		.unwrap_or(300);
	let mut rng = Rng(0x5EED_0000_1234_ABCD);
	for i in 0..iters {
		let s = soup_text(&mut rng, 12);
		let mut d = Document::new();
		assert!(d.set_string("k", &s));
		// In-memory: encode is the exact inverse of the scalar string read.
		let mem = d.read_string("k");
		assert_eq!(mem.value, s, "in-memory set/read #{} for {:?}", i, s);
		// Through emit + reparse: the value survives quoting/escaping intact.
		let text = d.to_canonical();
		let rt = Document::parse(&text).read_string("k");
		assert_eq!(rt.value, s, "reparse round-trip #{} for {:?}", i, s);
		assert_eq!(
			Document::parse(&text).to_canonical(),
			text,
			"writer output not a fixpoint #{} for {:?}",
			i,
			s
		);
		// The written document and its own reload agree on the source spelling
		// too, not just on the value: a text carrying both quote kinds used to
		// store one form and reparse as another.
		assert_eq!(
			d.instances("k"),
			Document::parse(&text).instances("k"),
			"instances differ between a written document and its reload #{} for {:?}",
			i,
			s
		);
		// Array form: each element unquotes/unescapes back to itself.
		let b = soup_text(&mut rng, 12);
		let mut da = Document::new();
		assert!(da.set_string_array("k", &[s.as_str(), b.as_str()]));
		let ra = Document::parse(&da.to_canonical()).read_string_array("k");
		assert_eq!(
			ra.value,
			vec![s.clone(), b.clone()],
			"array round-trip #{}",
			i
		);
	}
}

/// Lines built from the grammar with their spans known as they are laid
/// down, so the tokenizer has an oracle outside itself: the four bindings
/// agreeing on `tokens` proves parity, and this is what proves the spans are
/// the grammar's. Every piece kind the grammar has is drawn here: bare and
/// quoted names, bare and quoted selector bodies, bare, quoted, empty and
/// open elements, whitespace wherever the grammar allows it, a glued `#`
/// and a comment, non-ASCII text.
#[test]
fn tokens_follow_the_grammar() {
	let iters: usize = std::env::var("SHCL_FUZZ_ITERS")
		.ok()
		.and_then(|v| v.parse().ok())
		.unwrap_or(300)
		.max(2000);
	let mut rng = Rng(0x5EED_70CE_0000_0005);
	let mut tok = Tokens::default();
	for i in 0..iters {
		let (line, want) = grammar_line(&mut rng);
		tokenize(&line, b':', false, Rules::Current, &mut tok);
		assert_eq!(tok, want, "iteration {i}: {line:?}");
	}
}

struct LineGen {
	text: String,
	want: Tokens,
}

impl LineGen {
	fn wsp(&mut self, rng: &mut Rng) {
		self.text.push_str(["", " ", "\t", "  "][rng.below(4)]);
	}
	fn pick(&mut self, rng: &mut Rng, set: &[&str], n: usize) {
		for _ in 0..n {
			self.text.push_str(set[rng.below(set.len())]);
		}
	}
	/// A quoted piece: the quote, content that cannot close it, the quote. No
	/// `]` inside either, for the reason bare() gives.
	fn quoted(&mut self, rng: &mut Rng) -> Piece {
		let double = rng.below(2) == 0;
		let q = if double { '"' } else { '\'' };
		self.text.push(q);
		let start = self.text.len();
		// Inside double quotes a backslash escapes the next character, so an
		// escaped quote stays inside; inside single quotes a backslash is a
		// character and only the quote itself is off limits.
		let set: &[&str] = if double {
			&[
				"a", "Z", " ", "\\\"", "\\\\", "'", "#", ",", "[", ":", "\u{e9}", "\\a",
			]
		} else {
			&[
				"a", "Z", " ", "\"", "\\", "#", ",", "[", ":", "\u{e9}", "\\\\",
			]
		};
		// The first content character is a letter, and a quote of the other
		// kind inside is followed by one: a comma or a blank after a quote
		// would let an open piece earlier on the line close on it.
		self.pick(rng, &["a", "Z"], 1);
		for _ in 0..rng.below(5) {
			let c = set[rng.below(set.len())];
			self.text.push_str(c);
			if c == "'" || c == "\"" {
				self.text.push('a');
			}
		}
		let end = self.text.len();
		self.text.push(q);
		Piece {
			start,
			end,
			quote: if double { Quote::Double } else { Quote::Single },
		}
	}
	/// A bare piece for a value or a selector body: no comma, no bracket,
	/// no leading quote, a `#` only glued to the text before it, no edge
	/// whitespace. `open` makes it start with a quote it never closes the
	/// quoted way. No `]` at all, and no quote as a bare piece's last
	/// character: a quote that opens a piece closes at the next matching
	/// quote when that one sits right before the piece's terminator,
	/// wherever on the line it is, so those two shapes would hand an open
	/// piece a closing quote from a later one. That reading is the rule, not
	/// a defect; the generator just keeps to lines with one reading.
	fn bare(&mut self, rng: &mut Rng, term: char, open: bool) -> Piece {
		let start = self.text.len();
		let mut set: Vec<&str> = vec![
			"a", "Z", "-", "_", ".", ":", "\\", "\u{e9}", "'", "\"", "[", "#",
		];
		set.retain(|c| !c.starts_with(term));
		if open {
			// The quote either never closes or closes with text after it;
			// either way the tokenizer reads the piece bare.
			let q = if rng.below(2) == 0 { "\"" } else { "'" };
			self.text.push_str(q);
			set.retain(|c| c != &q);
			let n = 1 + rng.below(3);
			self.pick(rng, &set, n);
			if rng.below(2) == 0 {
				self.text.push_str(q);
				self.text.push_str(" b");
			}
		} else {
			// First character: neither a quote nor a bracket nor `#`.
			self.pick(rng, &["a", "Z", "-", "_", ".", ":", "\\", "\u{e9}"], 1);
			for _ in 0..rng.below(4) {
				// A space is fine mid-piece, but never right before a `#`.
				let c = set[rng.below(set.len())];
				if rng.below(5) == 0 && c != "#" {
					self.text.push(' ');
				}
				self.text.push_str(c);
			}
		}
		if self.text.ends_with(['\'', '"']) {
			self.text.push('a');
		}
		Piece {
			start,
			end: self.text.len(),
			quote: if open { Quote::Open } else { Quote::None },
		}
	}
	fn segment(&mut self, rng: &mut Rng) -> SegTok {
		let name = if rng.below(3) == 0 {
			self.quoted(rng)
		} else {
			let start = self.text.len();
			let n = 1 + rng.below(4);
			self.pick(rng, &["a", "Z", "0", "-", "_"], n);
			Piece {
				start,
				end: self.text.len(),
				quote: Quote::None,
			}
		};
		let mut selector = None;
		if rng.below(2) == 0 {
			self.wsp(rng);
			self.text.push('[');
			self.wsp(rng);
			let body = match rng.below(4) {
				0 => self.quoted(rng),
				1 => self.bare(rng, ']', true),
				_ => self.bare(rng, ']', false),
			};
			self.wsp(rng);
			self.text.push(']');
			selector = Some(body);
		}
		SegTok {
			name,
			selector,
			star: false,
		}
	}
}

fn grammar_line(rng: &mut Rng) -> (String, Tokens) {
	let mut g = LineGen {
		text: String::new(),
		want: Tokens::default(),
	};
	let nseg = 1 + rng.below(3);
	for i in 0..nseg {
		if i > 0 {
			g.wsp(rng);
			g.text.push('.');
		}
		g.wsp(rng);
		let seg = g.segment(rng);
		g.want.segments.push(seg);
	}
	g.wsp(rng);
	match rng.below(4) {
		0 => {}
		1 => {
			// A comment right after the path: only after whitespace.
			g.text.push_str(" # c");
			g.want.comment = Some(g.text.len() - 3);
		}
		_ => {
			g.want.sep = Some(g.text.len());
			g.text.push(':');
			let npieces = rng.below(4);
			if npieces == 0 {
				g.wsp(rng);
				let at = g.text.len();
				g.want.elements.push(Piece {
					start: at,
					end: at,
					quote: Quote::None,
				});
			}
			for j in 0..npieces {
				if j > 0 {
					g.wsp(rng);
					g.text.push(',');
				}
				g.wsp(rng);
				let piece = match rng.below(5) {
					0 => {
						let at = g.text.len();
						Piece {
							start: at,
							end: at,
							quote: Quote::None,
						}
					}
					1 => g.quoted(rng),
					2 => g.bare(rng, ',', true),
					_ => g.bare(rng, ',', false),
				};
				g.want.elements.push(piece);
			}
			// The value runs from its first piece (opening quote included) to
			// after its last non-blank character, comma or closing quote
			// included; an empty value is a point.
			let end = g.text.trim_end_matches([' ', '\t']).len();
			if rng.below(2) == 0 {
				g.text.push_str("  # c");
				g.want.comment = Some(g.text.len() - 3);
			}
			// An empty piece sits where the scan gave up on it: at the comma,
			// the comment or the end that follows the blank, not at the blank.
			for p in &mut g.want.elements {
				if p.quote == Quote::None && p.start == p.end {
					let rest = &g.text[p.start..];
					p.start += rest.len() - rest.trim_start_matches([' ', '\t']).len();
					p.end = p.start;
				}
			}
			let first = g.want.elements[0];
			let start = match first.quote {
				Quote::Single | Quote::Double => first.start - 1,
				_ => first.start,
			};
			g.want.value = (start, end.max(start));
		}
	}
	(g.text, g.want)
}
