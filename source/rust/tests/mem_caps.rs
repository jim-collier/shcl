// Allocation bounds the corpus cannot see: a capped parse must not hold what
// it is about to refuse. Its own test binary because the counting allocator
// is global to the process, and the other test files run in parallel.

use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering};

struct Counting;
static ALLOCATED: AtomicUsize = AtomicUsize::new(0);

unsafe impl GlobalAlloc for Counting {
	unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
		ALLOCATED.fetch_add(layout.size(), Ordering::Relaxed);
		unsafe { System.alloc(layout) }
	}
	unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
		unsafe { System.dealloc(ptr, layout) }
	}
	unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
		ALLOCATED.fetch_add(new_size, Ordering::Relaxed);
		unsafe { System.realloc(ptr, layout, new_size) }
	}
}

#[global_allocator]
static GLOBAL: Counting = Counting;

fn allocated_by(f: impl FnOnce()) -> usize {
	let before = ALLOCATED.load(Ordering::Relaxed);
	f();
	ALLOCATED.load(Ordering::Relaxed) - before
}

#[test]
fn element_cap_bounds_the_parse() {
	use shcl::{Document, Strictness};
	let text = format!("arr: {}\nok: 5\n", "1, ".repeat(200_000));
	let capped = allocated_by(|| {
		let doc = Document::parse_limited(&text, Strictness::Standard, 0, 8).unwrap();
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
