// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

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

const INTERESTING: &[char] = &[
	':', '[', ']', ',', '#', '"', '\'', '*', '~', '`', '\t', '\n', ' ', '.', '-', '\\', '%', '$',
	'0', '9', 'a', 'Z', '_', 'é', '\u{feff}',
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

fn seed_texts() -> Vec<String> {
	let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../project/conformance");
	let mut seeds: Vec<String> = Vec::new();
	if let Ok(entries) = std::fs::read_dir(&dir) {
		for entry in entries.flatten() {
			let p = entry.path().join("input.shcl");
			if let Ok(t) = std::fs::read_to_string(&p) {
				seeds.push(t);
			}
		}
	}
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
	for i in 0..iters {
		let base = &seeds[rng.below(seeds.len())];
		let text = mutate(&mut rng, base);
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
