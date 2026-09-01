// Allocation bounds the corpus cannot see: a capped parse must not hold what
// it is about to refuse. Its own test binary because the counting allocator
// is global to the process, and the other test files run in parallel.

use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering};

struct Counting;
static IN_USE: AtomicUsize = AtomicUsize::new(0);
static PEAK: AtomicUsize = AtomicUsize::new(0);

fn note(delta: isize) {
	let now = if delta >= 0 {
		IN_USE.fetch_add(delta as usize, Ordering::Relaxed) + delta as usize
	} else {
		IN_USE.fetch_sub((-delta) as usize, Ordering::Relaxed) - (-delta) as usize
	};
	PEAK.fetch_max(now, Ordering::Relaxed);
}

unsafe impl GlobalAlloc for Counting {
	unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
		note(layout.size() as isize);
		unsafe { System.alloc(layout) }
	}
	unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
		note(-(layout.size() as isize));
		unsafe { System.dealloc(ptr, layout) }
	}
	unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
		note(new_size as isize - layout.size() as isize);
		unsafe { System.realloc(ptr, layout, new_size) }
	}
}

#[global_allocator]
static GLOBAL: Counting = Counting;

/// Peak bytes held above the starting point while f runs: what the call
/// needed at its worst, not what it touched in total.
fn peak_during(f: impl FnOnce()) -> usize {
	// One measurement at a time: the harness runs the tests here on threads.
	static ONE_AT_A_TIME: std::sync::Mutex<()> = std::sync::Mutex::new(());
	let _turn = ONE_AT_A_TIME.lock().unwrap();
	let start = IN_USE.load(Ordering::Relaxed);
	PEAK.store(start, Ordering::Relaxed);
	f();
	PEAK.load(Ordering::Relaxed) - start
}

#[test]
fn element_cap_bounds_the_parse() {
	use shcl::{Document, Strictness};
	let text = format!("arr: {}\nok: 5\n", "1, ".repeat(200_000));
	let capped = peak_during(|| {
		let doc = Document::parse_limited(&text, Strictness::Standard, 0, 8, 0).unwrap();
		assert_eq!(doc.lost_count(), 1);
		assert_eq!(doc.get_int("ok"), Ok(5));
	});
	// The refused line used to be built in full first (48x the text), so the
	// cap saved nothing. What a capped parse holds is the text and its lines.
	assert!(
		capped < text.len() * 8,
		"a capped parse held the array it refused: {capped} bytes for {} of text",
		text.len()
	);
}

#[test]
fn diagnostic_cap_bounds_the_parse() {
	use shcl::{Document, Strictness};
	// The stacked spelling refuses each element line past the cap on its own,
	// and every refusal is a diagnostic: with the element cap alone, 200k
	// refused lines cost more than the elements they refused. The diagnostic
	// cap is what bounds that.
	let text = format!("arr:\n{}", "\t* 1\n".repeat(200_000));
	let capped = peak_during(|| {
		let doc = Document::parse_limited(&text, Strictness::Standard, 0, 8, 100).unwrap();
		assert_eq!(doc.diagnostics().len(), 101);
		assert_eq!(doc.lost_count(), 200_000 - 8);
	});
	assert!(
		capped < text.len() * 8,
		"a diagnostic-capped parse held its unlisted diagnostics: {capped} bytes for {} of text",
		text.len()
	);
}
