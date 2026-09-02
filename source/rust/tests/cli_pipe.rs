//! A reader that closes stdout early must not make the CLI print anything on
//! stderr or fail loudly: on unix it dies of SIGPIPE like every other tool,
//! and on windows, which has no such signal, it exits quietly. The release
//! build used to abort there ("failed printing to stdout"), so piping `fmt`
//! into `more` or `Select-Object -First` on windows was an abort.

use std::io::{Read, Write};
use std::process::{Command, Stdio};

#[test]
fn early_closed_stdout_is_quiet() {
	let dir = std::env::temp_dir().join(format!("shcl-pipe-{}", std::process::id()));
	std::fs::create_dir_all(&dir).unwrap();
	let file = dir.join("big.shcl");
	{
		let mut f = std::fs::File::create(&file).unwrap();
		for i in 0..40_000 {
			writeln!(f, "k{i}: {i}").unwrap();
		}
	}
	let mut child = Command::new(env!("CARGO_BIN_EXE_shcl"))
		.arg("fmt")
		.arg(&file)
		.stdin(Stdio::null())
		.stdout(Stdio::piped())
		.stderr(Stdio::piped())
		.spawn()
		.unwrap();
	// Take one byte, then close our end while the CLI still has most of the
	// document to write.
	let mut out = child.stdout.take().unwrap();
	let mut one = [0u8; 1];
	out.read_exact(&mut one).unwrap();
	drop(out);
	let mut err = String::new();
	child
		.stderr
		.take()
		.unwrap()
		.read_to_string(&mut err)
		.unwrap();
	let status = child.wait().unwrap();
	let _ = std::fs::remove_dir_all(&dir);
	assert_eq!(err, "", "stderr must stay empty on a broken pipe");
	#[cfg(unix)]
	{
		use std::os::unix::process::ExitStatusExt;
		assert!(
			status.signal() == Some(13) || status.success(),
			"expected SIGPIPE or a clean exit, got {status:?}"
		);
	}
	#[cfg(not(unix))]
	assert!(status.success(), "expected a clean exit, got {status:?}");
}
