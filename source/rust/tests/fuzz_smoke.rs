// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

//! Deterministic fuzz smoke: mutate the corpus inputs (and some synthetic soup)
//! with a fixed-seed PRNG and assert the invariants that must hold for ANY input:
//! no panic at any strictness, and the canonical formatter is a fixpoint.
//! Iteration count scales via SHCL_FUZZ_ITERS (cicd raises it; default is quick).

use shcl::{Document, Strictness};

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
		let line = match rng.below(17) {
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
			_ => format!("{indent}{name}{sel}: {}", rng.below(9)),
		};
		out.push_str(&line);
		out.push('\n');
	}
	out
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
		// Every setter answers false only for a path it could not write, which
		// a wildcard or a missing instance among the enumerated paths can be.
		let _ = match rng.below(7) {
			0 => doc.set_int(&path, 7),
			1 => doc.set_string(&path, "v w"),
			2 => doc.remove(&path) > 0,
			3 => doc.set_int_default(&path, 1),
			4 => doc.set_empty(&path),
			5 => doc.set_comment(&path, "note"),
			_ => doc.set_raw(&path, "line 1\n  line 2", "txt"),
		};
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
	let rand_str = |rng: &mut Rng| -> String {
		let len = rng.below(12);
		(0..len)
			.map(|_| INTERESTING[rng.below(INTERESTING.len())])
			.collect()
	};
	for i in 0..iters {
		let s = rand_str(&mut rng);
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
		let b = rand_str(&mut rng);
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
