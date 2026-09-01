// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

// Allocation bounds the corpus cannot see. A capped parse must not hold what
// it is about to refuse, and a read-only loop over a long-lived document must
// stay flat once shcl_reads_release has run. Its own translation unit because
// the counting allocator is global to the file.

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t allocated = 0;
static void *counting_malloc(size_t n) { allocated += n; return malloc(n); }
static void *counting_calloc(size_t a, size_t b) { allocated += a * b; return calloc(a, b); }
static void *counting_realloc(void *p, size_t n) { allocated += n; return realloc(p, n); }

#define malloc counting_malloc
#define calloc counting_calloc
#define realloc counting_realloc
#define SHCL_IMPLEMENTATION
#include "shcl.h"
#undef malloc
#undef calloc
#undef realloc

static int failures = 0;
static void fail(const char *what) { fprintf(stderr, "FAIL mem_bounds: %s\n", what); failures++; }

int main(void) {
	// Element cap: 200k elements on one line, refused at a cap of 8. The
	// refused line used to be built in full first, so the cap saved nothing.
	size_t reps = 200000, tlen = 5 + reps * 3 + 7;
	char *text = (char *)malloc(tlen + 1);
	memcpy(text, "arr: ", 5);
	for (size_t i = 0; i < reps; i++) memcpy(text + 5 + i * 3, "1, ", 3);
	memcpy(text + 5 + reps * 3, "\nok: 5\n", 8);
	size_t before = allocated;
	shcl_doc *d = shcl_parse_limited(text, tlen, SHCL_STANDARD, 0, 8);
	size_t capped = allocated - before;
	if (!d || shcl_lost_count(d) != 1 || shcl_get_int_or(d, "ok", 2, 0) != 5) fail("capped parse: wrong result");
	shcl_free(d);
	before = allocated;
	d = shcl_parse_limited(text, tlen, SHCL_STANDARD, 0, 0);
	size_t uncapped = allocated - before;
	shcl_read_i64_arr ra = shcl_read_int_array(d, "arr", 3);
	if (ra.status != SHCL_GOOD || ra.n != reps) fail("uncapped parse: wrong result");
	shcl_free(d);
	printf("mem_bounds: element cap: capped %zu, uncapped %zu, text %zu\n", capped, uncapped, tlen);
	if (capped > tlen * 8) fail("a capped parse held the array it refused");
	free(text);

	// A read-only loop over a long-lived document, with shcl_reads_release
	// between passes, must stay flat: every read call, including the two that
	// take no path and so never pass through the path lookup's scratch reset.
	// shcl_paths grew the document 11 KB per call before the reset.
	ShclSB sb = {0}; ShclArena tmp = {0};
	for (int i = 0; i < 60; i++) { char line[48]; snprintf(line, sizeof line, "group%d:\n\ta: 1\n\tb: x, y\n", i); sb_puts(&tmp, &sb, line); }
	d = shcl_parse(sb.data, sb.len);
	arena_free(&tmp);
	if (!d) { fail("document for the read loop did not parse"); return 1; }
	// One call per loop: a path read anywhere in the same loop resets scratch
	// on the next call's behalf and hides a call that does not reset it.
	for (int which = 0; which < 4; which++) {
		size_t grew = 0;
		for (int pass = 0; pass < 2; pass++) {
			// The first pass warms every arena; growth is judged on the second.
			if (pass == 1) before = allocated;
			for (int i = 0; i < 2000; i++) {
				size_t got = 0;
				if (which == 0) { shcl_str *paths; got = shcl_paths(d, &paths); }
				else if (which == 1) { shcl_str *kids; got = shcl_children(d, "group7", 6, &kids); }
				else if (which == 2) got = shcl_read_string_array(d, "group7.b", 8).n;
				else got = shcl_to_canonical(d).n != 0;
				if (got != (size_t[]){180, 2, 2, 1}[which]) { fail("read loop: wrong result"); break; }
				shcl_reads_release(d);
			}
		}
		grew = allocated - before;
		printf("mem_bounds: read loop %d: %zu bytes over 2000 released passes\n", which, grew);
		if (grew > 4096) fail("a released read loop grew the document");
	}
	shcl_free(d);

	if (failures) return 1;
	printf("mem_bounds: OK\n");
	return 0;
}
