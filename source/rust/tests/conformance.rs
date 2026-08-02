// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

//! Conformance-corpus runner. Every shipped binding must pass this corpus;
//! the Rust reference runs it natively here. Case layout and reads.tsv column
//! meanings are documented in project/conformance/README.md.

use shcl::{Document, Strictness, generate, parse_datetime, quote_segment};
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
	// Golden `check` stdout at Standard: diag lines (line/severity/code) + summary.
	expected_diags: String,
	// Write dimension (optional): an ops script and its golden canonical output.
	write_ops: Option<String>,
	expected_write: Option<String>,
	// Bad-op dimension (optional): ops that must each be rejected, applied alone.
	write_bad_ops: Option<String>,
	// Schema dimension (optional): a schema and the golden `check --schema` stdout.
	schema: Option<String>,
	expected_validate: Option<String>,
	// Layered-load dimension (optional): lower-priority layer files (in filename
	// order), optional `path=value` overrides, and the golden merged canonical.
	layers: Vec<String>,
	merge_sets: Option<String>,
	expected_merged: Option<String>,
	// Generation dimension (optional): a schema and the golden `init` output.
	init_schema: Option<String>,
	expected_init: Option<String>,
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

/// Apply one write-ops line via the library Writer, with the same value gates
/// the CLI applies. Err = the op must be rejected (bad value or unusable path).
fn try_apply_op(doc: &mut Document, line: &str) -> Result<(), String> {
	let f: Vec<&str> = line.split('\t').collect();
	let path = f.get(1).copied().unwrap_or("");
	let v = f.get(2).copied().unwrap_or("");
	let pint = |s: &str| s.parse::<i64>().map_err(|_| format!("bad int: {}", s));
	let pflt = |s: &str| s.parse::<f64>().map_err(|_| format!("bad float: {}", s));
	let ints = |xs: &[&str]| xs.iter().map(|s| pint(s)).collect::<Result<Vec<_>, _>>();
	let flts = |xs: &[&str]| xs.iter().map(|s| pflt(s)).collect::<Result<Vec<_>, _>>();
	let bools = |xs: &[&str]| xs.iter().map(|s| *s == "true").collect::<Vec<_>>();
	let dt = |s: &str| parse_datetime(s).ok_or_else(|| format!("bad datetime: {}", s));
	let arr = &f[2.min(f.len())..];
	let wrote = match f.first().copied().unwrap_or("") {
		"int" => doc.set_int(path, pint(v)?),
		"float" => doc.set_float(path, pflt(v)?),
		"bool" => doc.set_bool(path, v == "true"),
		"string" => doc.set_string(path, &unescape_ops(v)),
		"datetime" => doc.set_datetime(path, &dt(v)?),
		"int-default" => doc.set_int_default(path, pint(v)?),
		"float-default" => doc.set_float_default(path, pflt(v)?),
		"bool-default" => doc.set_bool_default(path, v == "true"),
		"string-default" => doc.set_string_default(path, &unescape_ops(v)),
		"datetime-default" => doc.set_datetime_default(path, &dt(v)?),
		"int-array" => doc.set_int_array(path, &ints(arr)?),
		"float-array" => doc.set_float_array(path, &flts(arr)?),
		"bool-array" => doc.set_bool_array(path, &bools(arr)),
		"string-array" => {
			let owned: Vec<String> = arr.iter().map(|s| unescape_ops(s)).collect();
			doc.set_string_array(path, &owned.iter().map(|s| s.as_str()).collect::<Vec<_>>())
		}
		"datetime-array" => {
			let dts = arr.iter().map(|s| dt(s)).collect::<Result<Vec<_>, _>>()?;
			doc.set_datetime_array(path, &dts)
		}
		"int-array-default" => doc.set_int_array_default(path, &ints(arr)?),
		"float-array-default" => doc.set_float_array_default(path, &flts(arr)?),
		"bool-array-default" => doc.set_bool_array_default(path, &bools(arr)),
		"string-array-default" => {
			let owned: Vec<String> = arr.iter().map(|s| unescape_ops(s)).collect();
			doc.set_string_array_default(
				path,
				&owned.iter().map(|s| s.as_str()).collect::<Vec<_>>(),
			)
		}
		"datetime-array-default" => {
			let dts = arr.iter().map(|s| dt(s)).collect::<Result<Vec<_>, _>>()?;
			doc.set_datetime_array_default(path, &dts)
		}
		"raw" => doc.set_raw(path, &unescape_ops(f.get(3).copied().unwrap_or("")), v),
		"raw-default" => {
			doc.set_raw_default(path, &unescape_ops(f.get(3).copied().unwrap_or("")), v)
		}
		"empty" => doc.set_empty(path),
		"comment" => doc.set_comment(path, v),
		"remove" => {
			doc.remove(path);
			true
		}
		other => return Err(format!("unknown op: {}", other)),
	};
	if !wrote {
		return Err(format!("cannot write {}", path));
	}
	Ok(())
}

/// Good-path wrapper: the op must apply.
fn apply_op(doc: &mut Document, line: &str, at: &str) {
	try_apply_op(doc, line).unwrap_or_else(|e| panic!("{}: {}", at, e));
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
		// Layer files: every `layer*.shcl`, read in filename (= priority) order.
		let mut layer_names: Vec<String> = std::fs::read_dir(&path)
			.unwrap()
			.filter_map(|e| e.ok().map(|e| e.file_name().to_string_lossy().into_owned()))
			.filter(|n| n.starts_with("layer") && n.ends_with(".shcl"))
			.collect();
		layer_names.sort();
		let layers: Vec<String> = layer_names
			.iter()
			.map(|n| std::fs::read_to_string(path.join(n)).unwrap())
			.collect();
		cases.push(Case {
			name: path.file_name().unwrap().to_string_lossy().into_owned(),
			input: std::fs::read_to_string(&input).unwrap(),
			expected_fmt: std::fs::read_to_string(path.join("expected.shcl")).unwrap(),
			reads: std::fs::read_to_string(path.join("reads.tsv")).unwrap(),
			expected_diags: std::fs::read_to_string(path.join("expected-diags.txt")).unwrap(),
			write_ops: read_opt("write.ops"),
			expected_write: read_opt("expected-write.shcl"),
			write_bad_ops: read_opt("write-bad.ops"),
			schema: read_opt("schema.shcl"),
			expected_validate: read_opt("expected-validate.txt"),
			layers,
			merge_sets: read_opt("merge.sets"),
			expected_merged: read_opt("expected-merged.shcl"),
			init_schema: read_opt("init-schema.shcl"),
			expected_init: read_opt("expected-init.shcl"),
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
fn diagnostics_match_expected() {
	// Pins count, line, severity, and stable code per case - the same shape
	// `check` prints to stdout at Standard (its cross-binding contract).
	for case in load_cases() {
		let doc = Document::parse(&case.input);
		let diags = doc.diagnostics();
		let mut got = String::new();
		for d in diags {
			got.push_str(&format!("line {}: {:?}: {}\n", d.line, d.severity, d.code));
		}
		let errors = diags
			.iter()
			.filter(|d| d.severity == shcl::Severity::Error)
			.count();
		if errors > 0 {
			got.push_str(&format!(
				"failed: {} diagnostic(s), {} error(s)\n",
				diags.len(),
				errors
			));
		} else {
			got.push_str(&format!("ok ({} diagnostic(s))\n", diags.len()));
		}
		assert_eq!(
			got, case.expected_diags,
			"{}: diagnostics differ from expected-diags.txt",
			case.name
		);
	}
}

#[test]
fn validation_matches_expected() {
	// Schema dimension: golden = the exact `check --schema` stdout at Standard
	// (doc parse diags, then validation diags, then the summary). A schema that
	// does not load cleanly is a single V099, mirroring the CLI.
	for case in load_cases() {
		let (schema_text, want) = match (&case.schema, &case.expected_validate) {
			(Some(s), Some(w)) => (s, w),
			(None, None) => continue,
			_ => panic!(
				"{}: schema.shcl and expected-validate.txt must come as a pair",
				case.name
			),
		};
		let doc = Document::parse(&case.input);
		let mut diags: Vec<shcl::Diagnostic> = doc.diagnostics().to_vec();
		let sdoc = Document::parse(schema_text);
		if sdoc
			.diagnostics()
			.iter()
			.any(|d| d.severity == shcl::Severity::Error)
		{
			diags.push(shcl::Diagnostic {
				line: 0,
				severity: shcl::Severity::Error,
				message: "schema failed to load".to_string(),
				code: "V099",
			});
		} else {
			diags.extend(doc.validate(&sdoc));
		}
		let mut got = String::new();
		for d in &diags {
			got.push_str(&format!("line {}: {:?}: {}\n", d.line, d.severity, d.code));
		}
		let errors = diags
			.iter()
			.filter(|d| d.severity == shcl::Severity::Error)
			.count();
		if errors > 0 {
			got.push_str(&format!(
				"failed: {} diagnostic(s), {} error(s)\n",
				diags.len(),
				errors
			));
		} else {
			got.push_str(&format!("ok ({} diagnostic(s))\n", diags.len()));
		}
		assert_eq!(
			got, *want,
			"{}: validation output differs from expected-validate.txt",
			case.name
		);
	}
}

#[test]
fn convenience_tier_falls_back_only_on_good() {
	// The get-tier value survives only on Good; Empty/BadType/NotFound all fall
	// back to the call-site default, so a real zero can't be faked. This pins the
	// semantic every port's *Or/get_*(default=) mirrors.
	let doc = Document::parse("a: 42\nb: not-a-number\ne:\narr: 1, 2, 3\n");
	assert_eq!(doc.get_int("a").unwrap_or(9), 42); // Good
	assert_eq!(doc.get_int("b").unwrap_or(9), 9); // BadType
	assert_eq!(doc.get_int("e").unwrap_or(9), 9); // Empty still falls back
	assert_eq!(doc.get_int("missing").unwrap_or(9), 9); // NotFound
	assert_eq!(doc.get_int_array("arr").unwrap_or(vec![7]), vec![1, 2, 3]);
	assert_eq!(doc.get_int_array("missing").unwrap_or(vec![7]), vec![7]);
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
					"rawinfo" => {
						let r = doc.read_raw_info(query);
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

#[test]
fn layered_merge_matches_expected() {
	// Layered-load dimension: fold the layer files (lowest first) and input.shcl
	// (highest file layer) via the library `merge`, apply the `path=value`
	// overrides as the top layer, and match the golden merged canonical.
	for case in load_cases() {
		let Some(want) = &case.expected_merged else {
			continue;
		};
		// Ordered lowest -> highest: the layer*.shcl files, then input.shcl.
		let mut texts: Vec<&str> = case.layers.iter().map(|s| s.as_str()).collect();
		texts.push(&case.input);
		let mut doc = Document::parse(texts[0]);
		for t in &texts[1..] {
			doc.merge(&Document::parse(t));
		}
		if let Some(sets) = &case.merge_sets {
			for line in sets.lines() {
				if line.is_empty() || line.starts_with('#') {
					continue;
				}
				let (p, v) = line
					.split_once('=')
					.unwrap_or_else(|| panic!("{}: bad merge.sets line: {}", case.name, line));
				doc.set_string(p, v);
			}
		}
		let got = doc.to_canonical();
		assert_eq!(
			&got, want,
			"{}: merged output differs from expected-merged.shcl",
			case.name
		);
		// The merged doc must be a formatter fixpoint like any canonical output.
		let again = Document::parse(&got).to_canonical();
		assert_eq!(
			again, got,
			"{}: merged output is not a fmt fixpoint",
			case.name
		);
	}
}

#[test]
fn init_generation_matches_expected() {
	// Generation dimension: `generate` on the schema must reproduce the golden
	// starter config, and that output must itself load cleanly.
	for case in load_cases() {
		let (schema, want) = match (&case.init_schema, &case.expected_init) {
			(Some(s), Some(w)) => (s, w),
			(None, None) => continue,
			_ => panic!(
				"{}: init-schema.shcl and expected-init.shcl must come as a pair",
				case.name
			),
		};
		let got = generate(&Document::parse(schema))
			.unwrap_or_else(|_| panic!("{}: init schema has faults", case.name));
		assert_eq!(
			&got, want,
			"{}: init output differs from expected-init.shcl",
			case.name
		);
		// The generated starter must be valid SHCL (loads with no error diagnostics).
		let doc = Document::parse(&got);
		assert!(
			!doc.diagnostics()
				.iter()
				.any(|d| d.severity == shcl::Severity::Error),
			"{}: generated starter does not load cleanly",
			case.name
		);
		// And it must satisfy the very schema that produced it - case 026's
		// golden once failed its own schema (repeat lower bound and a
		// materialized wildcard were ignored).
		let sdoc = Document::parse(schema);
		let vs = doc.validate(&sdoc);
		assert!(
			!vs.iter().any(|d| d.severity == shcl::Severity::Error),
			"{}: generated starter fails its own schema: {:?}",
			case.name,
			vs
		);
	}
}

#[test]
fn depth_cap_boundary_and_writer() {
	// Exactly at the cap: loads clean, formats, and round-trips.
	let segs: Vec<String> = (0..shcl::MAX_DEPTH).map(|i| format!("a{}", i)).collect();
	let at_cap = format!("{}: 1", segs.join("."));
	let doc = Document::parse(&at_cap);
	assert!(doc.diagnostics().is_empty(), "at-cap doc must load clean");
	let out = doc.to_canonical();
	assert_eq!(Document::parse(&out).to_canonical(), out);
	// One past the cap: a single E016, line skipped, strict load fails.
	let over = format!("a.{}: 1", segs.join("."));
	let doc2 = Document::parse(&over);
	assert_eq!(doc2.diagnostics().len(), 1);
	assert_eq!(doc2.diagnostics()[0].code, "E016");
	assert_eq!(doc2.to_canonical(), "");
	// The Writer refuses to create past the cap and stays a no-op.
	let mut w = Document::new();
	let deep_path = format!("a.{}", segs.join("."));
	w.set_int(&deep_path, 1);
	assert!(
		!w.exists("a"),
		"writer must not half-create a too-deep path"
	);
	w.set_int(&segs.join("."), 2);
	assert!(w.exists("a0"), "writer must still create an at-cap path");
}

#[test]
fn write_bad_ops_are_rejected() {
	// Bad-op dimension: each write-bad.ops line, applied alone to the case
	// input, must be rejected (bad value, bad datetime, or unusable path) and
	// leave the document unchanged.
	for case in load_cases() {
		let Some(bad) = &case.write_bad_ops else {
			continue;
		};
		for (n, line) in bad.lines().enumerate() {
			if line.is_empty() || line.starts_with('#') {
				continue;
			}
			let mut doc = Document::parse(&case.input);
			let before = doc.to_canonical();
			assert!(
				try_apply_op(&mut doc, line).is_err(),
				"{}: write-bad.ops line {} was accepted: {}",
				case.name,
				n + 1,
				line
			);
			assert_eq!(
				doc.to_canonical(),
				before,
				"{}: write-bad.ops line {} changed the document: {}",
				case.name,
				n + 1,
				line
			);
		}
	}
}

#[test]
fn read_surface_line_quoted_children() {
	// line/quoted on the read result, line(path), children(path). Same
	// fixture in every runner (C pins the accessors; its read structs stay
	// value+status).
	let text = "a: @null\nb: \"@null\"\ncode:\n\thook: 1\n\thook: 2\n\tdone: 3\n";
	let doc = Document::parse(text);
	assert!(!doc.read_string("a").quoted);
	assert!(doc.read_string("b").quoted);
	assert_eq!(doc.read_string("b").line, 2);
	assert_eq!(doc.line("code.done"), 6);
	assert_eq!(doc.line("code"), 3);
	assert_eq!(doc.line("missing"), 0);
	assert_eq!(doc.children("code"), vec!["hook", "hook", "done"]);
	assert_eq!(doc.children(""), vec!["a", "b", "code"]);
	assert!(doc.children("missing").is_empty());
}

#[test]
fn strict_failure_carries_document() {
	// A failed strict load hands back the document and names the first
	// failures in the message - the diagnostics are the point.
	let e = Document::parse_with("ok: 1\n: nope\n", Strictness::Strict).unwrap_err();
	assert!(!e.diagnostics.is_empty());
	assert_eq!(e.document.read_int("ok").value, 1);
	let msg = e.to_string();
	assert!(msg.contains("; line "), "{}", msg);
}

#[test]
fn raw_is_source_text() {
	// raw: the verbatim value span from the source line - not the display
	// join, which rewrites `{2,3}` to `{2, 3}`. Same fixture in every runner
	// whose read result exposes raw (the C read structs deliberately do not).
	let doc = Document::parse("regex: ^\\d{2,3}$\nlist: a,  \"b c\"\n");
	assert_eq!(doc.read_string("regex").raw.as_deref(), Some("^\\d{2,3}$"));
	assert_eq!(
		doc.read_string_array("list").raw.as_deref(),
		Some("a,  \"b c\"")
	);
	// A written value has no source spelling; raw falls back to display. The
	// selector's escaped spelling must land on the existing instance.
	let mut doc2 = Document::parse("who: 'q\"uote'\n");
	assert!(doc2.set_int("who[\"q\\\"uote\"].n", 5));
	assert_eq!(doc2.count("who"), 1);
	let r = doc2.read_int("who['q\"uote'].n");
	assert_eq!((r.value, r.status), (5, shcl::Status::Good));
	assert_eq!(doc2.read_int("who['q\"uote'].n").raw.as_deref(), Some("5"));
}

#[test]
fn paths_enumeration_shape() {
	// paths(): file order, deduplicated, non-bare segments quoted so every
	// path resolves. Same fixture is pinned in every runner.
	let doc = Document::parse("a: 1\na.b: 2\n\"q n\": 3\nx:\n\tb: 4\nx.b: 5\n");
	assert_eq!(doc.paths(), vec!["a", "a.b", "\"q n\"", "x", "x.b"]);
	for p in doc.paths() {
		assert!(doc.count(&p) >= 1, "emitted path does not resolve: {}", p);
	}
	// quote_segment: same spelling both directions, injection-safe.
	assert_eq!(quote_segment("port"), "port");
	assert_eq!(quote_segment("q n"), "\"q n\"");
	assert_eq!(quote_segment("a.b"), "\"a.b\"");
	let r = doc.read_int(&quote_segment("q n"));
	assert_eq!((r.value, r.status), (3, shcl::Status::Good));
}
