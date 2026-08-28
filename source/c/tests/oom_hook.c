// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

// SHCL_OOM is the one way an embedder keeps an allocation failure from ending
// its process. Every allocator here fails once a budget runs out, and the hook
// longjmps back to the caller; the check is that control actually comes back,
// budget by budget, until the parse fits. Its own translation unit because the
// allocator swap is global to the file.

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

// 1 when the parse fit in B allocations and read back right, 0 when the hook
// fired, -1 when it fit but read wrong. Its own function so nothing main holds
// is live across the longjmp.
static int parse_under(long b) {
	const char *text = "group:\n\tkey: value\n\tother: 12\n";
	if (setjmp(oom_jmp)) return 0;
	budget = b;
	shcl_doc *d = shcl_parse(text, strlen(text));
	budget = 1L << 30;
	int ok = shcl_get_int_or(d, "group.other", 11, -1) == 12;
	shcl_free(d);
	return ok ? 1 : -1;
}

int main(void) {
	int parsed = 0, failures = 0;
	for (long b = 0; b < 64 && !parsed; b++) {
		parsed = parse_under(b);
		if (parsed < 0) { fprintf(stderr, "FAIL oom_hook: parse under budget %ld read wrong\n", b); failures++; }
	}
	if (!parsed) { fprintf(stderr, "FAIL oom_hook: no budget under 64 allocations parsed a three-line document\n"); failures++; }
	if (oom_hits == 0) { fprintf(stderr, "FAIL oom_hook: the hook never fired\n"); failures++; }
	if (failures == 0) printf("oom_hook: ok (%d hook hits)\n", oom_hits);
	return failures ? 1 : 0;
}
