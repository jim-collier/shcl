//! Measures SHCL against JSON, YAML, TOML and XML on documents that hold the
//! same data in each format.
//!
//! The comparison holds the language constant at Rust: every format is parsed by
//! a mature native crate, built by the same compiler with the same flags. Mixing
//! languages would measure implementations, not formats.
//!
//! Each measurement runs in its own process, because peak resident memory is
//! only attributable that way - one process that parsed six documents tells you
//! nothing about what any one of them cost.
//!
//! Results go to a SHCL file, which is both the point and the dogfood: the file
//! is read, updated and pruned through this repo's own library.

mod bench;
mod model;

use model::{Fmt, Gen, Shape};
use std::collections::HashMap;
use std::io::Read as _;
use std::process::{Command, Stdio};

const HELP: &str = "\
shcl-comparison - measure SHCL against JSON, YAML, TOML and XML

Usage: shcl-comparison [options]

  --mib N          target size of each shape's SHCL encoding (default 16)
  --iters N        timed runs per measurement, best wins (default 3)
  --shape NAME     only this shape; repeatable (flat, deep, records, text)
  --entry KEY      only this library; repeatable (shcl, json, yaml, toml,
                   toml-edit, xml, xml-dom)
  --out FILE       results file to update (default results.shcl beside this tool)
  --keep-runs N    runs to retain in the results file, newest first (default 20)
  --no-record      print the table and write nothing
  --work DIR       where to generate documents (default a fresh temp dir)
  --keep           leave the generated documents behind
  --verify-units N units used for the pre-flight equivalence check (default 200)
  --no-verify      skip that check
  -h, --help       this text
";

struct Opts {
	mib: usize,
	iters: usize,
	shapes: Vec<Shape>,
	entries: Vec<&'static str>,
	out: String,
	keep_runs: usize,
	record: bool,
	work: Option<String>,
	keep: bool,
	verify_units: usize,
	verify: bool,
}

fn main() {
	let argv: Vec<String> = std::env::args().collect();

	// Worker mode: one measurement, one process, key=value on stdout.
	if argv.len() >= 5 && argv[1] == "--worker" {
		worker(&argv[2], &argv[3], argv[4].parse().unwrap_or(1));
		return;
	}

	let opts = match parse_args(&argv[1..]) {
		Ok(o) => o,
		Err(e) => {
			eprintln!("shcl-comparison: {e}");
			eprintln!("\n{HELP}");
			std::process::exit(2);
		}
	};
	std::process::exit(orchestrate(&opts));
}

fn parse_args(args: &[String]) -> Result<Opts, String> {
	let mut o = Opts {
		mib: 16,
		iters: 3,
		shapes: Vec::new(),
		entries: Vec::new(),
		out: format!("{}/results.shcl", env!("CARGO_MANIFEST_DIR")),
		keep_runs: 20,
		record: true,
		work: None,
		keep: false,
		verify_units: 200,
		verify: true,
	};
	let mut i = 0;
	while i < args.len() {
		let a = args[i].as_str();
		macro_rules! next {
			($opt:expr) => {{
				i += 1;
				args.get(i)
					.cloned()
					.ok_or_else(|| format!("{} wants a value", $opt))?
			}};
		}
		match a {
			"-h" | "--help" => {
				print!("{HELP}");
				std::process::exit(0);
			}
			"--mib" => {
				o.mib = next!("--mib")
					.parse()
					.map_err(|_| "--mib wants a positive integer".to_string())?
			}
			"--iters" => {
				o.iters = next!("--iters")
					.parse()
					.map_err(|_| "--iters wants a positive integer".to_string())?
			}
			"--shape" => {
				let v = next!("--shape");
				o.shapes
					.push(Shape::from_name(&v).ok_or(format!("unknown shape: {v}"))?);
			}
			"--entry" => {
				let v = next!("--entry");
				o.entries
					.push(bench::entry(&v).ok_or(format!("unknown entry: {v}"))?.key);
			}
			"--out" => o.out = next!("--out"),
			"--keep-runs" => {
				o.keep_runs = next!("--keep-runs")
					.parse()
					.map_err(|_| "--keep-runs wants an integer".to_string())?
			}
			"--no-record" => o.record = false,
			"--work" => o.work = Some(next!("--work")),
			"--keep" => o.keep = true,
			"--verify-units" => {
				o.verify_units = next!("--verify-units")
					.parse()
					.map_err(|_| "--verify-units wants an integer".to_string())?
			}
			"--no-verify" => o.verify = false,
			other => return Err(format!("unknown option: {other}")),
		}
		i += 1;
	}
	if o.mib == 0 || o.iters == 0 {
		return Err("--mib and --iters must be at least 1".into());
	}
	if o.shapes.is_empty() {
		o.shapes = Shape::all().to_vec();
	}
	if o.entries.is_empty() {
		o.entries = bench::ENTRIES.iter().map(|e| e.key).collect();
	}
	Ok(o)
}

//••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
// Generation
//••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

fn emit_unit(g: &mut Gen, o: &mut String, shape: Shape, i: usize) {
	match shape {
		Shape::Flat => {
			let (k, n) = model::flat_unit(i);
			g.entry(o, &k, &n);
		}
		Shape::Deep => {
			let (k, n) = model::deep_unit(i);
			g.entry(o, &k, &n);
		}
		Shape::Text => {
			let (k, n) = model::text_unit(i);
			g.entry(o, &k, &n);
		}
		Shape::Records => {
			let (name, f) = model::record_unit(i);
			g.record(o, Shape::Records.list_key().unwrap(), &name, &f);
		}
	}
}

/// Renders `units` units, or as many as it takes to reach `target` bytes.
fn render(shape: Shape, fmt: Fmt, units: usize, target: Option<usize>) -> (String, usize) {
	let mut g = Gen::new(fmt, shape);
	let mut out = String::with_capacity(target.unwrap_or(1 << 20) + (1 << 16));
	g.doc_open(&mut out);
	let mut n = 0;
	loop {
		match target {
			Some(t) => {
				if out.len() >= t {
					break;
				}
			}
			None => {
				if n >= units {
					break;
				}
			}
		}
		emit_unit(&mut g, &mut out, shape, n);
		n += 1;
	}
	g.doc_close(&mut out);
	(out, n)
}

//••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
// Orchestration
//••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

struct Row {
	shape: Shape,
	key: &'static str,
	bytes: usize,
	gzip_bytes: usize,
	m: bench::Measured,
}

fn orchestrate(o: &Opts) -> i32 {
	let mut rc = 0;

	if o.verify && !verify(o) {
		return 1;
	}

	let work = match &o.work {
		Some(d) => {
			let _ = std::fs::create_dir_all(d);
			d.clone()
		}
		None => match mktemp() {
			Ok(d) => d,
			Err(e) => {
				eprintln!("shcl-comparison: {e}");
				return 1;
			}
		},
	};
	let target = o.mib * 1024 * 1024;
	let exe = std::env::current_exe().unwrap_or_else(|_| "shcl-comparison".into());

	let mut rows: Vec<Row> = Vec::new();
	let mut units_by_shape: HashMap<&'static str, usize> = HashMap::new();

	for &shape in &o.shapes {
		// SHCL sets the unit count by hitting the size target; every other
		// format then encodes exactly that data, so size is a result rather
		// than an input.
		let (shcl_text, units) = render(shape, Fmt::Shcl, 0, Some(target));
		units_by_shape.insert(shape.name(), units);
		println!("generating {} ({units} units)", shape.name());

		let mut path_of: HashMap<&str, String> = HashMap::new();
		let mut bytes_of: HashMap<&str, usize> = HashMap::new();
		let mut gzip_of: HashMap<&str, usize> = HashMap::new();
		for fmt in Fmt::all() {
			let text = if fmt == Fmt::Shcl {
				shcl_text.clone()
			} else {
				render(shape, fmt, units, None).0
			};
			let path = format!("{work}/{}.{}", shape.name(), fmt.ext());
			if let Err(e) = std::fs::write(&path, &text) {
				eprintln!("shcl-comparison: cannot write {path}: {e}");
				return 1;
			}
			bytes_of.insert(fmt.name(), text.len());
			gzip_of.insert(fmt.name(), gzip_size(&path));
			path_of.insert(fmt.name(), path);
		}
		drop(shcl_text);

		for key in &o.entries {
			let fmt = bench::source_fmt(key);
			let path = &path_of[fmt.name()];
			let m = spawn_worker(&exe, key, path, o.iters);
			if let Some(e) = &m.failed {
				eprintln!("shcl-comparison: {} / {key}: {e}", shape.name());
				rc = 1;
			}
			rows.push(Row {
				shape,
				key,
				bytes: bytes_of[fmt.name()],
				gzip_bytes: gzip_of[fmt.name()],
				m,
			});
		}
		print_table(&rows[rows.len() - o.entries.len()..]);
	}

	if !o.keep && o.work.is_none() {
		let _ = std::fs::remove_dir_all(&work);
	} else {
		println!("\nkept generated documents in {work}");
	}

	if o.record {
		match record(o, &rows, &units_by_shape) {
			Ok(id) => println!("\nrecorded run {id} in {}", o.out),
			Err(e) => {
				eprintln!("shcl-comparison: cannot record results: {e}");
				rc = 1;
			}
		}
	}
	rc
}

/// Pre-flight: at a small scale, every library has to parse its own file and
/// find the same number of scalar values in it. Five encoders written from one
/// model can still disagree through an escaping mistake, and a size or speed
/// number taken from documents that are not the same data is worthless.
fn verify(o: &Opts) -> bool {
	let mut ok = true;
	for &shape in &o.shapes {
		let units = if shape == Shape::Deep {
			o.verify_units.div_ceil(50).max(1)
		} else {
			o.verify_units
		};
		let mut counts: Vec<(&str, u64)> = Vec::new();
		for fmt in Fmt::all() {
			let (text, _) = render(shape, fmt, units, None);
			for e in bench::ENTRIES {
				if bench::source_fmt(e.key) != fmt {
					continue;
				}
				let m = bench::run(e.key, &text, 1, true, 0);
				match m.failed {
					Some(err) => {
						eprintln!("verify: {}/{}: {err}", shape.name(), e.key);
						ok = false;
					}
					None => counts.push((e.key, m.scalars)),
				}
			}
		}
		// The four non-SHCL encodings carry the instance label as an ordinary
		// `name` field, which SHCL spells as the binding's own value. Same data,
		// one fewer scalar binding.
		let offset = if shape == Shape::Records {
			units as u64
		} else {
			0
		};
		let want = counts
			.iter()
			.find(|(k, _)| *k == "shcl")
			.map(|(_, n)| *n + offset);
		if let Some(want) = want {
			for (k, n) in &counts {
				let got = if *k == "shcl" { *n + offset } else { *n };
				if got != want {
					eprintln!(
						"verify: {}/{k}: {got} scalars, shcl says {want} - the documents are not the same data",
						shape.name()
					);
					ok = false;
				}
			}
		}
		if ok {
			println!(
				"verify: {} agrees across all encodings ({units} units, {} scalars)",
				shape.name(),
				want.unwrap_or(0)
			);
		}
	}
	ok
}

fn spawn_worker(exe: &std::path::Path, key: &str, path: &str, iters: usize) -> bench::Measured {
	let out = Command::new(exe)
		.args(["--worker", key, path, &iters.to_string()])
		.output();
	let out = match out {
		Ok(o) => o,
		Err(e) => {
			return bench::Measured {
				parse_secs: 0.0,
				emit_secs: None,
				emit_bytes: 0,
				roundtrip: false,
				scalars: 0,
				rss_bytes: 0,
				base_rss_bytes: 0,
				failed: Some(format!("cannot run worker: {e}")),
			};
		}
	};
	let text = String::from_utf8_lossy(&out.stdout);
	let mut kv: HashMap<&str, &str> = HashMap::new();
	for line in text.lines() {
		if let Some((k, v)) = line.split_once('=') {
			kv.insert(k, v);
		}
	}
	let num = |k: &str| -> f64 { kv.get(k).and_then(|v| v.parse().ok()).unwrap_or(0.0) };
	let int = |k: &str| -> u64 { kv.get(k).and_then(|v| v.parse().ok()).unwrap_or(0) };
	let failed = kv.get("failed").map(|s| (*s).to_string()).or_else(|| {
		if out.status.success() {
			None
		} else {
			Some(format!(
				"worker exit {:?}: {}",
				out.status.code(),
				String::from_utf8_lossy(&out.stderr)
					.lines()
					.next()
					.unwrap_or("")
			))
		}
	});
	bench::Measured {
		parse_secs: num("parse-secs"),
		emit_secs: kv.get("emit-secs").and_then(|v| v.parse().ok()),
		emit_bytes: int("emit-bytes") as usize,
		roundtrip: kv.get("roundtrip") == Some(&"true"),
		scalars: int("scalars"),
		rss_bytes: int("rss-bytes"),
		base_rss_bytes: int("base-rss-bytes"),
		failed,
	}
}

fn worker(key: &str, path: &str, iters: usize) {
	let src = match std::fs::read_to_string(path) {
		Ok(s) => s,
		Err(e) => {
			println!("failed=cannot read {path}: {e}");
			return;
		}
	};
	let base = bench::vmhwm(); // source text resident, nothing parsed yet
	let m = bench::run(key, &src, iters, false, base);
	if let Some(e) = m.failed {
		println!("failed={}", e.replace('\n', " "));
		return;
	}
	println!("parse-secs={:.6}", m.parse_secs);
	if let Some(s) = m.emit_secs {
		println!("emit-secs={s:.6}");
	}
	println!("emit-bytes={}", m.emit_bytes);
	println!("roundtrip={}", m.roundtrip);
	println!("scalars={}", m.scalars);
	println!("rss-bytes={}", m.rss_bytes);
	println!("base-rss-bytes={}", m.base_rss_bytes);
}

//••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
// Reporting
//••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

fn mib(bytes: u64) -> f64 {
	bytes as f64 / (1024.0 * 1024.0)
}

fn print_table(rows: &[Row]) {
	let mut shape = "";
	for r in rows {
		if r.shape.name() != shape {
			shape = r.shape.name();
			println!("\n{shape}");
			println!(
				"  {:<10} {:>9} {:>9} {:>9} {:>8} {:>9} {:>9} {:>7}  {:<16} round-trip",
				"library",
				"MiB",
				"gzip MiB",
				"parse s",
				"MiB/s",
				"emit s",
				"peak MiB",
				"x input",
				"retains"
			);
		}
		if r.m.failed.is_some() {
			println!(
				"  {:<10} {:>9}  FAILED: {}",
				r.key,
				"-",
				r.m.failed.as_deref().unwrap_or("")
			);
			continue;
		}
		let e = bench::entry(r.key).expect("entry exists");
		let peak = mib(r.m.rss_bytes);
		println!(
			"  {:<10} {:>9.2} {:>9.2} {:>9.3} {:>8.1} {:>9} {:>9.0} {:>7.1}  {:<16} {}",
			r.key,
			mib(r.bytes as u64),
			mib(r.gzip_bytes as u64),
			r.m.parse_secs,
			mib(r.bytes as u64) / r.m.parse_secs,
			r.m.emit_secs.map_or("-".to_string(), |s| format!("{s:.3}")),
			peak,
			r.m.rss_bytes as f64 / r.bytes as f64,
			e.retains,
			if r.m.emit_secs.is_none() {
				"-"
			} else if r.m.roundtrip {
				"byte-identical"
			} else {
				"reformatted"
			},
		);
	}
}

//••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
// Recording, through this repo's own library
//••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

fn record(o: &Opts, rows: &[Row], units: &HashMap<&'static str, usize>) -> Result<String, String> {
	let (mut doc, status) = shcl::Document::load_file(&o.out);
	match status {
		shcl::FileStatus::Clean | shcl::FileStatus::NotFound => {}
		other => return Err(format!("{} is unusable ({other:?})", o.out)),
	}

	let id = run_id();
	let vers = lock_versions();
	let run = format!("run[{id}]");
	macro_rules! set {
		($path:expr, $v:expr) => {
			let _ = doc.set_string(&$path, $v);
		};
	}

	set!(
		format!("{run}.tool"),
		&format!("shcl-comparison {}", env!("CARGO_PKG_VERSION"))
	);
	set!(
		format!("{run}.shcl-version"),
		vers.get("shcl").map_or("?", String::as_str)
	);
	set!(format!("{run}.host-cpu"), &cpu_model());
	set!(format!("{run}.os"), &uname());
	set!(
		format!("{run}.notes"),
		"same data in every format; one process per measurement; best of N runs"
	);
	let _ = doc.set_int(
		&format!("{run}.host-cores"),
		std::thread::available_parallelism().map_or(0, |n| n.get() as i64),
	);
	let _ = doc.set_int(&format!("{run}.target-mib"), o.mib as i64);
	let _ = doc.set_int(&format!("{run}.iterations"), o.iters as i64);

	for r in rows {
		let sh = format!("{run}.shape[{}]", r.shape.name());
		let _ = doc.set_int(
			&format!("{sh}.units"),
			*units.get(r.shape.name()).unwrap_or(&0) as i64,
		);
		let lib = format!("{sh}.library[{}]", r.key);
		let e = bench::entry(r.key).expect("entry exists");
		let about = format!("{run}.library[{}]", r.key);
		set!(format!("{about}.format"), e.format);
		set!(format!("{about}.crate"), e.library);
		set!(
			format!("{about}.version"),
			vers.get(e.library).map_or("?", String::as_str)
		);
		set!(format!("{about}.retains"), e.retains);
		set!(format!("{about}.note"), e.note);
		if let Some(f) = &r.m.failed {
			set!(format!("{lib}.failed"), f);
			continue;
		}
		let _ = doc.set_int(&format!("{lib}.bytes"), r.bytes as i64);
		let _ = doc.set_int(&format!("{lib}.gzip-bytes"), r.gzip_bytes as i64);
		let _ = doc.set_float(&format!("{lib}.parse-secs"), round6(r.m.parse_secs));
		let _ = doc.set_float(
			&format!("{lib}.parse-mib-per-sec"),
			round2(mib(r.bytes as u64) / r.m.parse_secs),
		);
		match r.m.emit_secs {
			Some(s) => {
				let _ = doc.set_float(&format!("{lib}.emit-secs"), round6(s));
				let _ = doc.set_int(&format!("{lib}.emit-bytes"), r.m.emit_bytes as i64);
				let _ = doc.set_bool(&format!("{lib}.roundtrip-identical"), r.m.roundtrip);
			}
			None => {
				set!(
					format!("{lib}.emit-secs"),
					"n/a - the library has no writer"
				);
			}
		}
		let _ = doc.set_float(&format!("{lib}.peak-rss-mib"), round2(mib(r.m.rss_bytes)));
		let _ = doc.set_float(
			&format!("{lib}.model-rss-mib"),
			round2(mib(r.m.rss_bytes.saturating_sub(r.m.base_rss_bytes))),
		);
		let _ = doc.set_float(
			&format!("{lib}.rss-per-input-byte"),
			round2(r.m.rss_bytes as f64 / r.bytes as f64),
		);
	}

	prune(&mut doc, o.keep_runs);
	doc.save_file(&o.out).map_err(|e| format!("{e:?}"))?;
	Ok(id)
}

/// Run ids sort chronologically, so keeping the newest is a tail of the sorted
/// instance list.
fn prune(doc: &mut shcl::Document, keep: usize) {
	if keep == 0 {
		return;
	}
	let mut ids = doc.instances("run");
	if ids.len() <= keep {
		return;
	}
	ids.sort();
	for id in &ids[..ids.len() - keep] {
		doc.remove(&format!("run[{id}]"));
	}
}

fn round2(v: f64) -> f64 {
	(v * 100.0).round() / 100.0
}
fn round6(v: f64) -> f64 {
	(v * 1_000_000.0).round() / 1_000_000.0
}

//••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••
// Environment odds and ends
//••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••••

fn shell(cmd: &str, args: &[&str]) -> String {
	Command::new(cmd)
		.args(args)
		.output()
		.ok()
		.filter(|o| o.status.success())
		.map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
		.unwrap_or_default()
}

fn run_id() -> String {
	let s = shell("date", &["+%Y%m%d-%H%M%S"]);
	if !s.is_empty() {
		return s;
	}
	let secs = std::time::SystemTime::now()
		.duration_since(std::time::UNIX_EPOCH)
		.map_or(0, |d| d.as_secs());
	format!("epoch-{secs}")
}

fn uname() -> String {
	let s = shell("uname", &["-srm"]);
	if s.is_empty() {
		std::env::consts::OS.to_string()
	} else {
		s
	}
}

fn cpu_model() -> String {
	let s = std::fs::read_to_string("/proc/cpuinfo").unwrap_or_default();
	for line in s.lines() {
		if let Some(rest) = line.split_once(':')
			&& line.starts_with("model name")
		{
			return rest.1.trim().to_string();
		}
	}
	"unknown".to_string()
}

fn mktemp() -> Result<String, String> {
	let d = shell("mktemp", &["-d", "/tmp/shcl-comparison.XXXXXX"]);
	if d.is_empty() {
		Err("mktemp failed".into())
	} else {
		Ok(d)
	}
}

/// gzip at its default level, which is what anyone shipping a compressed config
/// actually gets. Shelled out rather than pulling a compression crate in for one
/// number.
fn gzip_size(path: &str) -> usize {
	let f = match std::fs::File::open(path) {
		Ok(f) => f,
		Err(_) => return 0,
	};
	let child = Command::new("gzip")
		.arg("-c")
		.stdin(Stdio::from(f))
		.stdout(Stdio::piped())
		.spawn();
	let mut child = match child {
		Ok(c) => c,
		Err(_) => return 0,
	};
	let mut n = 0usize;
	if let Some(mut out) = child.stdout.take() {
		let mut buf = [0u8; 1 << 16];
		while let Ok(got) = out.read(&mut buf) {
			if got == 0 {
				break;
			}
			n += got;
		}
	}
	let _ = child.wait();
	n
}

/// Versions come from this tool's own committed lockfile, so a recorded run
/// always names the libraries it actually measured.
fn lock_versions() -> HashMap<String, String> {
	let mut out = HashMap::new();
	let text = std::fs::read_to_string(format!("{}/Cargo.lock", env!("CARGO_MANIFEST_DIR")))
		.unwrap_or_default();
	if let Ok(t) = text.parse::<toml::Table>()
		&& let Some(toml::Value::Array(pkgs)) = t.get("package")
	{
		for p in pkgs {
			if let (Some(toml::Value::String(n)), Some(toml::Value::String(v))) =
				(p.get("name"), p.get("version"))
			{
				out.insert(n.clone(), v.clone());
			}
		}
	}
	out
}
