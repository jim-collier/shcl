// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

// An allocation failure must not end the process. Every allocator here fails
// once a budget runs out; a parse or a validate that cannot finish comes back
// NULL, with nothing leaked and SHCL_OOM never reached. Its own translation
// unit because the allocator swap is global to the file - the same reason
// oom_hook.c has one.

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long budget = 1L << 30;
static void *failing_malloc(size_t n) { if (--budget < 0) return NULL; return malloc(n); }
static void *failing_calloc(size_t a, size_t b) { if (--budget < 0) return NULL; return calloc(a, b); }
static void *failing_realloc(void *p, size_t n) { if (--budget < 0) return NULL; return realloc(p, n); }

// The default exits; this one says which path got there first and then does the
// same, because a hook that returns is documented as broken.
#define SHCL_OOM() do { fprintf(stderr, "FAIL oom_recover: SHCL_OOM reached\n"); exit(1); } while (0)

#define malloc failing_malloc
#define calloc failing_calloc
#define realloc failing_realloc
#define SHCL_IMPLEMENTATION
#include "shcl.h"
#undef malloc
#undef calloc
#undef realloc

static int failures = 0;
static void fail(const char *what) { fprintf(stderr, "FAIL oom_recover: %s\n", what); failures++; }

// A document big enough that the parse takes many allocations, so the budgets
// below land in the middle of one rather than only at its edges.
static char *sample(size_t *len) {
	size_t cap = 64 * 1024;
	char *t = (char *)malloc(cap);
	size_t n = 0;
	n += (size_t)snprintf(t + n, cap - n, "group:\n");
	for (int i = 0; i < 400; i++) n += (size_t)snprintf(t + n, cap - n, "\tkey%d: value number %d\n", i, i);
	*len = n;
	return t;
}

int main(void) {
	size_t len; char *text = sample(&len);

	// Every budget short of the whole parse must come back NULL, and the ones
	// past it must read right. Nothing in between may crash or exit.
	int sawNull = 0, sawDoc = 0;
	for (long b = 0; b < 400; b++) {
		budget = b;
		shcl_doc *d = shcl_parse(text, len);
		budget = 1L << 30;
		if (!d) { sawNull = 1; continue; }
		sawDoc = 1;
		if (shcl_get_int_or(d, "group.key399", -1, -1) != -1) fail("a parse that finished read wrong");
		shcl_free(d);
	}
	if (!sawNull) fail("no budget was tight enough to fail a parse");
	if (!sawDoc) fail("no budget was loose enough to finish a parse");

	// A load is the same call plus a read, so it reports the same way.
	{
		// A mingw binary's fopen does not translate /tmp, and windows hosts do
		// not all have a C:\tmp for it to land in.
		const char *dir = getenv("TMPDIR");
		if (!dir) dir = getenv("TMP");
		if (!dir) dir = getenv("TEMP");
		if (!dir) dir = "/tmp";
		char path[512];
		snprintf(path, sizeof path, "%s/shcl-oom-recover.shcl", dir);
		FILE *f = fopen(path, "wb");
		if (!f) fail("could not write the fixture");
		else {
			fwrite(text, 1, len, f); fclose(f);
			int sawNullL = 0;
			for (long b = 0; b < 400; b++) {
				budget = b;
				shcl_file_status st = SHCL_FILE_CLEAN;
				shcl_doc *d = shcl_load_file(path, &st);
				budget = 1L << 30;
				if (!d) sawNullL = 1; else shcl_free(d);
			}
			if (!sawNullL) fail("no budget was tight enough to fail a load");
			remove(path);
		}
	}

	// Validation reports the same way its parse does.
	{
		const char *sch = "field: group.key0\n\ttype: string\n\trequired: yes\n";
		shcl_doc *d = shcl_parse(text, len);
		shcl_doc *s = shcl_parse(sch, strlen(sch));
		if (!d || !s) fail("the unbudgeted parses failed");
		else {
			int sawNullV = 0;
			for (long b = 0; b < 8; b++) {
				budget = b;
				shcl_validation *v = shcl_validate(d, s);
				budget = 1L << 30;
				if (!v) sawNullV = 1; else shcl_validation_free(v);
			}
			if (!sawNullV) fail("no budget was tight enough to fail a validate");
		}
		shcl_free(s); shcl_free(d);
	}

	free(text);
	if (failures == 0) printf("oom_recover: ok\n");
	return failures ? 1 : 0;
}
