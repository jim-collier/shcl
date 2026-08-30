// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

//! `shcl` CLI - the Tier 1 command binding. POSIX sh and PowerShell wrap this,
//! so the exit codes and flags below are a stable surface, not conveniences.

use shcl::{
	Diagnostic, Document, SaveError, Severity, Status, Strictness, generate, parse_datetime,
	suppress_declared_reopens, suppress_declared_repeats,
};
use std::process::ExitCode;

const HELP: &str = "\
shcl - Simple Hierarchical Config Language (reference CLI)

Usage:
  shcl get [type] [options] FILE PATH    read one value (or array) at a path
  shcl set [--write|-w] [options] FILE   apply edits (--set, or ops on stdin);
                                         print canonical (or rewrite FILE in
                                         place with --write)
  shcl fmt [--write|-w] FILE             print the canonical form (or rewrite
                                         FILE in place with --write)
  shcl check [options] FILE              load and print diagnostics
                                         (--schema=SCHEMA also validates FILE
                                         against a schema, itself a .shcl file)
  shcl init [--no-banner] --schema=S     print a commented starter config
                                         from a schema (required fields live,
                                         optional commented, wildcards noted)
  shcl count [options] FILE PATH         number of instances at a path
  shcl instances [options] FILE PATH     instance values at a path, one per line
  shcl help | version                    this help, or the version (also
                                         -h/--help, -v/-V/--version)
  shcl about | donate                    what shcl is, or how to support it
                                         (also --about, --donate)

set edits FILE, the base document. Values go in as repeatable --set PATH=VALUE
(data) or --set-literal PATH=TEXT (value syntax, so arrays work) options, which
persist with --write; given either, no ops are read from stdin. Raw blocks,
set-only-if-absent and removal go in as a write-ops script on stdin, one op per
line, tab-separated. FILE '-' follows stdin: the document when an option holds
the edits, an empty base when the ops script has stdin instead. With --write,
a FILE that does not exist yet is created. PATH ends at the first '=' outside
quotes and brackets, so a selector may hold one. Ops:
  int|float|bool|string|datetime<TAB>PATH<TAB>VALUE       set a scalar
  <type>-array<TAB>PATH<TAB>V1<TAB>V2...                  set an inline array
  <type>[-array]-default<TAB>...                          set only if absent
  literal[-default]<TAB>PATH<TAB>TEXT                     set from value syntax
  raw<TAB>PATH<TAB>INFO<TAB>CONTENT                       set a raw block
  empty<TAB>PATH   comment<TAB>PATH<TAB>TEXT   remove<TAB>PATH
string/raw values decode \\n \\t \\\\; a line starting with # is a script comment.

Types (get only; default --string):
  --int --float --bool --datetime --string --raw --rawinfo
  --array                                read the value as an array of the type
  --rawinfo reads a raw block's info-string (the fence tag), not its content

Options (the subcommands each belongs to are in parentheses):
  --default=VALUE                        (get) value to print when the read is
                                         not Good (implies --on-bad=default; for
                                         arrays, substituted per bad slot)
  --on-bad=error|default|flag            (get) error: fail loudly; default:
                                         print the default; flag: print the
                                         value anyway and report via exit code
                                         (the default)
  --slots                                (get) prefix each line with its slot
                                         status and a tab (per element, or per
                                         wildcard slot)
  --no-banner                            (init) leave out the footer naming the
                                         format and pointing at its spec
  --lossy                                (fmt/set) with --write, rewrite even
                                         when the load dropped lines this write
                                         would delete; without it the write
                                         refuses and nothing is changed
  --strictness=loose|standard|strict     (all but init) or 1|2|3 (default
                                         standard)
  --schema=SCHEMA                        (check/init) validate FILE against a
                                         schema; adds V### diagnostics
  --layer=FILE                           (get/fmt/count/instances/set) merge a
                                         lower-priority layer under FILE;
                                         repeatable, earlier = lower priority
  --set=PATH=VALUE                       (get/fmt/count/instances/set) override
                                         one path as the top layer, after all
                                         files; repeatable. On 'set'
                                         it is an edit to the document itself,
                                         so it persists with --write. VALUE
                                         goes in as data: its type still
                                         follows the text (8 is an int), but a
                                         comma or quote in it is content, not
                                         syntax
  --set-literal=PATH=TEXT                (same subcommands) as --set, except
                                         TEXT goes in as value
                                         syntax the way a file spells it, so
                                         'ports=80, 443' writes a two-element
                                         array. An unquoted # ends the value;
                                         text spanning lines is rejected

Value options accept either spelling: --default=VALUE or --default VALUE. In
the space form the next argument is taken as the value whatever it looks like,
so --default --int reads --int as the default. Use -- to end the options when a
FILE or PATH begins with a dash.
An option a subcommand does not use is a usage error, not ignored. Also
refused: --write with --layer; --write with --set outside 'set'; --lossy
without --write; --layer=- on 'set'; --array with --raw or --rawinfo; '-'
named more than once across FILE, --layer and --schema.
fmt and set print the load's diagnostics to stderr along with the canonical
document. An in-place write also refuses when the load dropped content the
rewrite would delete (--lossy overrides).
FILE may be '-' for stdin. With --layer, FILE is the highest file layer and
each --layer is merged under it in order; --set applies last. 'fmt' with
layers prints the merged canonical document.

Exit codes: 0 good, 1 usage or I/O error, 2 empty, 3 not found, 4 bad type,
5 multiple instances, 6 check failed, strict load failed, or init's schema
has faults, 7 in-place write refused (--lossy overrides).
";

// About and donate are stdout, so they are byte-for-byte contracts across the
// bindings the same way the help text and the init banner are. The version
// interpolates from the crate so it cannot drift from Cargo.toml.
const ABOUT: &str = concat!(
	"shcl v",
	env!("CARGO_PKG_VERSION"),
	"
Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞).
Project: https://github.com/jim-collier/shcl
Licensed under the MIT License. Full text at:
  https://spdx.org/licenses/MIT.html
No warranty.

Simple Hierarchical Config Language. Forgiving to write, predictable to read.
Types live in your code, not in the file, so nothing is guessed at parse time.
One broken line is skipped with a note instead of taking down the whole file.
"
);

const DONATE: &str = "\
shcl is free software under the MIT License, and stays that way.

If it saves you time and you want to give something back:
  https://github.com/sponsors/jim-collier

A star on the project, a clear bug report, or a mention to someone who needs it
are worth just as much.
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

/// One `--set`/`--set-literal` override. Both spellings share a list so they
/// apply in the order given, which is what decides the winner when two target
/// the same path.
struct Set {
	path: String,
	value: String,
	literal: bool,
}

impl Set {
	fn apply(&self, doc: &mut Document) -> bool {
		if self.literal {
			doc.set_literal(&self.path, &self.value)
		} else {
			doc.set_string(&self.path, &self.value)
		}
	}
	fn opt(&self) -> &'static str {
		if self.literal {
			"--set-literal"
		} else {
			"--set"
		}
	}
}

#[derive(Clone, Copy, PartialEq)]
enum Kind {
	Int,
	Float,
	Bool,
	Datetime,
	String,
	Raw,
	RawInfo,
}

impl Kind {
	fn from_opt(opt: &str) -> Option<Kind> {
		Some(match opt {
			"--int" => Kind::Int,
			"--float" => Kind::Float,
			"--bool" => Kind::Bool,
			"--datetime" => Kind::Datetime,
			"--string" => Kind::String,
			"--raw" => Kind::Raw,
			"--rawinfo" => Kind::RawInfo,
			_ => return None,
		})
	}
	fn name(self) -> &'static str {
		match self {
			Kind::Int => "int",
			Kind::Float => "float",
			Kind::Bool => "bool",
			Kind::Datetime => "datetime",
			Kind::String => "string",
			Kind::Raw => "raw",
			Kind::RawInfo => "rawinfo",
		}
	}
}

#[derive(Clone, Copy, PartialEq)]
enum OnBad {
	Error,
	Default,
	Flag,
}

struct Opts {
	kind: Kind,
	array: bool,
	slots: bool,
	default: Option<String>,
	on_bad: OnBad,
	strictness: Strictness,
	write: bool,
	lossy: bool,
	no_banner: bool,
	schema: Option<String>,
	layers: Vec<String>,     // lower-priority layers, in listed order
	sets: Vec<Set>,          // final override layer, in the order given
	args: Vec<String>,       // positional: FILE [PATH]
	seen: Vec<&'static str>, // canonical names of options given, for per-command validation
}

/// Did the command line ask for one of the informational outputs? Only tokens
/// in option position count: the value of a value-taking option and anything
/// after `--` are data (a FILE or PATH spelled `-h` needs the `--` anyway,
/// since the option parser would refuse it). Scanning values too once let a
/// read of a missing path answer with the help text and exit 0.
fn asked_for(argv: &[String]) -> Option<&'static str> {
	let mut i = 0;
	while i < argv.len() {
		let a = argv[i].as_str();
		match a {
			"-h" | "--help" => return Some("help"),
			"-v" | "-V" | "--version" => return Some("version"),
			"--about" => return Some("about"),
			"--donate" => return Some("donate"),
			"--" => return None,
			"--default" | "--on-bad" | "--strictness" | "--schema" | "--layer" | "--set"
			| "--set-literal" => i += 1,
			_ => {}
		}
		i += 1;
	}
	None
}

/// PATH=VALUE at the first `=` outside quotes and brackets, so a selector
/// holding one (`x[a=b].c=1`) still addresses its instance.
fn split_set(arg: &str) -> Option<(&str, &str)> {
	let bytes = arg.as_bytes();
	let mut in_quote: Option<u8> = None;
	let mut depth = 0usize;
	let mut i = 0;
	while i < bytes.len() {
		let b = bytes[i];
		if b == b'\\' {
			i += 2;
			continue;
		}
		match in_quote {
			Some(q) if b == q => in_quote = None,
			Some(_) => {}
			None => match b {
				b'"' | b'\'' => in_quote = Some(b),
				b'[' => depth += 1,
				b']' => depth = depth.saturating_sub(1),
				b'=' if depth == 0 => return Some((&arg[..i], &arg[i + 1..])),
				_ => {}
			},
		}
		i += 1;
	}
	None
}

fn parse_opts(argv: &[String]) -> Result<Opts, String> {
	let mut o = Opts {
		kind: Kind::String,
		array: false,
		slots: false,
		default: None,
		on_bad: OnBad::Flag,
		strictness: Strictness::Standard,
		write: false,
		lossy: false,
		no_banner: false,
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
		// Everything after `--` is positional, so a file or path may begin
		// with a dash.
		if a == "--" {
			o.args.extend(argv[i + 1..].iter().cloned());
			return Ok(o);
		}
		if let Some(k) = Kind::from_opt(a) {
			o.kind = k;
			o.seen.push("--<type>");
			i += 1;
			continue;
		}
		match a {
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
			"--lossy" => {
				o.lossy = true;
				o.seen.push("--lossy");
			}
			"--no-banner" => {
				o.no_banner = true;
				o.seen.push("--no-banner");
			}
			"--default" | "--on-bad" | "--strictness" | "--schema" | "--layer" | "--set"
			| "--set-literal" => {
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
			_ if a.starts_with("--set-literal=") => {
				set_value_opt(&mut o, "--set-literal", &a[14..])?
			}
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
			o.on_bad = OnBad::Default;
			o.seen.push("--default");
		}
		"--on-bad" => {
			o.on_bad = match v.to_ascii_lowercase().as_str() {
				"error" => OnBad::Error,
				"default" => OnBad::Default,
				"flag" => OnBad::Flag,
				_ => return Err(format!("bad --on-bad value: {}", v)),
			};
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
		"--set" | "--set-literal" => {
			let (p, val) = split_set(v).filter(|(p, _)| !p.is_empty()).ok_or_else(|| {
				format!(
					"bad {} value (want PATH=VALUE, quotes and brackets balanced): {}",
					name, v
				)
			})?;
			o.sets.push(Set {
				path: p.to_string(),
				value: val.to_string(),
				literal: name == "--set-literal",
			});
			o.seen.push(if name == "--set-literal" {
				"--set-literal"
			} else {
				"--set"
			});
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
			"--set-literal",
		],
		"set" => &[
			"--strictness",
			"--layer",
			"--set",
			"--set-literal",
			"--write",
			"--lossy",
		],
		"fmt" => &[
			"--write",
			"--lossy",
			"--strictness",
			"--layer",
			"--set",
			"--set-literal",
		],
		"check" => &["--strictness", "--schema"],
		"init" => &["--schema", "--no-banner"],
		"count" | "instances" => &["--strictness", "--layer", "--set", "--set-literal"],
		_ => &[],
	};
	for s in &o.seen {
		if !allowed.contains(s) {
			if *s == "--<type>" {
				eprintln!("type options are not valid for {} (see --help)", cmd);
			} else if cmd == "init" && *s == "--strictness" {
				// Deliberate, not an oversight: the schema is a program artifact,
				// so it always loads at Standard - the same rule `check --schema`
				// follows for the schema half.
				eprintln!(
					"option --strictness not valid for init: a schema always loads at standard strictness, being a program artifact rather than user data"
				);
			} else if cmd == "check" && matches!(*s, "--layer" | "--set" | "--set-literal") {
				// The one refusal a user is likely to want anyway: check reports
				// line numbers, and a merged document has no single file to
				// number against. Naming the pipeline turns a dead end into a
				// one-liner.
				eprintln!(
					"option {} not valid for check: diagnostics cite line numbers, which a merged document has none of. Pipe instead: shcl fmt {} ... FILE | shcl check --schema=SCHEMA -",
					s, s
				);
			} else {
				eprintln!("option {} not valid for {} (see --help)", s, cmd);
			}
			return Err(1);
		}
	}
	// Writing back the merged document would fold the lower layers permanently
	// into the top file, which is the opposite of what layering is for. On 'set'
	// the --set values are edits to the document rather than a layer over it, so
	// persisting them is the whole point; everywhere else they stay ephemeral.
	if o.write && !o.layers.is_empty() {
		eprintln!("--write cannot be combined with --layer (see --help)");
		return Err(1);
	}
	if o.write && !o.sets.is_empty() && cmd != "set" {
		eprintln!("--write cannot be combined with --set (see --help)");
		return Err(1);
	}
	// --lossy only overrides the in-place write's refusal, so on its own it says
	// nothing and would read as protection the command never had.
	if o.lossy && !o.write {
		eprintln!("--lossy is only meaningful with --write (see --help)");
		return Err(1);
	}
	// The ops script already has stdin, so a layer cannot read it too.
	if cmd == "set" && o.layers.iter().any(|l| l == "-") {
		eprintln!("--layer=- is not valid for set (stdin carries the ops script or the document)");
		return Err(1);
	}
	// Stdin reads once; a second '-' would silently get an empty document.
	let stdin_uses = o.layers.iter().filter(|l| *l == "-").count()
		+ usize::from(o.schema.as_deref() == Some("-"))
		+ usize::from(o.args.first().map(|f| f == "-").unwrap_or(false));
	if stdin_uses > 1 {
		eprintln!("'-' (stdin) can be named only once across FILE, --layer and --schema");
		return Err(1);
	}
	Ok(())
}

/// The per-binding wording behind a setter's bare `false`.
fn describe_refusal(doc: &Document, path: &str) -> &'static str {
	match doc.write_reason(path) {
		// The path itself is fine, so the value text must be what failed (a
		// literal that does not parse as one value).
		shcl::WriteReason::Writable => "the value text is not one value",
		shcl::WriteReason::BadPath => "not a usable path",
		shcl::WriteReason::ValueInPath => "a path with a value part cannot be written",
		shcl::WriteReason::Wildcard => "a wildcard path cannot be written",
		shcl::WriteReason::NoSuchIndex => "no instance at that index",
		shcl::WriteReason::TooDeep => "deeper than the nesting cap",
	}
}

/// The load's diagnostics, one line each, in the shape every command uses.
fn say_diagnostics(diags: &[Diagnostic]) {
	for d in diags {
		let space = if d.code.starts_with("V09") && d.code != "V099" {
			"schema line"
		} else {
			"line"
		};
		eprintln!(
			"{} {}: {:?}: {} {}",
			space, d.line, d.severity, d.code, d.message
		);
	}
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
	for s in &o.sets {
		if !s.apply(&mut doc) {
			eprintln!(
				"{}: cannot write {}: {}",
				s.opt(),
				s.path,
				describe_refusal(&doc, &s.path)
			);
			return Err(1);
		}
	}
	Ok(doc)
}

/// The in-place half of `fmt`/`set`. Overwriting the source is the one place a
/// recovered load turns destructive, so the diagnostics go out even though the
/// command succeeded, and the save runs through the library's own gate rather
/// than a second copy of the rule - the CLI and a consumer program cannot then
/// disagree about which rewrites are safe.
fn write_back(doc: &Document, file: &str, o: &Opts) -> u8 {
	let r = if o.lossy {
		doc.save_file_lossy(file)
	} else {
		doc.save_file(file)
	};
	match r {
		Ok(()) => 0,
		Err(SaveError::Refused { lost, .. }) => {
			// The rule stays in the library; only the wording is the CLI's,
			// because the override a user has here is a flag, not a function.
			eprintln!(
				"{}: refusing to rewrite: the load dropped {} line(s)/value(s) this write would delete (--lossy overrides)",
				file, lost
			);
			7
		}
		Err(e) => {
			eprintln!("{}", e);
			1
		}
	}
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
			say_diagnostics(&e.diagnostics);
			let errors = e
				.diagnostics
				.iter()
				.filter(|d| d.severity == Severity::Error)
				.count();
			eprintln!("strict load failed: {} error diagnostic(s)", errors);
			Err(6)
		}
	}
}

/// One value read, formatted for the shell: scalars print as one line, arrays
/// one element per line.
fn do_get(o: &Opts) -> u8 {
	let [file, path] = o.args.as_slice() else {
		eprintln!("usage: shcl get [type] [options] FILE PATH (see --help)");
		return 1;
	};
	let doc = match load_layered(o, file) {
		Ok(d) => d,
		Err(code) => return code,
	};
	let (lines, status, slots): (Vec<String>, Status, Vec<Status>) = if o.array {
		match o.kind {
			Kind::Int => {
				let r = doc.read_int_array(path);
				(
					r.value.iter().map(|v| v.to_string()).collect(),
					r.status,
					r.slots,
				)
			}
			Kind::Float => {
				let r = doc.read_float_array(path);
				(
					r.value.iter().map(|v| v.to_string()).collect(),
					r.status,
					r.slots,
				)
			}
			Kind::Bool => {
				let r = doc.read_bool_array(path);
				(
					r.value.iter().map(|v| v.to_string()).collect(),
					r.status,
					r.slots,
				)
			}
			Kind::Datetime => {
				let r = doc.read_datetime_array(path);
				(
					r.value.iter().map(|v| v.to_string()).collect(),
					r.status,
					r.slots,
				)
			}
			Kind::Raw | Kind::RawInfo => {
				eprintln!("--{} has no --array form", o.kind.name());
				return 1;
			}
			Kind::String => {
				let r = doc.read_string_array(path);
				(r.value, r.status, r.slots)
			}
		}
	} else {
		match o.kind {
			Kind::Int => {
				let r = doc.read_int(path);
				(vec![r.value.to_string()], r.status, Vec::new())
			}
			Kind::Float => {
				let r = doc.read_float(path);
				(vec![r.value.to_string()], r.status, Vec::new())
			}
			Kind::Bool => {
				let r = doc.read_bool(path);
				(vec![r.value.to_string()], r.status, Vec::new())
			}
			Kind::Datetime => {
				let r = doc.read_datetime(path);
				(vec![r.value.to_string()], r.status, Vec::new())
			}
			Kind::Raw => {
				let r = doc.read_raw(path);
				(vec![r.value], r.status, Vec::new())
			}
			Kind::RawInfo => {
				let r = doc.read_raw_info(path);
				(vec![r.value], r.status, Vec::new())
			}
			Kind::String => {
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
	// Why the read failed is worth saying even when the exit code already
	// carries it: at the default mode the user otherwise gets an empty line, a
	// nonzero code, and nothing to go on. Stdout is untouched - this only ever
	// goes to stderr. Two silences are deliberate: `default` mode, because a
	// caller who supplied a fallback has already said the miss is expected, and
	// Empty outside `error` mode, because an empty value is a legitimate answer
	// here rather than a failure - the same reason `ok()` counts it as fine.
	let say = status != Status::Good
		&& o.on_bad != OnBad::Default
		&& (status != Status::Empty || o.on_bad == OnBad::Error);
	if say {
		let type_name = if o.array {
			format!("{} array", o.kind.name())
		} else {
			o.kind.name().to_string()
		};
		let reason = match status {
			Status::BadType => match doc.read_string(path).raw {
				Some(raw) => format!("value {} is not a valid {}", quoted(&raw), type_name),
				None => format!("value is not a valid {}", type_name),
			},
			Status::NotFound => "no value at that path".to_string(),
			Status::Empty => "the value is empty".to_string(),
			Status::Multiple => "the path matches multiple instances".to_string(),
			Status::Good => String::new(), // handled above; keep the match total
		};
		eprintln!(
			"cannot read {} as {}: {} (in {})",
			path, type_name, reason, file
		);
	}
	match (status, o.on_bad) {
		(Status::Good, _) | (Status::Empty, OnBad::Flag) => {
			emit(&lines);
			status_code(status)
		}
		(_, OnBad::Default) => {
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
		// The message already went to stderr above; error mode differs only in
		// printing nothing on stdout.
		(_, OnBad::Error) => status_code(status),
		(_, OnBad::Flag) => {
			// print the zero/empty value anyway; the exit code carries the status
			emit(&lines);
			status_code(status)
		}
	}
}

/// The source text, quoted for a message: one line whatever it holds, with
/// the same escapes in every binding.
fn quoted(s: &str) -> String {
	let mut out = String::with_capacity(s.len() + 2);
	out.push('"');
	for c in s.chars() {
		match c {
			'"' => out.push_str("\\\""),
			'\\' => out.push_str("\\\\"),
			'\n' => out.push_str("\\n"),
			'\r' => out.push_str("\\r"),
			'\t' => out.push_str("\\t"),
			c if (c as u32) < 0x20 || c == '\x7f' => {
				out.push_str(&format!("\\u{{{:x}}}", c as u32))
			}
			c => out.push(c),
		}
	}
	out.push('"');
	out
}

fn do_fmt(o: &Opts) -> u8 {
	let [file] = o.args.as_slice() else {
		eprintln!("usage: shcl fmt [--write|-w] [options] FILE (see --help)");
		return 1;
	};
	if o.write && file == "-" {
		eprintln!("fmt --write cannot rewrite stdin; drop --write to print, or pass a FILE");
		return 1;
	}
	let doc = match load_layered(o, file) {
		Ok(d) => d,
		Err(code) => return code,
	};
	// Printing the canonical form drops what the load dropped, the same as a
	// rewrite does, so the diagnostics go out either way.
	say_diagnostics(doc.diagnostics());
	if o.write {
		return write_back(&doc, file, o);
	}
	print!("{}", doc.to_canonical());
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
	let pbool = |s: &str| match s {
		"true" => Ok(true),
		"false" => Ok(false),
		_ => Err(format!("bad bool: {}", s)),
	};
	let arr = &f[2.min(f.len())..];
	let wrote = match f.first().copied().unwrap_or("") {
		"int" => doc.set_int(path, pint(val())?),
		"float" => doc.set_float(path, pflt(val())?),
		"bool" => doc.set_bool(path, pbool(val())?),
		"string" => doc.set_string(path, &unescape_ops(val())),
		"datetime" => {
			let dt = parse_datetime(val()).ok_or_else(|| format!("bad datetime: {}", val()))?;
			doc.set_datetime(path, &dt)
		}
		"literal" => doc.set_literal(path, val()),
		"literal-default" => doc.set_literal_default(path, val()),
		"int-default" => doc.set_int_default(path, pint(val())?),
		"float-default" => doc.set_float_default(path, pflt(val())?),
		"bool-default" => doc.set_bool_default(path, pbool(val())?),
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
		"bool-array" => doc.set_bool_array(
			path,
			&arr.iter()
				.map(|s| pbool(s))
				.collect::<Result<Vec<_>, _>>()?,
		),
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
		"bool-array-default" => doc.set_bool_array_default(
			path,
			&arr.iter()
				.map(|s| pbool(s))
				.collect::<Result<Vec<_>, _>>()?,
		),
		"string-array-default" => {
			let owned: Vec<String> = arr.iter().map(|s| unescape_ops(s)).collect();
			doc.set_string_array_default(
				path,
				&owned.iter().map(|s| s.as_str()).collect::<Vec<_>>(),
			)
		}
		"datetime-array-default" => {
			let dts: Vec<_> = arr
				.iter()
				.map(|s| parse_datetime(s).ok_or_else(|| format!("bad datetime: {}", s)))
				.collect::<Result<Vec<_>, _>>()?;
			doc.set_datetime_array_default(path, &dts)
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
		return Err(format!(
			"cannot write {}: {}",
			path,
			describe_refusal(doc, path)
		));
	}
	Ok(())
}

fn do_set(o: &Opts) -> u8 {
	let [file] = o.args.as_slice() else {
		eprintln!("usage: shcl set [--write|-w] [options] FILE (see --help)");
		return 1;
	};
	if o.write && file == "-" {
		eprintln!("set --write cannot rewrite stdin; drop --write to print, or pass a FILE");
		return 1;
	}
	// Base doc: with the edits given as options no ops script is read, so a '-'
	// file is the document on stdin the way it is everywhere else; only when
	// stdin is the ops script does '-' mean an empty base. Reading neither threw
	// a piped document away at exit 0.
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
	// --write names the file this command produces, so a FILE that is not there
	// yet is a create and the edits land in a new document. Only under --write,
	// and only when nothing is at the path at all: without --write there is
	// nothing to create, and a file that exists but cannot be read is still an
	// error rather than something to quietly write over.
	let creating = o.write && file != "-" && !std::path::Path::new(file).exists();
	let base_text = if creating || (file == "-" && o.sets.is_empty()) {
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
	for s in &o.sets {
		if !s.apply(&mut doc) {
			eprintln!(
				"{}: cannot write {}: {}",
				s.opt(),
				s.path,
				describe_refusal(&doc, &s.path)
			);
			return 1;
		}
	}
	// --set carries the edits, so stdin is left alone: reading it here would
	// block on the console for anyone who passed edits as options.
	let mut ops = String::new();
	if o.sets.is_empty() {
		use std::io::Read;
		// Say so before blocking. With nothing on stdin this used to sit there
		// silently, which reads as a hang rather than as a prompt; the note is
		// unconditional so a pipeline and a terminal behave identically. The
		// program-name prefix marks it as a notice; errors carry none.
		eprintln!(
			"shcl: reading write-ops from stdin (one op per line, tab-separated; end with EOF)"
		);
		if let Err(e) = std::io::stdin().read_to_string(&mut ops) {
			eprintln!("stdin: {}", e);
			return 1;
		}
	}
	for (n, line) in ops.lines().enumerate() {
		let line = line.strip_suffix('\r').unwrap_or(line);
		if line.is_empty() || line.starts_with('#') {
			continue;
		}
		if let Err(e) = apply_op(&mut doc, line) {
			eprintln!("op line {}: {}", n + 1, e);
			return 1;
		}
	}
	say_diagnostics(doc.diagnostics());
	if o.write {
		return write_back(&doc, file, o);
	}
	print!("{}", doc.to_canonical());
	0
}

fn do_check(o: &Opts) -> u8 {
	let [file] = o.args.as_slice() else {
		eprintln!("usage: shcl check [options] FILE (see --help)");
		return 1;
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
						eprintln!(
							"schema line {}: {:?}: {} {}",
							d.line, d.severity, d.code, d.message
						);
					}
					diags.push(Diagnostic {
						line: 0,
						severity: Severity::Error,
						message: "schema failed to load".to_string(),
						code: "V099",
					});
				} else {
					diags.extend(doc.validate(&sdoc));
					suppress_declared_repeats(&sdoc, &mut diags);
					suppress_declared_reopens(&sdoc, &mut diags);
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
	}
	say_diagnostics(&diags);
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
	if !o.args.is_empty() {
		eprintln!("init takes no file argument (see --help)");
		return 1;
	}
	let Some(schema_file) = &o.schema else {
		eprintln!("init needs --schema=FILE (see --help)");
		return 1;
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
			eprintln!(
				"schema line {}: {:?}: {} {}",
				d.line, d.severity, d.code, d.message
			);
		}
		eprintln!("init: schema failed to load");
		// A broken schema is a config-semantics failure, not a usage error:
		// same exit as `check --schema` reporting it.
		return 6;
	}
	match generate(&sdoc, o.no_banner) {
		Ok(text) => {
			print!("{}", text);
			0
		}
		Err(faults) => {
			for d in &faults {
				eprintln!(
					"schema line {}: {:?}: {} {}",
					d.line, d.severity, d.code, d.message
				);
			}
			eprintln!("init: schema has faults");
			6
		}
	}
}

fn do_enum(o: &Opts, want_count: bool) -> u8 {
	let [file, path] = o.args.as_slice() else {
		let name = if want_count { "count" } else { "instances" };
		eprintln!("usage: shcl {} [options] FILE PATH (see --help)", name);
		return 1;
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

const COMMANDS: [&str; 7] = ["get", "set", "fmt", "check", "init", "count", "instances"];

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
		_ => do_enum(o, false),
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

/// Rust's runtime sets SIGPIPE to SIG_IGN, so a closed stdout comes back as an
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
	let asked = asked_for(&argv);
	// One convention: asking for the help - by name, by flag, or by asking for
	// nothing at all - prints it and succeeds. The blank lines separate the
	// block from the surrounding prompts. A bare run used to print the same
	// text unpadded and exit 1, which read as neither a help nor an error.
	if asked == Some("help") || first == Some("help") || argv.is_empty() {
		print!("\n{}\n", HELP);
		return ExitCode::from(0);
	}
	if asked == Some("version") || first == Some("version") {
		println!("shcl {}", env!("CARGO_PKG_VERSION"));
		return ExitCode::from(0);
	}
	if asked == Some("about") || first == Some("about") {
		print!("\n{}\n", ABOUT);
		return ExitCode::from(0);
	}
	if asked == Some("donate") || first == Some("donate") {
		print!("\n{}\n", DONATE);
		return ExitCode::from(0);
	}
	let cmd = argv[0].clone();
	if !COMMANDS.contains(&cmd.as_str()) {
		// Before the options are judged, so a typo in the command is reported
		// as that and not as an option the wrong command cannot take.
		if cmd.starts_with('-') && cmd != "--" {
			eprintln!("unknown option: {} (see --help)", cmd);
		} else {
			eprintln!("unknown command: {} (see --help)", cmd);
		}
		return ExitCode::from(1);
	}
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
