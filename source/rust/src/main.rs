// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

//! `shcl` CLI - the Tier 1 command binding. POSIX sh and PowerShell wrap this,
//! so the exit codes and flags below are a stable surface, not conveniences.

use shcl::{Document, Status, Strictness};
use std::process::ExitCode;

const HELP: &str = "\
shcl - Simple Hierarchical Config Language (reference CLI)

Usage:
  shcl get [type] [options] FILE PATH    read one value (or array) at a path
  shcl fmt [--write] FILE                print (or rewrite) the canonical form
  shcl check [options] FILE              load and print diagnostics
  shcl count [options] FILE PATH         number of instances at a path
  shcl instances [options] FILE PATH     instance values at a path, one per line

Types (default --string):
  --int --float --bool --datetime --string --raw
  --array                                read the value as an array of the type

Options:
  --default=VALUE                        value to print when the read is not Good
                                         (implies --on-bad=default)
  --on-bad=error|default|flag            error: fail loudly; default: print the
                                         default; flag: print the value anyway and
                                         report via exit code (the default mode)
  --strictness=loose|standard|strict     or 1|2|3 (default standard)

FILE may be '-' for stdin.

Exit codes: 0 good, 1 usage or I/O error, 2 empty, 3 not found, 4 bad type,
5 multiple instances, 6 strict load failure.
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
	default: Option<String>,
	on_bad: String, // error|default|flag
	strictness: Strictness,
	write: bool,
	args: Vec<String>, // positional: FILE [PATH]
}

fn parse_opts(argv: &[String]) -> Result<Opts, String> {
	let mut o = Opts {
		kind: "string".into(),
		array: false,
		default: None,
		on_bad: "flag".into(),
		strictness: Strictness::Standard,
		write: false,
		args: Vec::new(),
	};
	for a in argv {
		match a.as_str() {
			"--int" | "--float" | "--bool" | "--datetime" | "--string" | "--raw" => {
				o.kind = a[2..].to_string();
			}
			"--array" => o.array = true,
			"--write" | "-w" => o.write = true,
			_ if a.starts_with("--default=") => {
				o.default = Some(a["--default=".len()..].to_string());
				o.on_bad = "default".into();
			}
			_ if a.starts_with("--on-bad=") => {
				let v = &a["--on-bad=".len()..];
				if !matches!(v, "error" | "default" | "flag") {
					return Err(format!("bad --on-bad value: {}", v));
				}
				o.on_bad = v.to_string();
			}
			_ if a.starts_with("--strictness=") => {
				let v = &a["--strictness=".len()..];
				o.strictness = Strictness::from_arg(v)
					.ok_or_else(|| format!("bad --strictness value: {}", v))?;
			}
			_ if a.starts_with('-') && a.len() > 1 => {
				return Err(format!("unknown option: {}", a));
			}
			_ => o.args.push(a.clone()),
		}
	}
	Ok(o)
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
	let text = match read_input(file) {
		Ok(t) => t,
		Err(e) => {
			eprintln!("{}", e);
			return 1;
		}
	};
	let doc = match load(&text, o.strictness) {
		Ok(d) => d,
		Err(code) => return code,
	};
	let (lines, status): (Vec<String>, Status) = if o.array {
		match o.kind.as_str() {
			"int" => {
				let r = doc.read_int_array(path);
				(r.value.iter().map(|v| v.to_string()).collect(), r.status)
			}
			"float" => {
				let r = doc.read_float_array(path);
				(r.value.iter().map(|v| v.to_string()).collect(), r.status)
			}
			"bool" => {
				let r = doc.read_bool_array(path);
				(r.value.iter().map(|v| v.to_string()).collect(), r.status)
			}
			"datetime" => {
				let r = doc.read_datetime_array(path);
				(r.value.iter().map(|v| v.to_string()).collect(), r.status)
			}
			"raw" => {
				eprintln!("--raw has no --array form");
				return 1;
			}
			_ => {
				let r = doc.read_string_array(path);
				(r.value, r.status)
			}
		}
	} else {
		match o.kind.as_str() {
			"int" => {
				let r = doc.read_int(path);
				(vec![r.value.to_string()], r.status)
			}
			"float" => {
				let r = doc.read_float(path);
				(vec![r.value.to_string()], r.status)
			}
			"bool" => {
				let r = doc.read_bool(path);
				(vec![r.value.to_string()], r.status)
			}
			"datetime" => {
				let r = doc.read_datetime(path);
				(vec![r.value.to_string()], r.status)
			}
			"raw" => {
				let r = doc.read_raw(path);
				(vec![r.value], r.status)
			}
			_ => {
				let r = doc.read_string(path);
				(vec![r.value], r.status)
			}
		}
	};
	match (status, o.on_bad.as_str()) {
		(Status::Good, _) | (Status::Empty, "flag") => {
			for l in lines {
				println!("{}", l);
			}
			status_code(status)
		}
		(_, "default") => {
			println!("{}", o.default.clone().unwrap_or_default());
			0
		}
		(_, "error") => {
			eprintln!("{}: {:?}", path, status);
			status_code(status)
		}
		(_, _) => {
			// flag: print the zero/empty value anyway; the exit code carries the status
			for l in lines {
				println!("{}", l);
			}
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
	let text = match read_input(file) {
		Ok(t) => t,
		Err(e) => {
			eprintln!("{}", e);
			return 1;
		}
	};
	let canonical = Document::parse(&text).to_canonical();
	if o.write && file != "-" {
		if let Err(e) = std::fs::write(file, &canonical) {
			eprintln!("{}: {}", file, e);
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
	match Document::parse_with(&text, o.strictness) {
		Ok(doc) => {
			for d in doc.diagnostics() {
				println!("line {}: {:?}: {}", d.line, d.severity, d.message);
			}
			println!("ok ({} diagnostic(s))", doc.diagnostics().len());
			0
		}
		Err(e) => {
			for d in &e.diagnostics {
				println!("line {}: {:?}: {}", d.line, d.severity, d.message);
			}
			println!("{}", e);
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
	let text = match read_input(file) {
		Ok(t) => t,
		Err(e) => {
			eprintln!("{}", e);
			return 1;
		}
	};
	let doc = match load(&text, o.strictness) {
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

fn main() -> ExitCode {
	let argv: Vec<String> = std::env::args().skip(1).collect();
	if argv.is_empty() || argv.iter().any(|a| a == "-h" || a == "--help") {
		print!("{}", HELP);
		return ExitCode::from(if argv.is_empty() { 1 } else { 0 });
	}
	if argv.iter().any(|a| a == "-V" || a == "--version") {
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
	let code = match cmd.as_str() {
		"get" => do_get(&o),
		"fmt" => do_fmt(&o),
		"check" => do_check(&o),
		"count" => do_enum(&o, true),
		"instances" => do_enum(&o, false),
		other => {
			eprintln!("unknown command: {} (see --help)", other);
			1
		}
	};
	ExitCode::from(code)
}
