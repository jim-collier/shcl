// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

//! Conformance-corpus runner. Every shipped binding must pass this corpus;
//! the Rust reference runs it natively here. Case layout and reads.tsv column
//! meanings are documented in project/conformance/README.md.

use shcl::{Document, Strictness};
use std::path::{Path, PathBuf};

fn corpus_dir() -> PathBuf {
	Path::new(env!("CARGO_MANIFEST_DIR")).join("../../project/conformance")
}

/// TSV-safe form: real newlines/tabs in a value are written \n / \t.
fn tsv_escape(s: &str) -> String {
	s.replace('\n', "\\n").replace('\t', "\\t")
}

fn parse_level(s: Option<&str>) -> Strictness {
	match s {
		None | Some("") | Some("standard") => Strictness::Standard,
		Some("loose") => Strictness::Loose,
		Some("strict") => Strictness::Strict,
		Some(other) => panic!("unknown level '{}' in reads.tsv", other),
	}
}

struct Case {
	name: String,
	input: String,
	expected_fmt: String,
	reads: String,
}

fn load_cases() -> Vec<Case> {
	let dir = corpus_dir();
	let mut cases: Vec<Case> = Vec::new();
	let entries =
		std::fs::read_dir(&dir).unwrap_or_else(|e| panic!("corpus dir {}: {}", dir.display(), e));
	for entry in entries {
		let path = entry.unwrap().path();
		if !path.is_dir() {
			continue;
		}
		let input = path.join("input.shcl");
		if !input.exists() {
			continue;
		}
		cases.push(Case {
			name: path.file_name().unwrap().to_string_lossy().into_owned(),
			input: std::fs::read_to_string(&input).unwrap(),
			expected_fmt: std::fs::read_to_string(path.join("expected.shcl")).unwrap(),
			reads: std::fs::read_to_string(path.join("reads.tsv")).unwrap(),
		});
	}
	cases.sort_by(|a, b| a.name.cmp(&b.name));
	assert!(
		!cases.is_empty(),
		"no corpus cases found under {}",
		dir.display()
	);
	cases
}

fn doc_for(case: &Case, level: Strictness) -> Document {
	Document::parse_with(&case.input, level).unwrap_or_else(|e| {
		panic!(
			"{}: load failed at {:?} but reads.tsv has reads there: {}",
			case.name, level, e
		)
	})
}

#[test]
fn canonical_format_matches_expected() {
	for case in load_cases() {
		let got = Document::parse(&case.input).to_canonical();
		assert_eq!(
			got, case.expected_fmt,
			"{}: canonical output differs from expected.shcl",
			case.name
		);
		// The formatter must be a fixpoint: canonicalizing its own output changes nothing.
		let again = Document::parse(&got).to_canonical();
		assert_eq!(again, got, "{}: formatter is not idempotent", case.name);
	}
}

#[test]
fn reads_match_expected() {
	for case in load_cases() {
		for (n, line) in case.reads.lines().enumerate() {
			if n == 0 || line.trim().is_empty() {
				continue; // header
			}
			let cols: Vec<&str> = line.split('\t').collect();
			assert!(
				cols.len() >= 4,
				"{}: reads.tsv line {} too short",
				case.name,
				n + 1
			);
			let (query, kind, expected, status) = (cols[0], cols[1], cols[2], cols[3]);
			let level = parse_level(cols.get(4).copied());
			let at = format!(
				"{}: reads.tsv line {} ({} {})",
				case.name,
				n + 1,
				query,
				kind
			);

			if kind == "load" {
				let ok = Document::parse_with(&case.input, level).is_ok();
				let want = match expected {
					"ok" => true,
					"fail" => false,
					other => panic!("{}: bad load expectation '{}'", at, other),
				};
				assert_eq!(ok, want, "{}: load outcome", at);
				continue;
			}

			let doc = doc_for(&case, level);
			if kind == "count" {
				let want: usize = expected
					.parse()
					.unwrap_or_else(|_| panic!("{}: bad count", at));
				assert_eq!(doc.count(query), want, "{}", at);
				continue;
			}
			if kind == "instances" {
				let got = doc.instances(query).join("|");
				assert_eq!(got, expected, "{}", at);
				continue;
			}

			let (got_value, got_status): (String, shcl::Status) = match kind {
				"int" => {
					let r = doc.read_int(query);
					(r.value.to_string(), r.status)
				}
				"float" => {
					let r = doc.read_float(query);
					(r.value.to_string(), r.status)
				}
				"bool" => {
					let r = doc.read_bool(query);
					(r.value.to_string(), r.status)
				}
				"datetime" => {
					let r = doc.read_datetime(query);
					(r.value.to_string(), r.status)
				}
				"string" => {
					let r = doc.read_string(query);
					(tsv_escape(&r.value), r.status)
				}
				"raw" => {
					let r = doc.read_raw(query);
					(tsv_escape(&r.value), r.status)
				}
				"int[]" => {
					let r = doc.read_int_array(query);
					(
						r.value
							.iter()
							.map(|v| v.to_string())
							.collect::<Vec<_>>()
							.join("|"),
						r.status,
					)
				}
				"float[]" => {
					let r = doc.read_float_array(query);
					(
						r.value
							.iter()
							.map(|v| v.to_string())
							.collect::<Vec<_>>()
							.join("|"),
						r.status,
					)
				}
				"bool[]" => {
					let r = doc.read_bool_array(query);
					(
						r.value
							.iter()
							.map(|v| v.to_string())
							.collect::<Vec<_>>()
							.join("|"),
						r.status,
					)
				}
				"datetime[]" => {
					let r = doc.read_datetime_array(query);
					(
						r.value
							.iter()
							.map(|v| v.to_string())
							.collect::<Vec<_>>()
							.join("|"),
						r.status,
					)
				}
				"string[]" => {
					let r = doc.read_string_array(query);
					(
						r.value
							.iter()
							.map(|v| tsv_escape(v))
							.collect::<Vec<_>>()
							.join("|"),
						r.status,
					)
				}
				other => panic!("{}: unknown type '{}'", at, other),
			};
			assert_eq!(format!("{:?}", got_status), status, "{}: status", at);
			if expected != "-" {
				assert_eq!(got_value, expected, "{}: value", at);
			}
		}
	}
}
