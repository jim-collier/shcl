// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier [ID: 2უNაɘ«҂թȹɤξπ๙¿ձϖ]

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
#ifndef _WIN32
#include <unistd.h>
#endif

static long budget = 1L << 30;
// Live block count, so a call the hook cut short can be asked whether it left
// anything behind. Blocks, not bytes: a leak is a block nobody can reach any
// more, and counting them needs no header on every allocation.
static long live_blocks = 0;
static void tracked_free(void *p) { if (p) live_blocks--; free(p); }
static void *failing_malloc(size_t n) { if (--budget < 0) return NULL; void *p = malloc(n); if (p) live_blocks++; return p; }
static void *failing_calloc(size_t a, size_t b) { if (--budget < 0) return NULL; void *p = calloc(a, b); if (p) live_blocks++; return p; }
static void *failing_realloc(void *p, size_t n) { if (--budget < 0) return NULL; void *q = realloc(p, n); if (q && !p) live_blocks++; return q; }

static jmp_buf oom_jmp;
static int oom_hits = 0;
#define SHCL_OOM() do { oom_hits++; longjmp(oom_jmp, 1); } while (0)

#define malloc failing_malloc
#define calloc failing_calloc
#define realloc failing_realloc
#define free tracked_free
#define SHCL_IMPLEMENTATION
#include "shcl.h"
#undef malloc
#undef calloc
#undef realloc
#undef free

// 1 when the writes fit in B allocations and read back right, 0 when the hook
// fired, -1 when they fit but read wrong. Its own function so nothing main
// holds is live across the longjmp.
static int write_under(shcl_doc *d, long b) {
	if (SHCL_SETJMP(oom_jmp)) return 0;
	budget = b;
	for (int i = 0; i < 40; i++) {
		char path[32]; int n = snprintf(path, sizeof path, "group.added%d", i);
		shcl_set_int(d, path, (size_t)n, i);
	}
	budget = 1L << 30;
	return shcl_get_int_or(d, "group.added39", 13, -1) == 39 ? 1 : -1;
}

// The first lookup builds the name index, which spans many allocations on a
// wide document. 1 when the lookup fits in B allocations and answers right,
// 0 when the hook fired, -1 when it fit but answered wrong.
static int lookup_under(shcl_doc *d, long b, size_t want) {
	if (SHCL_SETJMP(oom_jmp)) return 0;
	budget = b;
	shcl_str *kids = NULL;
	size_t got = shcl_children(d, "group", 5, &kids);
	budget = 1L << 30;
	return got == want ? 1 : -1;
}

// 1 when the migration fits in B allocations, 0 when the hook fired. Its own
// function so nothing main holds is live across the longjmp.
static int migrate_under(const char *text, size_t len, long b) {
	if (SHCL_SETJMP(oom_jmp)) return 0;
	budget = b;
	shcl_migration m = shcl_migrate(text, len, 1);
	budget = 1L << 30;
	tracked_free(m.text);
	return 1;
}

// 2.x spellings the current rules read differently, so every line gets rewritten
// and the working arenas fill.
static char *v2_doc(size_t n, size_t *len) {
	size_t cap = n * 48 + 32;
	char *text = (char *)malloc(cap);
	size_t at = 0;
	for (size_t i = 0; i < n; i++) at += (size_t)snprintf(text + at, cap - at, "k%zu: a\\,b, c#%zu\n", i, i);
	*len = at;
	return text;
}

static shcl_doc *wide_doc(size_t n) {
	size_t cap = n * 16 + 16;
	char *text = (char *)malloc(cap);
	size_t at = (size_t)snprintf(text, cap, "group:\n");
	for (size_t i = 0; i < n; i++) at += (size_t)snprintf(text + at, cap - at, "\tk%zu: %zu\n", i, i);
	shcl_doc *d = shcl_parse(text, at);
	free(text);
	return d;
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

	// A lookup the hook cut short must not poison the next one: the same call
	// again, unbudgeted, has to come back with the right answer. The half-built
	// index used to chain a node to itself, and the retry never returned.
	const size_t wide = 20000;
	int found = 0;
	for (long b = 0; b < 512 && !found; b++) {
		shcl_doc *d = wide_doc(wide);
		if (!d) { fprintf(stderr, "FAIL oom_hook: the wide parse failed\n"); failures++; break; }
		found = lookup_under(d, b, wide);
		budget = 1L << 30;
		if (found < 0) { fprintf(stderr, "FAIL oom_hook: lookup under budget %ld answered wrong\n", b); failures++; }
		if (found == 0) {
#ifndef _WIN32
			alarm(20);
#endif
			int again = lookup_under(d, 1L << 30, wide);
#ifndef _WIN32
			alarm(0);
#endif
			if (again != 1) { fprintf(stderr, "FAIL oom_hook: lookup after a cut-short one under budget %ld: %d\n", b, again); failures++; }
		}
		shcl_free(d);
	}
	if (!found) { fprintf(stderr, "FAIL oom_hook: no budget under 512 allocations completed the wide lookup\n"); failures++; }
	// A migration the hook cut short owns two arenas nobody else can reach. They
	// were frame locals, and an unwind skips the frame holding them, so every
	// cut-short call used to keep about two megabytes for good.
	size_t v2len; char *v2 = v2_doc(2000, &v2len);
	int migrated = 0, sawCut = 0;
	for (long b = 0; b < 4096 && !migrated; b++) {
		long before = live_blocks;
		migrated = migrate_under(v2, v2len, b);
		budget = 1L << 30;
		if (!migrated) sawCut = 1;
		if (live_blocks != before) {
			fprintf(stderr, "FAIL oom_hook: a migration under budget %ld left %ld block(s)\n", b, live_blocks - before);
			failures++;
			break;
		}
	}
	free(v2);
	if (!migrated) { fprintf(stderr, "FAIL oom_hook: no budget under 4096 allocations completed the migration\n"); failures++; }
	if (!sawCut) { fprintf(stderr, "FAIL oom_hook: no budget cut the migration short\n"); failures++; }

	if (oom_hits == 0) { fprintf(stderr, "FAIL oom_hook: the hook never fired\n"); failures++; }
	if (failures == 0) printf("oom_hook: ok (%d hook hits)\n", oom_hits);
	return failures ? 1 : 0;
}
