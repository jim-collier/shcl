// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

//! `shcl` CLI - the Tier 1 command binding. POSIX sh and PowerShell wrap this,
//! so the exit codes and flags below are a stable surface, not conveniences.

use shcl::{Diagnostic, Document, Severity, Status, Strictness, generate, parse_datetime};
use std::process::ExitCode;

const HELP: &str = "\
shcl - Simple Hierarchical Config Language (reference CLI)

Usage:
  shcl get [type] [options] FILE PATH    read one value (or array) at a path
  shcl set [--write|-w] [options] FILE   apply write-ops (stdin); print canonical
                                         (or rewrite FILE in place with --write)
  shcl fmt [--write|-w] FILE             print (or rewrite in place) the canonical form
  shcl check [options] FILE              load and print diagnostics
                                         (--schema=SCHEMA also validates FILE
                                         against a schema, itself a .shcl file)
  shcl init --schema=SCHEMA              print a commented starter config from
                                         a schema (required fields live, optional
                                         commented, wildcards noted)
  shcl count [options] FILE PATH         number of instances at a path
  shcl instances [options] FILE PATH     instance values at a path, one per line
  shcl help | version                    this help, or the version (also -h/--help, -V/--version)

set reads a write-ops script from stdin (one op per line, tab-separated) and
prints the canonical document. FILE is the base ('-' = empty base). Ops:
  int|float|bool|string|datetime<TAB>PATH<TAB>VALUE       set a scalar
  <type>-array<TAB>PATH<TAB>V1<TAB>V2...                  set an inline array
  <type>[-array]-default<TAB>...                          set only if absent
  raw<TAB>PATH<TAB>INFO<TAB>CONTENT                       set a raw block
  empty<TAB>PATH   comment<TAB>PATH<TAB>TEXT   remove<TAB>PATH
string/raw values decode \\n \\t \\\\; a line starting with # is a script comment.

Types (default --string):
  --int --float --bool --datetime --string --raw --rawinfo
  --array                                read the value as an array of the type
  --rawinfo reads a raw block's info-string (the fence tag), not its content

Options:
  --default=VALUE                        value to print when the read is not Good
                                         (implies --on-bad=default; for arrays,
                                         substituted per bad slot)
  --on-bad=error|default|flag            error: fail loudly; default: print the
                                         default; flag: print the value anyway and
                                         report via exit code (the default mode)
  --slots                                prefix each line with its slot status and
                                         a tab (per element, or per wildcard slot)
  --strictness=loose|standard|strict     or 1|2|3 (default standard)
  --schema=SCHEMA                        (check/init) validate FILE against a
                                         schema; adds V### diagnostics
  --layer=FILE                           (get/fmt/count/instances/set) merge a
                                         lower-priority layer under FILE;
                                         repeatable, earlier = lower priority
  --set=PATH=VALUE                       override one path as the top layer,
                                         after all files; repeatable

Value options accept either spelling: --default=VALUE or --default VALUE.
An option a subcommand does not use is a usage error, not ignored.
FILE may be '-' for stdin. With --layer, FILE is the highest file layer and
each --layer is merged under it in order; --set applies last. 'fmt' with
layers prints the merged canonical document.

Exit codes: 0 good, 1 usage or I/O error, 2 empty, 3 not found, 4 bad type,
5 multiple instances, 6 check failed or strict load failure.
";

fn status_code(st: Status) -> u8 {
	match st {
		Status::Good => 0,
		Status::Empty => 2,
		Status::NotFound => 3,
		Status::BadType => 4,
		Status::Multiple => 5,
	}
}

struct Opts {
	kind: String, // int|float|bool|datetime|string|raw
	array: bool,
	slots: bool,
	default: Option<String>,
	on_bad: String, // error|default|flag
	strictness: Strictness,
	write: bool,
	schema: Option<String>,
	layers: Vec<String>,         // lower-priority layers, in listed order
	sets: Vec<(String, String)>, // final override layer: path=value
	args: Vec<String>,           // positional: FILE [PATH]
	seen: Vec<&'static str>,     // canonical names of options given, for per-command validation
}

fn parse_opts(argv: &[String]) -> Result<Opts, String> {
	let mut o = Opts {
		kind: "string".into(),
		array: false,
		slots: false,
		default: None,
		on_bad: "flag".into(),
		strictness: Strictness::Standard,
		write: false,
		schema: None,
		layers: Vec::new(),
		sets: Vec::new(),
		args: Vec::new(),
		seen: Vec::new(),
	};
	// Value-taking options accept both --opt=VALUE and the space form --opt VALUE.
	let mut i = 0;
	while i < argv.len() {
		let a = argv[i].as_str();
		match a {
			"--int" | "--float" | "--bool" | "--datetime" | "--string" | "--raw" | "--rawinfo" => {
				o.kind = a[2..].to_string();
				o.seen.push("--<type>");
			}
			"--array" => {
				o.array = true;
				o.seen.push("--array");
			}
			"--slots" => {
				o.slots = true;
				o.seen.push("--slots");
			}
			"--write" | "-w" => {
				o.write = true;
				o.seen.push("--write");
			}
			"--default" | "--on-bad" | "--strictness" | "--schema" | "--layer" | "--set" => {
				i += 1;
				let v = argv
					.get(i)
					.ok_or_else(|| format!("missing value for {} (try {}=VALUE)", a, a))?;
				set_value_opt(&mut o, a, v)?;
			}
			_ if a.starts_with("--default=") => set_value_opt(&mut o, "--default", &a[10..])?,
			_ if a.starts_with("--on-bad=") => set_value_opt(&mut o, "--on-bad", &a[9..])?,
			_ if a.starts_with("--strictness=") => set_value_opt(&mut o, "--strictness", &a[13..])?,
			_ if a.starts_with("--schema=") => set_value_opt(&mut o, "--schema", &a[9..])?,
			_ if a.starts_with("--layer=") => set_value_opt(&mut o, "--layer", &a[8..])?,
			_ if a.starts_with("--set=") => set_value_opt(&mut o, "--set", &a[6..])?,
			_ if a.starts_with('-') && a.len() > 1 => {
				return Err(format!("unknown option: {}", a));
			}
			_ => o.args.push(argv[i].clone()),
		}
		i += 1;
	}
	Ok(o)
}

fn set_value_opt(o: &mut Opts, name: &str, v: &str) -> Result<(), String> {
	match name {
		"--default" => {
			o.default = Some(v.to_string());
			o.on_bad = "default".into();
			o.seen.push("--default");
		}
		"--on-bad" => {
			if !matches!(v, "error" | "default" | "flag") {
				return Err(format!("bad --on-bad value: {}", v));
			}
			o.on_bad = v.to_string();
			o.seen.push("--on-bad");
		}
		"--strictness" => {
			o.strictness =
				Strictness::from_arg(v).ok_or_else(|| format!("bad --strictness value: {}", v))?;
			o.seen.push("--strictness");
		}
		"--schema" => {
			o.schema = Some(v.to_string());
			o.seen.push("--schema");
		}
		"--layer" => {
			o.layers.push(v.to_string());
			o.seen.push("--layer");
		}
		"--set" => {
			let (p, val) = v
				.split_once('=')
				.ok_or_else(|| format!("bad --set value (want PATH=VALUE): {}", v))?;
			o.sets.push((p.to_string(), val.to_string()));
			o.seen.push("--set");
		}
		_ => return Err(format!("unknown option: {}", name)),
	}
	Ok(())
}

/// Every option must be meaningful for its subcommand; an option that would be
/// silently ignored (`set --write` before it existed, `--schema` on `get`) is a
/// usage error instead.
fn check_opts(cmd: &str, o: &Opts) -> Result<(), u8> {
	let allowed: &[&str] = match cmd {
		"get" => &[
			"--<type>",
			"--array",
			"--slots",
			"--default",
			"--on-bad",
			"--strictness",
			"--layer",
			"--set",
		],
		"set" => &["--strictness", "--layer", "--set", "--write"],
		"fmt" => &["--write", "--strictness", "--layer", "--set"],
		"check" => &["--strictness", "--schema"],
		"init" => &["--schema"],
		"count" | "instances" => &["--strictness", "--layer", "--set"],
		_ => &[],
	};
	for s in &o.seen {
		if !allowed.contains(s) {
			if *s == "--<type>" {
				eprintln!("type options are not valid for {} (see --help)", cmd);
			} else {
				eprintln!("option {} not valid for {} (see --help)", s, cmd);
			}
			return Err(1);
		}
	}
	Ok(())
}

/// Load `file` with `o`'s lower-priority `--layer` files underneath it and its
/// `--set` overrides on top - the layered-load fold. Every layer parses at the
/// requested strictness; a strict-load failure on any layer aborts like a
/// single-file strict failure (exit 6, nothing printed).
fn load_layered(o: &Opts, file: &str) -> Result<Document, u8> {
	// Lowest -> highest file layer: the --layer files in order, then FILE.
	let mut texts: Vec<String> = Vec::with_capacity(o.layers.len() + 1);
	for lf in &o.layers {
		texts.push(read_input(lf).map_err(|e| {
			eprintln!("{}", e);
			1u8
		})?);
	}
	let base_text = read_input(file).map_err(|e| {
		eprintln!("{}", e);
		1u8
	})?;
	texts.push(base_text);
	let mut doc = load(&texts[0], o.strictness)?;
	for t in &texts[1..] {
		let over = load(t, o.strictness)?;
		doc.merge(&over);
	}
	for (p, v) in &o.sets {
		if !doc.set_string(p, v) {
			eprintln!("shcl: cannot write {} (from --set)", p);
			return Err(1);
		}
	}
	Ok(doc)
}

/// Write atomically: temp file in the same dir, then rename over the target,
/// so an interrupted write can never truncate the config it rewrites. The data
/// is synced before the rename so a crash cannot publish an empty file.
///
/// A rename publishes a new inode, so the target is resolved through symlinks
/// first (otherwise a linked-in config gets replaced by a regular file and the
/// real one is left stale) and the original's mode is copied onto the temp file
/// (otherwise a 600 config comes back at whatever the umask allows). Other hard
/// links to the old inode cannot survive a rename and keep the old content.
fn write_atomic(file: &str, data: &str) -> Result<(), String> {
	use std::io::Write;
	// canonicalize fails when the target does not exist yet; that is a plain
	// create, so the path as given is already the right one.
	let target = std::fs::canonicalize(file).unwrap_or_else(|_| std::path::PathBuf::from(file));
	let dir = match target.parent() {
		Some(d) if !d.as_os_str().is_empty() => d,
		_ => std::path::Path::new("."),
	};
	let base = target
		.file_name()
		.map(|b| b.to_string_lossy().into_owned())
		.unwrap_or_else(|| file.to_string());
	let tmp = dir.join(format!(".{}.tmp{}", base, std::process::id()));
	let res = std::fs::File::create(&tmp).and_then(|mut f| {
		f.write_all(data.as_bytes())?;
		// Best effort: a filesystem that cannot carry the mode is not a reason
		// to fail a write that otherwise succeeded.
		if let Ok(m) = std::fs::metadata(&target) {
			let _ = f.set_permissions(m.permissions());
		}
		f.sync_all()
	});
	if let Err(e) = res {
		let _ = std::fs::remove_file(&tmp);
		return Err(format!("{}: {}", file, e));
	}
	std::fs::rename(&tmp, &target).map_err(|e| {
		let _ = std::fs::remove_file(&tmp);
		format!("{}: {}", file, e)
	})
}

fn read_input(file: &str) -> Result<String, String> {
	if file == "-" {
		let mut s = String::new();
		use std::io::Read;
		std::io::stdin()
			.read_to_string(&mut s)
			.map_err(|e| format!("stdin: {}", e))?;
		Ok(s)
	} else {
		std::fs::read_to_string(file).map_err(|e| format!("{}: {}", file, e))
	}
}

fn load(text: &str, strictness: Strictness) -> Result<Document, u8> {
	match Document::parse_with(text, strictness) {
		Ok(d) => Ok(d),
		Err(e) => {
			for d in &e.diagnostics {
				eprintln!("line {}: {:?}: {}", d.line, d.severity, d.message);
			}
			eprintln!("{}", e);
			Err(6)
		}
	}
}

/// One value read, formatted for the shell: scalars print as one line, arrays
/// one element per line.
fn do_get(o: &Opts) -> u8 {
	let (file, path) = match o.args.as_slice() {
		[f, p] => (f, p),
		_ => {
			eprintln!("get needs FILE and PATH (see --help)");
			return 1;
		}
	};
	let doc = match load_layered(o, file) {
		Ok(d) => d,
		Err(code) => return code,
	};
	let (lines, status, slots): (Vec<String>, Status, Vec<Status>) = if o.array {
		match o.kind.as_str() {
			"int" => {
				let r = doc.read_int_array(path);
				(
					r.value.iter().map(|v| v.to_string()).collect(),
					r.status,
					r.slots,
				)
			}
			"float" => {
				let r = doc.read_float_array(path);
				(
					r.value.iter().map(|v| v.to_string()).collect(),
					r.status,
					r.slots,
				)
			}
			"bool" => {
				let r = doc.read_bool_array(path);
				(
					r.value.iter().map(|v| v.to_string()).collect(),
					r.status,
					r.slots,
				)
			}
			"datetime" => {
				let r = doc.read_datetime_array(path);
				(
					r.value.iter().map(|v| v.to_string()).collect(),
					r.status,
					r.slots,
				)
			}
			"raw" | "rawinfo" => {
				eprintln!("--{} has no --array form", o.kind);
				return 1;
			}
			_ => {
				let r = doc.read_string_array(path);
				(r.value, r.status, r.slots)
			}
		}
	} else {
		match o.kind.as_str() {
			"int" => {
				let r = doc.read_int(path);
				(vec![r.value.to_string()], r.status, Vec::new())
			}
			"float" => {
				let r = doc.read_float(path);
				(vec![r.value.to_string()], r.status, Vec::new())
			}
			"bool" => {
				let r = doc.read_bool(path);
				(vec![r.value.to_string()], r.status, Vec::new())
			}
			"datetime" => {
				let r = doc.read_datetime(path);
				(vec![r.value.to_string()], r.status, Vec::new())
			}
			"raw" => {
				let r = doc.read_raw(path);
				(vec![r.value], r.status, Vec::new())
			}
			"rawinfo" => {
				let r = doc.read_raw_info(path);
				(vec![r.value], r.status, Vec::new())
			}
			_ => {
				let r = doc.read_string(path);
				(vec![r.value], r.status, Vec::new())
			}
		}
	};
	// Per-line slot status: falls back to the aggregate for scalar reads.
	let slot_at = |i: usize| slots.get(i).copied().unwrap_or(status);
	let emit = |lines: &[String]| {
		for (i, l) in lines.iter().enumerate() {
			if o.slots {
				println!("{:?}\t{}", slot_at(i), l);
			} else {
				println!("{}", l);
			}
		}
	};
	match (status, o.on_bad.as_str()) {
		(Status::Good, _) | (Status::Empty, "flag") => {
			emit(&lines);
			status_code(status)
		}
		(_, "default") => {
			if !slots.is_empty() {
				// Array read: the default substitutes per bad slot; alignment holds.
				let dv = o.default.clone().unwrap_or_default();
				let subbed: Vec<String> = lines
					.iter()
					.enumerate()
					.map(|(i, l)| {
						if slot_at(i) == Status::Good {
							l.clone()
						} else {
							dv.clone()
						}
					})
					.collect();
				emit(&subbed);
			} else {
				let dv = o.default.clone().unwrap_or_default();
				if o.slots {
					println!("{:?}\t{}", status, dv);
				} else {
					println!("{}", dv);
				}
			}
			0
		}
		(_, "error") => {
			let type_name = if o.array {
				format!("{} array", o.kind)
			} else {
				o.kind.clone()
			};
			let reason = match status {
				Status::BadType => match doc.read_string(path).raw {
					Some(raw) => format!("value {:?} is not a valid {}", raw, type_name),
					None => format!("value is not a valid {}", type_name),
				},
				Status::NotFound => "no value at that path".to_string(),
				Status::Empty => "the value is empty".to_string(),
				Status::Multiple => "the path matches multiple instances".to_string(),
				Status::Good => String::new(), // handled above; keep the match total
			};
			eprintln!(
				"shcl: cannot read {} as {}: {} (in {})",
				path, type_name, reason, file
			);
			status_code(status)
		}
		(_, _) => {
			// flag: print the zero/empty value anyway; the exit code carries the status
			emit(&lines);
			status_code(status)
		}
	}
}

fn do_fmt(o: &Opts) -> u8 {
	let file = match o.args.as_slice() {
		[f] => f,
		_ => {
			eprintln!("fmt needs FILE (see --help)");
			return 1;
		}
	};
	if o.write && file == "-" {
		eprintln!("fmt --write cannot rewrite stdin; drop --write to print, or pass a FILE");
		return 1;
	}
	let canonical = match load_layered(o, file) {
		Ok(d) => d.to_canonical(),
		Err(code) => return code,
	};
	if o.write {
		if let Err(e) = write_atomic(file, &canonical) {
			eprintln!("{}", e);
			return 1;
		}
	} else {
		print!("{}", canonical);
	}
	0
}

/// Decode an ops-script value: \n \t \\ only; other `\x` stays verbatim. The
/// setters re-encode, so this is just for embedding newlines/tabs on one line.
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

fn apply_op(doc: &mut Document, line: &str) -> Result<(), String> {
	let f: Vec<&str> = line.split('\t').collect();
	let path = f.get(1).copied().unwrap_or("");
	let val = || f.get(2).copied().unwrap_or("");
	let pint = |s: &str| s.parse::<i64>().map_err(|_| format!("bad int: {}", s));
	let pflt = |s: &str| s.parse::<f64>().map_err(|_| format!("bad float: {}", s));
	let arr = &f[2.min(f.len())..];
	let wrote = match f.first().copied().unwrap_or("") {
		"int" => doc.set_int(path, pint(val())?),
		"float" => doc.set_float(path, pflt(val())?),
		"bool" => doc.set_bool(path, val() == "true"),
		"string" => doc.set_string(path, &unescape_ops(val())),
		"datetime" => {
			let dt = parse_datetime(val()).ok_or_else(|| format!("bad datetime: {}", val()))?;
			doc.set_datetime(path, &dt)
		}
		"int-default" => doc.set_int_default(path, pint(val())?),
		"float-default" => doc.set_float_default(path, pflt(val())?),
		"bool-default" => doc.set_bool_default(path, val() == "true"),
		"string-default" => doc.set_string_default(path, &unescape_ops(val())),
		"datetime-default" => {
			let dt = parse_datetime(val()).ok_or_else(|| format!("bad datetime: {}", val()))?;
			doc.set_datetime_default(path, &dt)
		}
		"int-array" => doc.set_int_array(
			path,
			&arr.iter().map(|s| pint(s)).collect::<Result<Vec<_>, _>>()?,
		),
		"float-array" => doc.set_float_array(
			path,
			&arr.iter().map(|s| pflt(s)).collect::<Result<Vec<_>, _>>()?,
		),
		"bool-array" => {
			doc.set_bool_array(path, &arr.iter().map(|s| *s == "true").collect::<Vec<_>>())
		}
		"string-array" => {
			let owned: Vec<String> = arr.iter().map(|s| unescape_ops(s)).collect();
			doc.set_string_array(path, &owned.iter().map(|s| s.as_str()).collect::<Vec<_>>())
		}
		"datetime-array" => {
			let dts: Vec<_> = arr
				.iter()
				.map(|s| parse_datetime(s).ok_or_else(|| format!("bad datetime: {}", s)))
				.collect::<Result<Vec<_>, _>>()?;
			doc.set_datetime_array(path, &dts)
		}
		"int-array-default" => doc.set_int_array_default(
			path,
			&arr.iter().map(|s| pint(s)).collect::<Result<Vec<_>, _>>()?,
		),
		"float-array-default" => doc.set_float_array_default(
			path,
			&arr.iter().map(|s| pflt(s)).collect::<Result<Vec<_>, _>>()?,
		),
		"bool-array-default" => {
			doc.set_bool_array_default(path, &arr.iter().map(|s| *s == "true").collect::<Vec<_>>())
		}
		"string-array-default" => {
			let owned: Vec<String> = arr.iter().map(|s| unescape_ops(s)).collect();
			doc.set_string_array_default(
				path,
				&owned.iter().map(|s| s.as_str()).collect::<Vec<_>>(),
			)
		}
		"raw" => doc.set_raw(path, &unescape_ops(f.get(3).copied().unwrap_or("")), val()),
		"raw-default" => {
			doc.set_raw_default(path, &unescape_ops(f.get(3).copied().unwrap_or("")), val())
		}
		"empty" => doc.set_empty(path),
		"comment" => doc.set_comment(path, val()),
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

fn do_set(o: &Opts) -> u8 {
	let file = match o.args.as_slice() {
		[f] => f,
		_ => {
			eprintln!("set needs FILE (ops on stdin; see --help)");
			return 1;
		}
	};
	if o.write && file == "-" {
		eprintln!("set --write cannot rewrite stdin; drop --write to print, or pass a FILE");
		return 1;
	}
	// Base doc: '-' means an empty base, since stdin carries the ops script.
	// Any --layer files sit under it and --set overrides sit on top, before ops.
	let mut layer_texts: Vec<String> = Vec::new();
	for lf in &o.layers {
		match read_input(lf) {
			Ok(t) => layer_texts.push(t),
			Err(e) => {
				eprintln!("{}", e);
				return 1;
			}
		}
	}
	let base_text = if file == "-" {
		String::new()
	} else {
		match read_input(file) {
			Ok(t) => t,
			Err(e) => {
				eprintln!("{}", e);
				return 1;
			}
		}
	};
	layer_texts.push(base_text);
	let mut doc = match load(&layer_texts[0], o.strictness) {
		Ok(d) => d,
		Err(code) => return code,
	};
	for t in &layer_texts[1..] {
		match load(t, o.strictness) {
			Ok(over) => doc.merge(&over),
			Err(code) => return code,
		}
	}
	for (p, v) in &o.sets {
		if !doc.set_string(p, v) {
			eprintln!("shcl: cannot write {} (from --set)", p);
			return 1;
		}
	}
	let mut ops = String::new();
	use std::io::Read;
	if let Err(e) = std::io::stdin().read_to_string(&mut ops) {
		eprintln!("stdin: {}", e);
		return 1;
	}
	for (n, line) in ops.lines().enumerate() {
		if line.is_empty() || line.starts_with('#') {
			continue;
		}
		if let Err(e) = apply_op(&mut doc, line) {
			eprintln!("op line {}: {}", n + 1, e);
			return 1;
		}
	}
	let canonical = doc.to_canonical();
	if o.write {
		if let Err(e) = write_atomic(file, &canonical) {
			eprintln!("{}", e);
			return 1;
		}
	} else {
		print!("{}", canonical);
	}
	0
}

fn do_check(o: &Opts) -> u8 {
	let file = match o.args.as_slice() {
		[f] => f,
		_ => {
			eprintln!("check needs FILE (see --help)");
			return 1;
		}
	};
	let text = match read_input(file) {
		Ok(t) => t,
		Err(e) => {
			eprintln!("{}", e);
			return 1;
		}
	};
	let (diags, strict_failed) = match Document::parse_with(&text, o.strictness) {
		Ok(doc) => {
			let mut diags = doc.diagnostics().to_vec();
			// --schema: append validation diagnostics under the same contract.
			// The schema itself always loads at Standard (a program artifact);
			// one that does not load cleanly is a single V099 schema fault.
			if let Some(schema_file) = &o.schema {
				let stext = match read_input(schema_file) {
					Ok(t) => t,
					Err(e) => {
						eprintln!("{}", e);
						return 1;
					}
				};
				let sdoc = Document::parse(&stext);
				if sdoc
					.diagnostics()
					.iter()
					.any(|d| d.severity == Severity::Error)
				{
					for d in sdoc.diagnostics() {
						eprintln!("schema line {}: {:?}: {}", d.line, d.severity, d.message);
					}
					diags.push(Diagnostic {
						line: 0,
						severity: Severity::Error,
						message: "schema failed to load".to_string(),
						code: "V099",
					});
				} else {
					diags.extend(doc.validate(&sdoc));
				}
			}
			(diags, false)
		}
		Err(e) => (e.diagnostics.clone(), true),
	};
	// stdout carries the stable codes - the cross-binding contract. The prose is
	// per-binding voice and goes to stderr (which the differential check drops).
	// A V090-V093 line number is a SCHEMA line (the code table says so); the
	// prose names the file so the two number spaces cannot be confused.
	for d in &diags {
		println!("line {}: {:?}: {}", d.line, d.severity, d.code);
		let space = if d.code.starts_with("V09") && d.code != "V099" {
			"schema line"
		} else {
			"line"
		};
		eprintln!("{} {}: {:?}: {}", space, d.line, d.severity, d.message);
	}
	let errors = diags
		.iter()
		.filter(|d| d.severity == Severity::Error)
		.count();
	if strict_failed {
		println!("strict load failed: {} diagnostic(s)", diags.len());
		6
	} else if errors > 0 {
		// Loaded, but lines were dropped: nonzero so a CI gate on check catches it.
		println!("failed: {} diagnostic(s), {} error(s)", diags.len(), errors);
		6
	} else {
		println!("ok ({} diagnostic(s))", diags.len());
		0
	}
}

fn do_init(o: &Opts) -> u8 {
	let schema_file = match &o.schema {
		Some(s) => s,
		None => {
			eprintln!("init needs --schema=FILE (see --help)");
			return 1;
		}
	};
	let stext = match read_input(schema_file) {
		Ok(t) => t,
		Err(e) => {
			eprintln!("{}", e);
			return 1;
		}
	};
	// The schema always loads at Standard - a program artifact, not user data.
	let sdoc = Document::parse(&stext);
	if sdoc
		.diagnostics()
		.iter()
		.any(|d| d.severity == Severity::Error)
	{
		for d in sdoc.diagnostics() {
			eprintln!("schema line {}: {:?}: {}", d.line, d.severity, d.message);
		}
		eprintln!("init: schema failed to load");
		// A broken schema is a config-semantics failure, not a usage error:
		// same exit as `check --schema` reporting it.
		return 6;
	}
	match generate(&sdoc) {
		Ok(text) => {
			print!("{}", text);
			0
		}
		Err(faults) => {
			for d in &faults {
				eprintln!("schema line {}: {:?}: {}", d.line, d.severity, d.message);
			}
			eprintln!("init: schema has faults");
			6
		}
	}
}

fn do_enum(o: &Opts, want_count: bool) -> u8 {
	let (file, path) = match o.args.as_slice() {
		[f, p] => (f, p),
		_ => {
			eprintln!("count/instances need FILE and PATH (see --help)");
			return 1;
		}
	};
	let doc = match load_layered(o, file) {
		Ok(d) => d,
		Err(code) => return code,
	};
	if want_count {
		println!("{}", doc.count(path));
	} else {
		for v in doc.instances(path) {
			println!("{}", v);
		}
	}
	0
}

fn run(cmd: &str, o: &Opts) -> u8 {
	if let Err(code) = check_opts(cmd, o) {
		return code;
	}
	match cmd {
		"get" => do_get(o),
		"set" => do_set(o),
		"fmt" => do_fmt(o),
		"check" => do_check(o),
		"init" => do_init(o),
		"count" => do_enum(o, true),
		"instances" => do_enum(o, false),
		other => {
			eprintln!("unknown command: {} (see --help)", other);
			1
		}
	}
}

/// cicd profiler stage only (profiling builds, SHCL_PROFILE_OUT set): repeat the
/// command under an in-process sampler for SHCL_PROFILE_SECS, then write a
/// flamegraph SVG. Never compiled into a normal build.
#[cfg(feature = "profiling")]
fn run_profiled(cmd: &str, o: &Opts, out: &str) -> u8 {
	let secs: u64 = std::env::var("SHCL_PROFILE_SECS")
		.ok()
		.and_then(|v| v.parse().ok())
		.unwrap_or(8);
	let guard = pprof::ProfilerGuardBuilder::default()
		.frequency(199)
		.blocklist(&["libc", "libpthread", "vdso", "libgcc"])
		.build()
		.expect("pprof: failed to start profiler");
	let deadline = std::time::Instant::now() + std::time::Duration::from_secs(secs);
	let mut code = run(cmd, o);
	while std::time::Instant::now() < deadline {
		code = run(cmd, o);
	}
	let report = guard
		.report()
		.build()
		.expect("pprof: failed to build report");
	let file = std::fs::File::create(out).expect("pprof: failed to create SVG");
	report
		.flamegraph(file)
		.expect("pprof: failed to write flamegraph");
	eprintln!("shcl: wrote flamegraph -> {}", out);
	code
}

/// Rust's runtime sets SIGPIPE to SIG_IGN, so a closed stdout surfaces as an
/// EPIPE write error and the next println! panics (exit 134). Restore SIG_DFL so
/// a broken pipe kills us by signal - the conventional 141 - like head/cat, and
/// matching the other bindings. Self-contained extern to stay zero-dep.
#[cfg(unix)]
fn reset_sigpipe() {
	const SIGPIPE: i32 = 13;
	const SIG_DFL: usize = 0;
	unsafe {
		unsafe extern "C" {
			fn signal(signum: i32, handler: usize) -> usize;
		}
		signal(SIGPIPE, SIG_DFL);
	}
}
#[cfg(not(unix))]
fn reset_sigpipe() {}

fn main() -> ExitCode {
	reset_sigpipe();
	let argv: Vec<String> = match std::env::args_os()
		.skip(1)
		.map(|a| a.into_string())
		.collect::<Result<Vec<_>, _>>()
	{
		Ok(v) => v,
		Err(_) => {
			eprintln!("invalid argument encoding (expected UTF-8)");
			return ExitCode::from(1);
		}
	};
	let first = argv.first().map(|s| s.as_str());
	if argv.is_empty() || argv.iter().any(|a| a == "-h" || a == "--help") || first == Some("help") {
		print!("{}", HELP);
		return ExitCode::from(if argv.is_empty() { 1 } else { 0 });
	}
	if argv.iter().any(|a| a == "-V" || a == "--version") || first == Some("version") {
		println!("shcl {}", env!("CARGO_PKG_VERSION"));
		return ExitCode::from(0);
	}
	let cmd = argv[0].clone();
	let o = match parse_opts(&argv[1..]) {
		Ok(o) => o,
		Err(e) => {
			eprintln!("{}", e);
			return ExitCode::from(1);
		}
	};
	#[cfg(feature = "profiling")]
	if let Ok(out) = std::env::var("SHCL_PROFILE_OUT") {
		return ExitCode::from(run_profiled(&cmd, &o, &out));
	}
	ExitCode::from(run(&cmd, &o))
}
