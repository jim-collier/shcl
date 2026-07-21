// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

//! Conformance-corpus runner. Every shipped binding must pass this corpus;
//! the Rust reference runs it natively here. Case layout and reads.tsv column
//! meanings are documented in project/conformance/README.md.

use shcl::{Document, Strictness, parse_datetime};
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
	// Write dimension (optional): an ops script and its golden canonical output.
	write_ops: Option<String>,
	expected_write: Option<String>,
}

/// Decode an ops value: \n \t \\ only, others verbatim (mirrors the CLI).
fn unescape_ops(s: &str) -> String {
	let mut out = String::with_capacity(s.len());
	let mut it = s.chars();
	while let Some(c) = it.next() {
		if c != '\\' {
			out.push(c);
			continue;
		}
		match it.next() {
			Some('n') => out.push('\n'),
			Some('t') => out.push('\t'),
			Some('\\') => out.push('\\'),
			Some(other) => {
				out.push('\\');
				out.push(other);
			}
			None => out.push('\\'),
		}
	}
	out
}

/// Apply one write-ops line to the document via the library Writer.
fn apply_op(doc: &mut Document, line: &str, at: &str) {
	let f: Vec<&str> = line.split('\t').collect();
	let path = f.get(1).copied().unwrap_or("");
	let v = f.get(2).copied().unwrap_or("");
	let ints = |xs: &[&str]| {
		xs.iter()
			.map(|s| s.parse::<i64>().unwrap())
			.collect::<Vec<_>>()
	};
	let flts = |xs: &[&str]| {
		xs.iter()
			.map(|s| s.parse::<f64>().unwrap())
			.collect::<Vec<_>>()
	};
	let bools = |xs: &[&str]| xs.iter().map(|s| *s == "true").collect::<Vec<_>>();
	let dt = |s: &str| parse_datetime(s).unwrap_or_else(|| panic!("{}: bad datetime {}", at, s));
	let arr = &f[2.min(f.len())..];
	match f.first().copied().unwrap_or("") {
		"int" => doc.set_int(path, v.parse().unwrap()),
		"float" => doc.set_float(path, v.parse().unwrap()),
		"bool" => doc.set_bool(path, v == "true"),
		"string" => doc.set_string(path, &unescape_ops(v)),
		"datetime" => doc.set_datetime(path, &dt(v)),
		"int-default" => doc.set_int_default(path, v.parse().unwrap()),
		"float-default" => doc.set_float_default(path, v.parse().unwrap()),
		"bool-default" => doc.set_bool_default(path, v == "true"),
		"string-default" => doc.set_string_default(path, &unescape_ops(v)),
		"datetime-default" => doc.set_datetime_default(path, &dt(v)),
		"int-array" => doc.set_int_array(path, &ints(arr)),
		"float-array" => doc.set_float_array(path, &flts(arr)),
		"bool-array" => doc.set_bool_array(path, &bools(arr)),
		"string-array" => {
			let owned: Vec<String> = arr.iter().map(|s| unescape_ops(s)).collect();
			doc.set_string_array(path, &owned.iter().map(|s| s.as_str()).collect::<Vec<_>>());
		}
		"datetime-array" => {
			doc.set_datetime_array(path, &arr.iter().map(|s| dt(s)).collect::<Vec<_>>())
		}
		"int-array-default" => doc.set_int_array_default(path, &ints(arr)),
		"float-array-default" => doc.set_float_array_default(path, &flts(arr)),
		"bool-array-default" => doc.set_bool_array_default(path, &bools(arr)),
		"string-array-default" => {
			let owned: Vec<String> = arr.iter().map(|s| unescape_ops(s)).collect();
			doc.set_string_array_default(
				path,
				&owned.iter().map(|s| s.as_str()).collect::<Vec<_>>(),
			);
		}
		"datetime-array-default" => {
			doc.set_datetime_array_default(path, &arr.iter().map(|s| dt(s)).collect::<Vec<_>>())
		}
		"raw" => doc.set_raw(path, &unescape_ops(f.get(3).copied().unwrap_or("")), v),
		"raw-default" => {
			doc.set_raw_default(path, &unescape_ops(f.get(3).copied().unwrap_or("")), v)
		}
		"empty" => doc.set_empty(path),
		"comment" => doc.set_comment(path, v),
		"remove" => {
			doc.remove(path);
		}
		other => panic!("{}: unknown op '{}'", at, other),
	}
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
		let read_opt = |name: &str| std::fs::read_to_string(path.join(name)).ok();
		cases.push(Case {
			name: path.file_name().unwrap().to_string_lossy().into_owned(),
			input: std::fs::read_to_string(&input).unwrap(),
			expected_fmt: std::fs::read_to_string(path.join("expected.shcl")).unwrap(),
			reads: std::fs::read_to_string(path.join("reads.tsv")).unwrap(),
			write_ops: read_opt("write.ops"),
			expected_write: read_opt("expected-write.shcl"),
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

			let (got_value, got_status, got_slots): (String, shcl::Status, Vec<shcl::Status>) =
				match kind {
					"int" => {
						let r = doc.read_int(query);
						(r.value.to_string(), r.status, r.slots)
					}
					"float" => {
						let r = doc.read_float(query);
						(r.value.to_string(), r.status, r.slots)
					}
					"bool" => {
						let r = doc.read_bool(query);
						(r.value.to_string(), r.status, r.slots)
					}
					"datetime" => {
						let r = doc.read_datetime(query);
						(r.value.to_string(), r.status, r.slots)
					}
					"string" => {
						let r = doc.read_string(query);
						(tsv_escape(&r.value), r.status, r.slots)
					}
					"raw" => {
						let r = doc.read_raw(query);
						(tsv_escape(&r.value), r.status, r.slots)
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
							r.slots,
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
							r.slots,
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
							r.slots,
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
							r.slots,
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
							r.slots,
						)
					}
					other => panic!("{}: unknown type '{}'", at, other),
				};
			assert_eq!(format!("{:?}", got_status), status, "{}: status", at);
			if expected != "-" {
				assert_eq!(got_value, expected, "{}: value", at);
			}
			// Optional 6th column: per-slot statuses, |-joined (needs col 5 set).
			if let Some(want_slots) = cols.get(5) {
				let got = got_slots
					.iter()
					.map(|s| format!("{:?}", s))
					.collect::<Vec<_>>()
					.join("|");
				assert_eq!(&got, want_slots, "{}: slots", at);
			}
		}
	}
}

#[test]
fn write_ops_match_expected() {
	for case in load_cases() {
		let (ops, want) = match (&case.write_ops, &case.expected_write) {
			(Some(o), Some(w)) => (o, w),
			(None, None) => continue,
			_ => panic!(
				"{}: write.ops and expected-write.shcl must come as a pair",
				case.name
			),
		};
		// Base doc loads at Standard; ops build/edit it via the library Writer.
		let mut doc = Document::parse(&case.input);
		for (n, line) in ops.lines().enumerate() {
			if line.is_empty() || line.starts_with('#') {
				continue;
			}
			apply_op(
				&mut doc,
				line,
				&format!("{}: write.ops line {}", case.name, n + 1),
			);
		}
		let got = doc.to_canonical();
		assert_eq!(
			&got, want,
			"{}: writer output differs from expected-write.shcl",
			case.name
		);
		// The written doc must be a formatter fixpoint like any canonical output.
		let again = Document::parse(&got).to_canonical();
		assert_eq!(
			again, got,
			"{}: written output is not a fmt fixpoint",
			case.name
		);
	}
}
