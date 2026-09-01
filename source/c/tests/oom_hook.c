// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

// A parse and a validate recover on their own (oom_recover.c). Everywhere else
// - a read, a write, a merge on a document already built - SHCL_OOM is still
// the one way an embedder keeps an allocation failure from ending its process.
// Every allocator here fails once a budget runs out, and the hook longjmps back
// to the caller; the check is that control actually comes back. Its own
// translation unit because the allocator swap is global to the file.

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

static long budget = 1L << 30;
static void *failing_malloc(size_t n) { if (--budget < 0) return NULL; return malloc(n); }
static void *failing_calloc(size_t a, size_t b) { if (--budget < 0) return NULL; return calloc(a, b); }
static void *failing_realloc(void *p, size_t n) { if (--budget < 0) return NULL; return realloc(p, n); }

static jmp_buf oom_jmp;
static int oom_hits = 0;
#define SHCL_OOM() do { oom_hits++; longjmp(oom_jmp, 1); } while (0)

#define malloc failing_malloc
#define calloc failing_calloc
#define realloc failing_realloc
#define SHCL_IMPLEMENTATION
#include "shcl.h"
#undef malloc
#undef calloc
#undef realloc

// 1 when the writes fit in B allocations and read back right, 0 when the hook
// fired, -1 when they fit but read wrong. Its own function so nothing main
// holds is live across the longjmp.
static int write_under(shcl_doc *d, long b) {
	if (setjmp(oom_jmp)) return 0;
	budget = b;
	for (int i = 0; i < 40; i++) {
		char path[32]; int n = snprintf(path, sizeof path, "group.added%d", i);
		shcl_set_int(d, path, (size_t)n, i);
	}
	budget = 1L << 30;
	return shcl_get_int_or(d, "group.added39", 13, -1) == 39 ? 1 : -1;
}

int main(void) {
	const char *text = "group:\n\tkey: value\n\tother: 12\n";
	int wrote = 0, failures = 0;
	for (long b = 0; b < 512 && !wrote; b++) {
		// A fresh document each round: a write that the hook cut short leaves
		// one half-edited, which is the documented cost of the hook returning.
		shcl_doc *d = shcl_parse(text, strlen(text));
		if (!d) { fprintf(stderr, "FAIL oom_hook: the unbudgeted parse failed\n"); failures++; break; }
		wrote = write_under(d, b);
		budget = 1L << 30;  // the hook jumps out with it already spent
		if (wrote < 0) { fprintf(stderr, "FAIL oom_hook: writes under budget %ld read wrong\n", b); failures++; }
		shcl_free(d);
	}
	if (!wrote) { fprintf(stderr, "FAIL oom_hook: no budget under 512 allocations completed 40 writes\n"); failures++; }
	if (oom_hits == 0) { fprintf(stderr, "FAIL oom_hook: the hook never fired\n"); failures++; }
	if (failures == 0) printf("oom_hook: ok (%d hook hits)\n", oom_hits);
	return failures ? 1 : 0;
}
