// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

// Conformance-corpus runner for the C binding. Same corpus every shipped binding
// must pass; column meanings live in project/conformance/README.md. Exit nonzero
// on any miss. Corpus root is argv[1] (default project/conformance, run from the
// repo root as cicd does).

#define _POSIX_C_SOURCE 200809L // strdup, opendir/readdir under -std=c11

// A consumer's own type names must survive the header: its internal typedefs
// are Shcl-prefixed, so these deliberately common names prove no collision
// remains (the public shcl_* surface is separate and unchanged).
typedef int Arena, Node, Value, Element, Str, Parser, Diag, Segment, Selector, Slot;

#define SHCL_IMPLEMENTATION
#include "shcl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>   // _mkdir - windows' mkdir takes no mode argument
#else
#include <sys/stat.h>   // the file-mode fixture stats and chmods for itself
#endif

// Temp root for the file-tier fixture. TMPDIR is the POSIX spelling; windows
// sets TEMP/TMP and has no /tmp to fall back on.
static const char *tmp_root(void) {
	const char *v;
	if ((v = getenv("TMPDIR")) && *v) return v;
	if ((v = getenv("TEMP")) && *v) return v;
	if ((v = getenv("TMP")) && *v) return v;
	return "/tmp";
}

static int nfail = 0;
static void fail(const char *at, const char *msg) { fprintf(stderr, "FAIL %s: %s\n", at, msg); nfail++; }

// Substring search over a length-delimited buffer (memmem is GNU-only).
static int contains(const char *p, size_t n, const char *needle) {
	size_t m = strlen(needle);
	if (m > n) return 0;
	for (size_t i = 0; i + m <= n; i++) if (memcmp(p + i, needle, m) == 0) return 1;
	return 0;
}

// realloc that never returns NULL: on OOM, free the old block and exit 2.
static void *xrealloc(void *p, size_t n) {
	void *q = realloc(p, n);
	if (!q) { free(p); fprintf(stderr, "out of memory\n"); exit(2); }
	return q;
}

static char *read_file(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) { *len = 0; return NULL; }
	char *buf = NULL; size_t cap = 0, n = 0, r; char chunk[65536];
	while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) { if (n + r > cap) { cap = (n + r) * 2; buf = xrealloc(buf, cap); } memcpy(buf + n, chunk, r); n += r; }
	fclose(f);
	if (!buf) buf = xrealloc(NULL, 1);
	*len = n; return buf;
}

// Bytes the document arena holds, for the retention fixture.
static size_t arena_bytes(const ShclArena *a) {
	size_t n = 0;
	for (const ShclBlock *b = a->head; b; b = b->next) n += b->used;
	return n;
}

// Append s to *out with \n and \t escaped, as the corpus writes raw newlines.
static void tsv_escape(const char *s, size_t n, char **out, size_t *olen, size_t *ocap) {
	for (size_t i = 0; i < n; i++) {
		const char *rep = NULL;
		if (s[i] == '\n') rep = "\\n"; else if (s[i] == '\t') rep = "\\t";
		size_t add = rep ? 2 : 1;
		if (*olen + add + 1 > *ocap) { *ocap = (*olen + add + 1) * 2; *out = xrealloc(*out, *ocap); }
		if (rep) { (*out)[(*olen)++] = rep[0]; (*out)[(*olen)++] = rep[1]; }
		else (*out)[(*olen)++] = s[i];
	}
	(*out)[*olen] = '\0';
}

static shcl_strictness parse_level(const char *s) {
	if (!s || !*s || !strcmp(s, "standard")) return SHCL_STANDARD;
	if (!strcmp(s, "loose")) return SHCL_LOOSE;
	if (!strcmp(s, "strict")) return SHCL_STRICT;
	fprintf(stderr, "unknown level '%s' in reads.tsv\n", s); exit(2);
}

// Renders a scalar/array read into a malloc'd string (caller frees); sets *st,
// and for array kinds the per-slot statuses (arena memory, freed with the doc).
static char *scalar_read(shcl_doc *d, const char *kind, const char *q, size_t qn, shcl_status *st, const shcl_status **slots, size_t *nslots) {
	*slots = NULL; *nslots = 0;
	char *out = xrealloc(NULL, 8); memset(out, 0, 8); size_t olen = 0, ocap = 8; char nb[SHCL_F64_BUF];
	#define AS_STR(P, N) do { if ((N) + 1 > ocap) { ocap = (N) + 1; out = xrealloc(out, ocap); } memcpy(out, (P), (N)); out[(N)] = '\0'; olen = (N); } while (0)
	if (!strcmp(kind, "int")) { shcl_read_i64 r = shcl_read_int(d, q, qn); *st = r.status; int k = snprintf(nb, sizeof nb, "%" PRId64, r.value); AS_STR(nb, (size_t)k); }
	else if (!strcmp(kind, "float")) { shcl_read_f64 r = shcl_read_float(d, q, qn); *st = r.status; size_t k = shcl_format_f64(r.value, nb); AS_STR(nb, k); }
	else if (!strcmp(kind, "bool")) { shcl_read_bool r = shcl_read_bool_(d, q, qn); *st = r.status; AS_STR(r.value ? "true" : "false", r.value ? 4u : 5u); }
	else if (!strcmp(kind, "datetime")) { shcl_read_dt r = shcl_read_datetime(d, q, qn); *st = r.status; size_t k = shcl_datetime_str(&r.value, nb); AS_STR(nb, k); }
	else if (!strcmp(kind, "string")) { shcl_read_str r = shcl_read_string(d, q, qn); *st = r.status; tsv_escape(r.value.p, r.value.n, &out, &olen, &ocap); }
	else if (!strcmp(kind, "raw")) { shcl_read_str r = shcl_read_raw(d, q, qn); *st = r.status; tsv_escape(r.value.p, r.value.n, &out, &olen, &ocap); }
	else if (!strcmp(kind, "rawinfo")) { shcl_read_str r = shcl_read_raw_info(d, q, qn); *st = r.status; tsv_escape(r.value.p, r.value.n, &out, &olen, &ocap); }
	else if (!strcmp(kind, "int[]")) { shcl_read_i64_arr r = shcl_read_int_array(d, q, qn); *st = r.status; *slots = r.statuses; *nslots = r.n; for (size_t i = 0; i < r.n; i++) { if (i) tsv_escape("|", 1, &out, &olen, &ocap); int k = snprintf(nb, sizeof nb, "%" PRId64, r.values[i]); tsv_escape(nb, (size_t)k, &out, &olen, &ocap); } }
	else if (!strcmp(kind, "float[]")) { shcl_read_f64_arr r = shcl_read_float_array(d, q, qn); *st = r.status; *slots = r.statuses; *nslots = r.n; for (size_t i = 0; i < r.n; i++) { if (i) tsv_escape("|", 1, &out, &olen, &ocap); size_t k = shcl_format_f64(r.values[i], nb); tsv_escape(nb, k, &out, &olen, &ocap); } }
	else if (!strcmp(kind, "bool[]")) { shcl_read_bool_arr r = shcl_read_bool_array(d, q, qn); *st = r.status; *slots = r.statuses; *nslots = r.n; for (size_t i = 0; i < r.n; i++) { if (i) tsv_escape("|", 1, &out, &olen, &ocap); const char *b = r.values[i] ? "true" : "false"; tsv_escape(b, strlen(b), &out, &olen, &ocap); } }
	else if (!strcmp(kind, "datetime[]")) { shcl_read_dt_arr r = shcl_read_datetime_array(d, q, qn); *st = r.status; *slots = r.statuses; *nslots = r.n; for (size_t i = 0; i < r.n; i++) { if (i) tsv_escape("|", 1, &out, &olen, &ocap); size_t k = shcl_datetime_str(&r.values[i], nb); tsv_escape(nb, k, &out, &olen, &ocap); } }
	else if (!strcmp(kind, "string[]")) { shcl_read_str_arr r = shcl_read_string_array(d, q, qn); *st = r.status; *slots = r.statuses; *nslots = r.n; for (size_t i = 0; i < r.n; i++) { if (i) tsv_escape("|", 1, &out, &olen, &ocap); tsv_escape(r.values[i].p, r.values[i].n, &out, &olen, &ocap); } }
	else { fprintf(stderr, "unknown type '%s'\n", kind); exit(2); }
	#undef AS_STR
	return out;
}

// ops-value unescape (\n \t \\); out >= inlen. Returns length.
static size_t cf_unescape(const char *in, size_t inlen, char *out) {
	size_t w = 0;
	for (size_t i = 0; i < inlen; i++) {
		if (in[i] != '\\' || i + 1 >= inlen) { out[w++] = in[i]; continue; }
		char c = in[++i];
		if (c == 'n') out[w++] = '\n';
		else if (c == 't') out[w++] = '\t';
		else if (c == '\\') out[w++] = '\\';
		else { out[w++] = '\\'; out[w++] = c; }
	}
	return w;
}

// Reference-equivalent op-value gates (same grammar the CLI applies): sign +
// ASCII digits + i64 range for ints; the Rust f64 FromStr grammar for floats
// (overflow yields +-inf, not an error).
static int cf_i64(const char *p, size_t n, int64_t *out) {
	size_t i = 0;
	if (i < n && (p[i] == '+' || p[i] == '-')) i++;
	if (i == n) return 0;
	for (size_t k = i; k < n; k++) if (p[k] < '0' || p[k] > '9') return 0;
	char *b = (char *)xrealloc(NULL, n + 1); memcpy(b, p, n); b[n] = 0;
	errno = 0; char *end; long long v = strtoll(b, &end, 10);
	int ok = *end == 0 && errno != ERANGE;
	free(b);
	if (!ok) return 0;
	*out = (int64_t)v; return 1;
}
static int cf_ci_eq(const char *p, size_t n, const char *kw) {
	size_t kn = strlen(kw);
	if (n != kn) return 0;
	for (size_t i = 0; i < n; i++) { char c = p[i]; if (c >= 'A' && c <= 'Z') c += 32; if (c != kw[i]) return 0; }
	return 1;
}
static int cf_f64(const char *p, size_t n, double *out) {
	size_t i = 0;
	if (i < n && (p[i] == '+' || p[i] == '-')) i++;
	if (!(cf_ci_eq(p + i, n - i, "inf") || cf_ci_eq(p + i, n - i, "infinity") || cf_ci_eq(p + i, n - i, "nan"))) {
		size_t d1 = 0, d2 = 0;
		while (i < n && p[i] >= '0' && p[i] <= '9') { i++; d1++; }
		if (i < n && p[i] == '.') { i++; while (i < n && p[i] >= '0' && p[i] <= '9') { i++; d2++; } }
		if (d1 + d2 == 0) return 0;
		if (i < n && (p[i] == 'e' || p[i] == 'E')) {
			i++;
			if (i < n && (p[i] == '+' || p[i] == '-')) i++;
			size_t d3 = 0;
			while (i < n && p[i] >= '0' && p[i] <= '9') { i++; d3++; }
			if (d3 == 0) return 0;
		}
		if (i != n) return 0;
	}
	char *b = (char *)xrealloc(NULL, n + 1); memcpy(b, p, n); b[n] = 0;
	*out = strtod(b, NULL);
	free(b);
	return 1;
}
// Exactly `true` or `false`; anything else is rejected, like a bad int.
static int cf_bool(const char *p, int *out) {
	if (!strcmp(p, "true")) { *out = 1; return 1; }
	if (!strcmp(p, "false")) { *out = 0; return 1; }
	return 0;
}

// Apply one write-ops line (NUL-terminated, tab-split in place) via the library
// Writer, with the CLI's value gates. A "-default" suffix means "only if
// absent"; values gate first, like the reference. Returns 0 ok, 1 rejected.
static int try_apply_op_c(shcl_doc *d, char *line) {
	size_t cap = 8, nf = 0; char **f = (char **)xrealloc(NULL, cap * sizeof *f);
	f[nf++] = line;
	for (char *p = line; *p; p++) if (*p == '\t') { *p = '\0'; if (nf == cap) { cap *= 2; f = (char **)xrealloc(f, cap * sizeof *f); } f[nf++] = p + 1; }
	char *op = f[0];
	const char *path = nf > 1 ? f[1] : ""; size_t plen = nf > 1 ? strlen(f[1]) : 0;
	const char *v = nf > 2 ? f[2] : ""; size_t vn = nf > 2 ? strlen(f[2]) : 0;
	size_t an = nf > 2 ? nf - 2 : 0;
	size_t oplen = strlen(op);
	int only_absent = 0, rc = 0, wrote = 1;
	if (oplen >= 8 && !strcmp(op + oplen - 8, "-default")) {
		only_absent = 1;
		op[oplen - 8] = '\0';
	}
	#define PRESENT (only_absent && shcl_exists(d, path, plen))
	if (!strcmp(op, "int")) { int64_t x; if (!cf_i64(v, vn, &x)) rc = 1; else if (!PRESENT) wrote = shcl_set_int(d, path, plen, x); }
	else if (!strcmp(op, "float")) { double x; if (!cf_f64(v, vn, &x)) rc = 1; else if (!PRESENT) wrote = shcl_set_float(d, path, plen, x); }
	else if (!strcmp(op, "bool")) { int x; if (!cf_bool(v, &x)) rc = 1; else if (!PRESENT) wrote = shcl_set_bool(d, path, plen, x); }
	else if (!strcmp(op, "literal")) { if (!PRESENT) wrote = shcl_set_literal(d, path, plen, v, vn); }
	else if (!strcmp(op, "string")) { if (!PRESENT) { char *b = (char *)xrealloc(NULL, vn ? vn : 1); size_t m = cf_unescape(v, vn, b); wrote = shcl_set_string(d, path, plen, b, m); free(b); } }
	else if (!strcmp(op, "datetime")) { shcl_datetime dt; ShclStr sv; sv.p = v; sv.n = vn; if (!parse_datetime(&d->arena, sv, &dt)) rc = 1; else if (!PRESENT) wrote = shcl_set_datetime(d, path, plen, &dt); }
	else if (!strcmp(op, "int-array")) {
		int64_t *a = (int64_t *)xrealloc(NULL, (an ? an : 1) * sizeof *a);
		for (size_t i = 0; i < an && !rc; i++) if (!cf_i64(f[2 + i], strlen(f[2 + i]), &a[i])) rc = 1;
		if (!rc && !PRESENT) wrote = shcl_set_int_array(d, path, plen, a, an);
		free(a);
	}
	else if (!strcmp(op, "float-array")) {
		double *a = (double *)xrealloc(NULL, (an ? an : 1) * sizeof *a);
		for (size_t i = 0; i < an && !rc; i++) if (!cf_f64(f[2 + i], strlen(f[2 + i]), &a[i])) rc = 1;
		if (!rc && !PRESENT) wrote = shcl_set_float_array(d, path, plen, a, an);
		free(a);
	}
	else if (!strcmp(op, "bool-array")) {
		int *a = (int *)xrealloc(NULL, (an ? an : 1) * sizeof *a);
		for (size_t i = 0; i < an && !rc; i++) if (!cf_bool(f[2 + i], &a[i])) rc = 1;
		if (!rc && !PRESENT) wrote = shcl_set_bool_array(d, path, plen, a, an);
		free(a);
	}
	else if (!strcmp(op, "string-array")) {
		if (!PRESENT) {
			char **sv = (char **)xrealloc(NULL, (an ? an : 1) * sizeof *sv); size_t *sl = (size_t *)xrealloc(NULL, (an ? an : 1) * sizeof *sl);
			sv[0] = NULL; sl[0] = 0; // silence -Wmaybe-uninitialized for the an==0 call
			for (size_t i = 0; i < an; i++) { size_t L = strlen(f[2 + i]); char *b = (char *)xrealloc(NULL, L ? L : 1); sl[i] = cf_unescape(f[2 + i], L, b); sv[i] = b; }
			wrote = shcl_set_string_array(d, path, plen, (const char *const *)sv, sl, an);
			for (size_t i = 0; i < an; i++) free(sv[i]);
			free(sv); free(sl);
		}
	}
	else if (!strcmp(op, "datetime-array")) {
		shcl_datetime *a = (shcl_datetime *)xrealloc(NULL, (an ? an : 1) * sizeof *a);
		for (size_t i = 0; i < an && !rc; i++) { ShclStr sv; sv.p = f[2 + i]; sv.n = strlen(f[2 + i]); if (!parse_datetime(&d->arena, sv, &a[i])) rc = 1; }
		if (!rc && !PRESENT) wrote = shcl_set_datetime_array(d, path, plen, a, an);
		free(a);
	}
	else if (!strcmp(op, "raw")) { if (!PRESENT) { const char *cont = nf > 3 ? f[3] : ""; size_t cn = nf > 3 ? strlen(f[3]) : 0; char *b = (char *)xrealloc(NULL, cn ? cn : 1); size_t m = cf_unescape(cont, cn, b); wrote = shcl_set_raw(d, path, plen, b, m, v, vn); free(b); } }
	else if (!strcmp(op, "empty") && !only_absent) wrote = shcl_set_empty(d, path, plen);
	else if (!strcmp(op, "comment") && !only_absent) wrote = shcl_set_comment(d, path, plen, v, vn);
	else if (!strcmp(op, "remove") && !only_absent) shcl_remove(d, path, plen);
	else rc = 1; // unknown op
	if (rc == 0 && !wrote) rc = 1;
	#undef PRESENT
	free(f);
	return rc;
}

// Good-path wrapper: the op must apply.
static void apply_op_c(shcl_doc *d, char *line) {
	if (try_apply_op_c(d, line)) { fprintf(stderr, "write op rejected: %s\n", line); nfail++; }
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

// Splits raw TSV/text buffer into an array of NUL-terminated lines (in place).
static size_t split_lines(char *buf, size_t n, char ***out) {
	size_t cap = 16, cnt = 0; char **lines = xrealloc(NULL, cap * sizeof *lines);
	size_t start = 0;
	for (size_t i = 0; i <= n; i++) if (i == n || buf[i] == '\n') {
		if (cnt == cap) { cap *= 2; lines = xrealloc(lines, cap * sizeof *lines); }
		buf[i] = '\0'; lines[cnt++] = buf + start; start = i + 1;
	}
	*out = lines; return cnt;
}

int main(int argc, char **argv) {
	setlocale(LC_ALL, "C");
	const char *corpus = argc > 1 ? argv[1] : "project/conformance";

	DIR *dir = opendir(corpus);
	if (!dir) { fprintf(stderr, "no corpus dir: %s\n", corpus); return 2; }
	char **names = NULL; size_t nn = 0, cn = 0; const struct dirent *de;
	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.') continue;
		char path[4096]; snprintf(path, sizeof path, "%s/%s/input.shcl", corpus, de->d_name);
		FILE *t = fopen(path, "rb"); if (!t) continue; fclose(t);
		if (nn == cn) { cn = cn ? cn * 2 : 8; names = xrealloc(names, cn * sizeof *names); }
		names[nn++] = strdup(de->d_name);
	}
	closedir(dir);
	if (nn == 0) { fprintf(stderr, "no corpus cases under %s\n", corpus); return 2; }
	qsort(names, nn, sizeof *names, cmp_str);

	for (size_t ci = 0; ci < nn; ci++) {
		char path[4096];
		snprintf(path, sizeof path, "%s/%s/input.shcl", corpus, names[ci]); size_t ilen; char *input = read_file(path, &ilen);
		snprintf(path, sizeof path, "%s/%s/expected.shcl", corpus, names[ci]); size_t elen; char *expected = read_file(path, &elen);
		snprintf(path, sizeof path, "%s/%s/reads.tsv", corpus, names[ci]); size_t rlen; char *reads = read_file(path, &rlen);

		// Canonical output must match expected.shcl and be a fixpoint.
		shcl_doc *d = shcl_parse(input, ilen);
		shcl_str got = shcl_to_canonical(d);
		if (got.n != elen || (elen && memcmp(got.p, expected, elen) != 0)) fail(names[ci], "canonical output differs from expected.shcl");
		shcl_doc *d2 = shcl_parse(got.p, got.n);
		shcl_str again = shcl_to_canonical(d2);
		if (again.n != got.n || (got.n && memcmp(again.p, got.p, got.n) != 0)) fail(names[ci], "formatter is not idempotent");
		shcl_free(d2);

		// Diagnostics: count, line, severity, and stable code must match the golden
		// (the same shape `check` prints to stdout at Standard).
		snprintf(path, sizeof path, "%s/%s/expected-diags.txt", corpus, names[ci]); size_t dlen; char *ediags = read_file(path, &dlen);
		if (!ediags) fail(names[ci], "missing expected-diags.txt");
		else {
			size_t ndiag = shcl_diag_count(d), nerr = 0;
			char *dj = xrealloc(NULL, 64); size_t jl = 0, jc = 64;
			for (size_t i = 0; i < ndiag; i++) {
				if (shcl_diag_severity(d, i) == SHCL_SEV_ERROR) nerr++;
				char ln[128]; int w = snprintf(ln, sizeof ln, "line %zu: %s: %s\n", shcl_diag_line(d, i), shcl_diag_severity(d, i) == SHCL_SEV_ERROR ? "Error" : "Hint", shcl_diag_code(d, i));
				if (jl + (size_t)w + 1 > jc) { jc = (jl + (size_t)w + 1) * 2; dj = xrealloc(dj, jc); }
				memcpy(dj + jl, ln, (size_t)w); jl += (size_t)w;
			}
			char sum[96]; int sw;
			if (nerr) sw = snprintf(sum, sizeof sum, "failed: %zu diagnostic(s), %zu error(s)\n", ndiag, nerr);
			else sw = snprintf(sum, sizeof sum, "ok (%zu diagnostic(s))\n", ndiag);
			if (jl + (size_t)sw + 1 > jc) { jc = jl + (size_t)sw + 1; dj = xrealloc(dj, jc); }
			memcpy(dj + jl, sum, (size_t)sw); jl += (size_t)sw;
			if (jl != dlen || (dlen && memcmp(dj, ediags, dlen) != 0)) fail(names[ci], "diagnostics differ from expected-diags.txt");
			free(dj); free(ediags);
		}
		shcl_free(d);

		if (reads) {
			char **lines; size_t nl = split_lines(reads, rlen, &lines);
			for (size_t li = 0; li < nl; li++) {
				if (li == 0 || lines[li][0] == '\0') continue;
				// split by tab
				char *cols[8]; int nc = 0; char *p = lines[li];
				cols[nc++] = p;
				for (; *p && nc < 8; p++) if (*p == '\t') { *p = '\0'; cols[nc++] = p + 1; }
				if (nc < 4) { fail(names[ci], "reads.tsv line too short"); continue; }
				const char *query = cols[0], *kind = cols[1], *exp = cols[2], *status = cols[3];
				shcl_strictness level = parse_level(nc > 4 ? cols[4] : NULL);
				size_t qn = strlen(query);
				char at[512]; snprintf(at, sizeof at, "%s (%s %s)", names[ci], query, kind);

				if (!strcmp(kind, "load")) {
					shcl_doc *ld = shcl_parse_with(input, ilen, level);
					int ok = !shcl_strict_failed(ld); shcl_free(ld);
					int want = !strcmp(exp, "ok") ? 1 : (!strcmp(exp, "fail") ? 0 : -1);
					if (want < 0) fail(at, "bad load expectation");
					else if (ok != want) fail(at, "load outcome mismatch");
					continue;
				}
				shcl_doc *rd = shcl_parse_with(input, ilen, level);
				if (shcl_strict_failed(rd)) { fail(at, "load failed but reads.tsv has reads there"); shcl_free(rd); continue; }
				if (!strcmp(kind, "count")) {
					char nb[32]; snprintf(nb, sizeof nb, "%zu", shcl_count(rd, query, qn));
					if (strcmp(nb, exp)) fail(at, "count mismatch");
					shcl_free(rd); continue;
				}
				if (!strcmp(kind, "instances")) {
					shcl_str *vals; size_t n = shcl_instances(rd, query, qn, &vals);
					char *joined = xrealloc(NULL, 8); joined[0] = '\0'; size_t jl = 0, jc = 8;
					for (size_t i = 0; i < n; i++) { if (i) { if (jl + 2 > jc) { jc = jl + 2; joined = xrealloc(joined, jc); } joined[jl++] = '|'; joined[jl] = '\0'; } if (jl + vals[i].n + 1 > jc) { jc = jl + vals[i].n + 1; joined = xrealloc(joined, jc); } memcpy(joined + jl, vals[i].p, vals[i].n); jl += vals[i].n; joined[jl] = '\0'; }
					if (strcmp(joined, exp)) fail(at, "instances mismatch");
					free(joined); shcl_free(rd); continue;
				}
				shcl_status st; const shcl_status *slots; size_t nslots;
				char *val = scalar_read(rd, kind, query, qn, &st, &slots, &nslots);
				if (strcmp(shcl_status_name(st), status)) fail(at, "status mismatch");
				if (strcmp(exp, "-") && strcmp(val, exp)) fail(at, "value mismatch");
				// Optional 6th column: per-slot statuses, |-joined (needs col 5 set).
				if (nc > 5) {
					char sj[1024]; size_t sl = 0; sj[0] = '\0';
					for (size_t i = 0; i < nslots && sl + 16 < sizeof sj; i++)
						sl += (size_t)snprintf(sj + sl, sizeof sj - sl, "%s%s", i ? "|" : "", shcl_status_name(slots[i]));
					if (strcmp(sj, cols[5])) fail(at, "slots mismatch");
				}
				free(val); shcl_free(rd);
			}
			free(lines);
		}

		// Write dimension (optional): apply write.ops and match expected-write.shcl.
		snprintf(path, sizeof path, "%s/%s/write.ops", corpus, names[ci]); size_t olen; char *ops = read_file(path, &olen);
		if (ops) {
			snprintf(path, sizeof path, "%s/%s/expected-write.shcl", corpus, names[ci]); size_t wlen; char *ew = read_file(path, &wlen);
			shcl_doc *wd = shcl_parse(input, ilen);
			char **olines; size_t nol = split_lines(ops, olen, &olines);
			for (size_t li = 0; li < nol; li++) {
				if (olines[li][0] == '\0' || olines[li][0] == '#') continue;
				apply_op_c(wd, olines[li]);
			}
			shcl_str wgot = shcl_to_canonical(wd);
			if (!ew || wgot.n != wlen || (wlen && memcmp(wgot.p, ew, wlen) != 0)) fail(names[ci], "writer output differs from expected-write.shcl");
			shcl_doc *wd2 = shcl_parse(wgot.p, wgot.n);
			shcl_str wagain = shcl_to_canonical(wd2);
			if (wagain.n != wgot.n || (wgot.n && memcmp(wagain.p, wgot.p, wgot.n) != 0)) fail(names[ci], "written output is not a fmt fixpoint");
			shcl_free(wd2); shcl_free(wd); free(olines); free(ops); free(ew);
		}

		// Bad-op dimension (optional): each write-bad.ops line, applied alone,
		// must be rejected and leave the document unchanged.
		snprintf(path, sizeof path, "%s/%s/write-bad.ops", corpus, names[ci]); size_t blen; char *bops = read_file(path, &blen);
		if (bops) {
			char **blines; size_t nbl = split_lines(bops, blen, &blines);
			for (size_t li = 0; li < nbl; li++) {
				if (blines[li][0] == '\0' || blines[li][0] == '#') continue;
				shcl_doc *bd = shcl_parse(input, ilen);
				shcl_str before = shcl_to_canonical(bd);
				char *before_copy = (char *)xrealloc(NULL, before.n ? before.n : 1);
				memcpy(before_copy, before.p, before.n); size_t before_n = before.n;
				if (!try_apply_op_c(bd, blines[li])) fail(names[ci], "write-bad.ops line was accepted");
				shcl_str after = shcl_to_canonical(bd);
				if (after.n != before_n || (before_n && memcmp(after.p, before_copy, before_n) != 0)) fail(names[ci], "write-bad.ops line changed the document");
				free(before_copy); shcl_free(bd);
			}
			free(blines); free(bops);
		}

		// Schema dimension (optional): golden = the exact `check --schema` stdout
		// at Standard (doc parse diags, then validation diags, then the summary).
		// A schema that does not load cleanly is a single V099, mirroring the CLI.
		snprintf(path, sizeof path, "%s/%s/schema.shcl", corpus, names[ci]); size_t schlen; char *sch = read_file(path, &schlen);
		if (sch) {
			snprintf(path, sizeof path, "%s/%s/expected-validate.txt", corpus, names[ci]); size_t evlen; char *ev = read_file(path, &evlen);
			if (!ev) { fail(names[ci], "schema.shcl without expected-validate.txt"); free(sch); }
			else {
				shcl_doc *vd = shcl_parse(input, ilen);
				shcl_doc *sd = shcl_parse(sch, schlen);
				int v99 = 0;
				for (size_t i = 0; i < shcl_diag_count(sd); i++) if (shcl_diag_severity(sd, i) == SHCL_SEV_ERROR) v99 = 1;
				shcl_validation *vv = v99 ? NULL : shcl_validate(vd, sd);
				if (vv) { shcl_suppress_declared_repeats(sd, vd); shcl_suppress_declared_reopens(sd, vd); }
				size_t nd = shcl_diag_count(vd), nv = vv ? shcl_validation_count(vv) : 0, nerr = 0;
				size_t total = nd + nv + (v99 ? 1 : 0);
				char *vj = xrealloc(NULL, 64); size_t jl = 0, jc = 64;
				for (size_t i = 0; i < nd + nv + (size_t)(v99 ? 1 : 0); i++) {
					char ln[128]; int w;
					if (i < nd) {
						if (shcl_diag_severity(vd, i) == SHCL_SEV_ERROR) nerr++;
						w = snprintf(ln, sizeof ln, "line %zu: %s: %s\n", shcl_diag_line(vd, i), shcl_diag_severity(vd, i) == SHCL_SEV_ERROR ? "Error" : "Hint", shcl_diag_code(vd, i));
					} else if (v99) {
						nerr++;
						w = snprintf(ln, sizeof ln, "line 0: Error: V099\n");
					} else {
						size_t k = i - nd;
						if (shcl_validation_severity(vv, k) == SHCL_SEV_ERROR) nerr++;
						w = snprintf(ln, sizeof ln, "line %zu: %s: %s\n", shcl_validation_line(vv, k), shcl_validation_severity(vv, k) == SHCL_SEV_ERROR ? "Error" : "Hint", shcl_validation_code(vv, k));
					}
					if (jl + (size_t)w + 1 > jc) { jc = (jl + (size_t)w + 1) * 2; vj = xrealloc(vj, jc); }
					memcpy(vj + jl, ln, (size_t)w); jl += (size_t)w;
				}
				char sum[96]; int sw;
				if (nerr) sw = snprintf(sum, sizeof sum, "failed: %zu diagnostic(s), %zu error(s)\n", total, nerr);
				else sw = snprintf(sum, sizeof sum, "ok (%zu diagnostic(s))\n", total);
				if (jl + (size_t)sw + 1 > jc) { jc = jl + (size_t)sw + 1; vj = xrealloc(vj, jc); }
				memcpy(vj + jl, sum, (size_t)sw); jl += (size_t)sw;
				if (jl != evlen || (evlen && memcmp(vj, ev, evlen) != 0)) fail(names[ci], "validation output differs from expected-validate.txt");
				free(vj);
				shcl_validation_free(vv); shcl_free(sd); shcl_free(vd); free(sch); free(ev);
			}
		}

		// Layered-load dimension (optional): fold the layer*.shcl files (lowest
		// first) and input.shcl (highest file layer) via the library shcl_merge,
		// apply the merge.sets path=value overrides, match expected-merged.shcl.
		snprintf(path, sizeof path, "%s/%s/expected-merged.shcl", corpus, names[ci]); size_t emlen; char *em = read_file(path, &emlen);
		if (em) {
			// Collect layer file names in the case dir, sorted (filename = priority).
			char layerNames[64][256]; int nlayer = 0;
			snprintf(path, sizeof path, "%s/%s", corpus, names[ci]);
			DIR *cd = opendir(path);
			if (cd) {
				const struct dirent *le;
				while ((le = readdir(cd))) {
					size_t dn = strlen(le->d_name);
					if (dn > 5 && !strncmp(le->d_name, "layer", 5) && !strcmp(le->d_name + dn - 5, ".shcl") && nlayer < 64)
						snprintf(layerNames[nlayer++], 256, "%s", le->d_name);
				}
				closedir(cd);
			}
			for (int a = 0; a < nlayer; a++) for (int b = a + 1; b < nlayer; b++) if (strcmp(layerNames[a], layerNames[b]) > 0) { char tmp[256]; memcpy(tmp, layerNames[a], 256); memcpy(layerNames[a], layerNames[b], 256); memcpy(layerNames[b], tmp, 256); }
			// Fold: layer0 (base) then each higher layer, then input.shcl.
			char *ltexts[65]; size_t llens[65]; int nt = 0;
			shcl_doc *md = NULL;
			for (int li = 0; li < nlayer; li++) {
				// The precision is the same 256 the store above enforces: without it the
				// compiler bounds layerNames[li] by the whole array, not by one row.
				snprintf(path, sizeof path, "%s/%s/%.255s", corpus, names[ci], layerNames[li]);
				ltexts[nt] = read_file(path, &llens[nt]);
				shcl_doc *dd = shcl_parse(ltexts[nt], llens[nt]); nt++;
				if (!md) md = dd; else { shcl_merge(md, dd); shcl_free(dd); }
			}
			{
				shcl_doc *dd = shcl_parse(input, ilen);
				if (!md) md = dd; else { shcl_merge(md, dd); shcl_free(dd); }
			}
			// merge.sets: one path=value per line, applied as the top layer.
			snprintf(path, sizeof path, "%s/%s/merge.sets", corpus, names[ci]); size_t mslen; char *ms = read_file(path, &mslen);
			if (ms) {
				char **slines; size_t nsl = split_lines(ms, mslen, &slines);
				for (size_t li = 0; li < nsl; li++) {
					if (slines[li][0] == '\0' || slines[li][0] == '#') continue;
					const char *eq = strchr(slines[li], '=');
					if (eq) shcl_set_string(md, slines[li], (size_t)(eq - slines[li]), eq + 1, strlen(eq + 1));
				}
				free(slines); free(ms);
			}
			shcl_str mgot = shcl_to_canonical(md);
			if (mgot.n != emlen || (emlen && memcmp(mgot.p, em, emlen) != 0)) fail(names[ci], "merged output differs from expected-merged.shcl");
			shcl_doc *md2 = shcl_parse(mgot.p, mgot.n);
			shcl_str magain = shcl_to_canonical(md2);
			if (magain.n != mgot.n || (mgot.n && memcmp(magain.p, mgot.p, mgot.n) != 0)) fail(names[ci], "merged output is not a fmt fixpoint");
			shcl_free(md2); shcl_free(md);
			for (int li = 0; li < nt; li++) free(ltexts[li]);
			free(em);
		}

		// Generation dimension (optional): Generate on the schema must reproduce
		// the golden starter config, and that output must itself load cleanly.
		snprintf(path, sizeof path, "%s/%s/init-schema.shcl", corpus, names[ci]); size_t islen; char *isch = read_file(path, &islen);
		if (isch) {
			snprintf(path, sizeof path, "%s/%s/expected-init.shcl", corpus, names[ci]); size_t eilen; char *ei = read_file(path, &eilen);
			if (!ei) { fail(names[ci], "init-schema.shcl without expected-init.shcl"); free(isch); }
			else {
				shcl_doc *isd = shcl_parse(isch, islen);
				int ok = 0;
				shcl_str it = shcl_generate(isd, 0, &ok);
				if (!ok) fail(names[ci], "init schema has faults");
				else {
					if (it.n != eilen || (eilen && memcmp(it.p, ei, eilen) != 0)) fail(names[ci], "init output differs from expected-init.shcl");
					// The footer is the only difference the flag makes:
					// everything before it is byte-for-byte what the default
					// run produced.
					shcl_str bare = shcl_generate(isd, 1, &ok);
					if (!bare.n || bare.n >= it.n || memcmp(it.p, bare.p, bare.n) != 0)
						fail(names[ci], "--no-banner output is not a prefix of the default");
					else if (!contains(it.p + bare.n, it.n - bare.n, "This config file format is SHCL."))
						fail(names[ci], "default init output is missing the format footer");
					shcl_doc *gd = shcl_parse(it.p, it.n);
					int cln = 1;
					for (size_t i = 0; i < shcl_diag_count(gd); i++) if (shcl_diag_severity(gd, i) == SHCL_SEV_ERROR) cln = 0;
					if (!cln) fail(names[ci], "generated starter does not load cleanly");
					// And it must satisfy the very schema that produced it.
					shcl_validation *gv = shcl_validate(gd, isd);
					for (size_t i = 0; i < shcl_validation_count(gv); i++)
						if (shcl_validation_severity(gv, i) == SHCL_SEV_ERROR) { fail(names[ci], "generated starter fails its own schema"); break; }
					shcl_validation_free(gv);
					shcl_free(gd);
				}
				shcl_free(isd); free(isch); free(ei);
			}
		}
		free(input); free(expected); free(reads);
	}
	for (size_t i = 0; i < nn; i++) free(names[i]);
	free(names);
	// paths(): file order, deduplicated, non-bare segments quoted so every
	// path resolves. Same fixture is pinned in every runner.
	{
		const char *pt = "a: 1\na.b: 2\n\"q n\": 3\nx:\n\tb: 4\nx.b: 5\n";
		shcl_doc *pd = shcl_parse(pt, strlen(pt));
		shcl_str *pv; size_t pn = shcl_paths(pd, &pv);
		const char *want[] = { "a", "a.b", "\"q n\"", "x", "x.b" };
		if (pn != 5) fail("paths", "count mismatch");
		else for (size_t i = 0; i < 5; i++) if (pv[i].n != strlen(want[i]) || memcmp(pv[i].p, want[i], pv[i].n) != 0) { fail("paths", "fixture mismatch"); break; }
		for (size_t i = 0; i < pn; i++) if (shcl_count(pd, pv[i].p, pv[i].n) < 1) { fail("paths", "emitted path does not resolve"); break; }
		// quote_segment: same spelling both directions, injection-safe.
		shcl_str qs = shcl_quote_segment(pd, "q n", 3);
		if (qs.n != 5 || memcmp(qs.p, "\"q n\"", 5) != 0) fail("paths", "quote_segment spelling drift");
		shcl_read_i64 qr = shcl_read_int(pd, qs.p, qs.n);
		if (qr.value != 3 || qr.status != SHCL_GOOD) fail("paths", "quoted segment read failed");
		shcl_free(pd);
	}
	// A raw body is the only content kept untrimmed, so it is the only place a
	// trailing CR survives the load - and one written back becomes CRLF, which
	// reads as neither. The whole trailing run comes off instead; a CR inside a
	// line is content and stays. Same fixture in every runner: a golden would be
	// rewritten by any platform's line-ending translation.
	{
		const char *rt = "r:\n\t~~~\n\tone\r\r\n\ta\rb\n\t~~~\n";
		shcl_doc *rd = shcl_parse(rt, strlen(rt));
		shcl_read_str rr = shcl_read_raw(rd, "r", 1);
		if (rr.value.n != 7 || memcmp(rr.value.p, "one\na\rb", 7) != 0) fail("raw", "trailing CR run not normalized");
		shcl_str rc = shcl_to_canonical(rd);
		shcl_doc *rd2 = shcl_parse(rc.p, rc.n);
		shcl_str rc2 = shcl_to_canonical(rd2);
		if (rc.n != rc2.n || memcmp(rc.p, rc2.p, rc.n) != 0) fail("raw", "raw block with CR is not a formatter fixpoint");
		shcl_free(rd2);
		shcl_free(rd);
	}
	// line()/quoted()/children(): read-surface accessors. Same fixture in every
	// runner - the other three carry line and quoted on the read result, C
	// keeps its read structs value+status and answers with these instead.
	{
		const char *lt = "a: @null\nb: \"@null\"\ncode:\n\thook: 1\n\thook: 2\n\tdone: 3\n";
		shcl_doc *ld = shcl_parse(lt, strlen(lt));
		if (shcl_quoted(ld, "a", 1)) fail("quoted", "unquoted read reports quoted");
		if (!shcl_quoted(ld, "b", 1)) fail("quoted", "quoted read not flagged");
		if (shcl_quoted(ld, "code", 4)) fail("quoted", "a block reports quoted");
		if (shcl_quoted(ld, "missing", 7)) fail("quoted", "missing path reports quoted");
		if (shcl_line(ld, "code.done", 9) != 6) fail("line", "code.done not line 6");
		if (shcl_line(ld, "code", 4) != 3) fail("line", "code not line 3");
		if (shcl_line(ld, "missing", 7) != 0) fail("line", "missing path not 0");
		// lines(): the plural - a repeated field cites every binding, wildcard
		// slots keep their index (0 = unresolved), a miss is the empty list.
		if (shcl_line(ld, "code.hook", 9) != 0) fail("line", "code.hook not 0"); // Multiple - the singular's gap
		size_t *lv; size_t lnc = shcl_lines(ld, "code.hook", 9, &lv);
		if (lnc != 2 || lv[0] != 4 || lv[1] != 5) fail("lines", "code.hook not [4, 5]");
		lnc = shcl_lines(ld, "code.done", 9, &lv);
		if (lnc != 1 || lv[0] != 6) fail("lines", "code.done not [6]");
		lnc = shcl_lines(ld, "a", 1, &lv);
		if (lnc != 1 || lv[0] != 1) fail("lines", "a not [1]");
		lnc = shcl_lines(ld, "code[*].done", 12, &lv);
		if (lnc != 1 || lv[0] != 6) fail("lines", "code[*].done not [6]");
		lnc = shcl_lines(ld, "code[*].nope", 12, &lv);
		if (lnc != 1 || lv[0] != 0) fail("lines", "code[*].nope not [0]");
		if (shcl_lines(ld, "missing", 7, &lv) != 0) fail("lines", "missing path not empty");
		shcl_str *cv; size_t chn = shcl_children(ld, "code", 4, &cv);
		const char *cw[] = { "hook", "hook", "done" };
		if (chn != 3) fail("children", "code count mismatch");
		else for (size_t i = 0; i < 3; i++) if (cv[i].n != strlen(cw[i]) || memcmp(cv[i].p, cw[i], cv[i].n) != 0) { fail("children", "code names mismatch"); break; }
		chn = shcl_children(ld, "", 0, &cv);
		const char *rw[] = { "a", "b", "code" };
		if (chn != 3) fail("children", "root count mismatch");
		else for (size_t i = 0; i < 3; i++) if (cv[i].n != strlen(rw[i]) || memcmp(cv[i].p, rw[i], cv[i].n) != 0) { fail("children", "root names mismatch"); break; }
		if (shcl_children(ld, "missing", 7, &cv) != 0) fail("children", "missing path not empty");
		shcl_free(ld);
	}
	// authored_name: the author's spelling, unfolded; merged instances keep the
	// first binding's; unresolved or Multiple is empty; writer-built keeps the
	// setter path's spelling. Same fixture in every runner.
	{
		const char *nt = "SYMBOLS: 3\nCode:\n\tx: 1\ncode:\n\ty: 2\n";
		shcl_doc *nd = shcl_parse(nt, strlen(nt));
		shcl_str sn = shcl_authored_name(nd, "symbols", 7);
		if (sn.n != 7 || memcmp(sn.p, "SYMBOLS", 7) != 0) fail("authored_name", "symbols spelling mismatch");
		sn = shcl_authored_name(nd, "code", 4);
		if (sn.n != 4 || memcmp(sn.p, "Code", 4) != 0) fail("authored_name", "code spelling mismatch");
		if (shcl_authored_name(nd, "missing", 7).n != 0) fail("authored_name", "missing path not empty");
		if (!shcl_set_int(nd, "NewTop.n", 8, 1)) fail("authored_name", "set_int NewTop.n failed");
		sn = shcl_authored_name(nd, "newtop", 6);
		if (sn.n != 6 || memcmp(sn.p, "NewTop", 6) != 0) fail("authored_name", "newtop spelling mismatch");
		shcl_free(nd);
		// Escapes ARE resolved on a name, so both spellings of the path find the
		// same node - while shcl_authored_name still hands back the source
		// spelling, which is the one thing it is for. Same fixture everywhere.
		const char *et = "\"Ab\\tCd\": 2\n";
		shcl_doc *ed = shcl_parse(et, strlen(et));
		sn = shcl_authored_name(ed, "\"ab\\tcd\"", 8);
		if (sn.n != 6 || memcmp(sn.p, "Ab\\tCd", 6) != 0) fail("authored_name", "escaped spelling mismatch");
		sn = shcl_authored_name(ed, "\"ab\tcd\"", 7); // a real tab: one byte shorter than the escaped spelling
		if (sn.n != 6 || memcmp(sn.p, "Ab\\tCd", 6) != 0) fail("authored_name", "literal spelling mismatch");
		if (shcl_read_int(ed, "\"ab\tcd\"", 7).value != 2) fail("authored_name", "read via the literal spelling failed");
		{
			// Canonical output folds the case, as it always has, and escapes the tab.
			const char *ec = "\"ab\\tcd\": 2\n";
			shcl_str ecn = shcl_to_canonical(ed);
			if (ecn.n != strlen(ec) || memcmp(ecn.p, ec, ecn.n) != 0) fail("authored_name", "canonical name spelling moved");
		}
		shcl_free(ed);
	}
	// The get-tier value survives only on Good; Empty/BadType/NotFound all fall
	// back to the call-site default, so a real zero can't be faked. `_or` is the
	// cross-binding spelling for it, so a routine ported between two bindings
	// cannot keep the call name while changing which tier it lands on. Same
	// fixture in every runner (C's convenience tier is the value types only).
	{
		const char *ct = "a: 42\nb: not-a-number\ne:\n";
		shcl_doc *cd = shcl_parse(ct, strlen(ct));
		if (shcl_get_int_or(cd, "a", 1, 9) != 42) fail("get_or", "Good did not read through");
		if (shcl_get_int_or(cd, "b", 1, 9) != 9) fail("get_or", "BadType did not fall back");
		if (shcl_get_int_or(cd, "e", 1, 9) != 9) fail("get_or", "Empty did not fall back");
		if (shcl_get_int_or(cd, "missing", 7, 9) != 9) fail("get_or", "NotFound did not fall back");
		if (shcl_get_float_or(cd, "missing", 7, 1.5) != 1.5) fail("get_or", "float did not fall back");
		if (shcl_get_bool_or(cd, "missing", 7, 1) != 1) fail("get_or", "bool did not fall back");
		// The ok predicate and the convenience tier deliberately disagree on an
		// explicitly emptied field: one asks whether the author spoke for it, the
		// other whether there is a usable value.
		if (!shcl_status_ok(shcl_read_int(cd, "e", 1).status)) fail("status_ok", "an emptied field is not ok");
		if (shcl_status_ok(shcl_read_int(cd, "missing", 7).status)) fail("status_ok", "a missing field is ok");
		shcl_free(cd);
	}
	// load_file/save_file: the status separates absent / unreadable / parsed
	// with errors / clean, and a save round-trips through the atomic write.
	// Same fixture in every runner.
	{
		char tdir[256], tfile[288];
		snprintf(tdir, sizeof tdir, "%s/shcl-filetier-%ld", tmp_root(), (long)getpid());
#ifdef _WIN32
		if (_mkdir(tdir) != 0) fail("file_tier", "mkdir failed");
#else
		if (mkdir(tdir, 0700) != 0) fail("file_tier", "mkdir failed");
#endif
		snprintf(tfile, sizeof tfile, "%s/t.shcl", tdir);
		shcl_file_status fst;
		shcl_doc *fd = shcl_load_file(tfile, &fst);
		if (fst != SHCL_FILE_NOT_FOUND) fail("file_tier", "missing file status");
		shcl_free(fd);
		fd = shcl_load_file(tdir, &fst); // a directory is not readable
		if (fst != SHCL_FILE_UNREADABLE) fail("file_tier", "directory status");
		shcl_free(fd);
		// Bad encoding is unreadable too: the parser assumes well-formed text, so a
		// binary file loading clean would read back mangled and a later save would
		// write the mangled version over the original.
		{
			FILE *bf = fopen(tfile, "wb");
			if (!bf || fputs("a: 1\nb: \xff\xfe bad\n", bf) == EOF || fclose(bf) != 0) fail("file_tier", "seed write failed");
			fd = shcl_load_file(tfile, &fst);
			if (fst != SHCL_FILE_UNREADABLE) fail("file_tier", "bad encoding status");
			if (shcl_to_canonical(fd).n != 0) fail("file_tier", "bad encoding document");
			shcl_free(fd);
		}
		FILE *tf = fopen(tfile, "wb");
		if (!tf || fputs("a: 1\n: broken\n", tf) == EOF || fclose(tf) != 0) fail("file_tier", "seed write failed");
		fd = shcl_load_file(tfile, &fst);
		if (fst != SHCL_FILE_HAD_ERRORS) fail("file_tier", "broken file status");
		if (shcl_get_int(fd, "a", 1, 0) != 1) fail("file_tier", "broken file read");
		shcl_free(fd);
		tf = fopen(tfile, "wb");
		if (!tf || fputs("a: 1\nb: x\n", tf) == EOF || fclose(tf) != 0) fail("file_tier", "seed rewrite failed");
		fd = shcl_load_file(tfile, &fst);
		if (fst != SHCL_FILE_CLEAN) fail("file_tier", "clean file status");
		// shcl_read_file is the load's read half on its own: the exact bytes,
		// or the status. The cap counts bytes, and a file exactly at it passes.
		// Same fixture in every runner.
		{
			size_t rn = 0; shcl_file_status rst;
			char *rt = shcl_read_file(tfile, 0, &rn, &rst);
			if (!rt || rst != SHCL_FILE_CLEAN || rn != 10 || memcmp(rt, "a: 1\nb: x\n", 11) != 0) fail("file_tier", "read_file");
			free(rt);
			rt = shcl_read_file(tfile, 10, &rn, &rst);
			if (!rt || rst != SHCL_FILE_CLEAN || rn != 10) fail("file_tier", "read_file at the cap");
			free(rt);
			rt = shcl_read_file(tfile, 9, &rn, &rst);
			if (rt || rst != SHCL_FILE_UNREADABLE) fail("file_tier", "read_file past the cap");
			free(rt);
			// A cap spelled as the type maximum used to overflow the over-cap
			// probe and read nothing. Same fixture in every runner.
			rt = shcl_read_file(tfile, (size_t)-1, &rn, &rst);
			if (!rt || rst != SHCL_FILE_CLEAN || rn != 10 || memcmp(rt, "a: 1\nb: x\n", 11) != 0) fail("file_tier", "read_file at the largest cap");
			free(rt);
			char none[320];
			snprintf(none, sizeof none, "%s/none.shcl", tdir);
			rt = shcl_read_file(none, 0, &rn, &rst);
			if (rt || rst != SHCL_FILE_NOT_FOUND) fail("file_tier", "read_file missing");
			free(rt);
		}
		if (!shcl_set_int(fd, "c", 1, 3)) fail("file_tier", "set_int failed");
		if (shcl_save_file(fd, tfile) != SHCL_SAVE_OK) fail("file_tier", "save failed");
		shcl_doc *fb = shcl_load_file(tfile, &fst);
		shcl_str c1 = shcl_to_canonical(fd), c2 = shcl_to_canonical(fb);
		if (fst != SHCL_FILE_CLEAN || c1.n != c2.n || memcmp(c1.p, c2.p, c1.n) != 0) fail("file_tier", "save round-trip mismatch");
		shcl_free(fb); shcl_free(fd);
		// Creating a file and overwriting one are two different code paths in
		// the write - the create picks its own mode, the overwrite copies the
		// target's, and the publish step differs by platform (windows goes
		// through ReplaceFile, with a rename fallback). Both run everywhere: an
		// overwrite used to throw outright on windows in the python binding,
		// which no POSIX-only fixture could ever have caught. Same fixture in
		// every runner.
		{
			char fresh[320];
			snprintf(fresh, sizeof fresh, "%s/fresh.shcl", tdir);
			shcl_doc *nd = shcl_parse("a: 1\n", 5);
			for (int pass = 0; pass < 2; pass++) {
				if (shcl_save_file(nd, fresh) != SHCL_SAVE_OK) fail("file_tier", pass ? "overwrite save failed" : "new file save failed");
				shcl_doc *nb = shcl_load_file(fresh, &fst);
				shcl_str nc = shcl_to_canonical(nb);
				if (fst != SHCL_FILE_CLEAN || nc.n != 5 || memcmp(nc.p, "a: 1\n", 5) != 0) fail("file_tier", pass ? "overwritten file round-trip" : "new file round-trip");
				shcl_free(nb);
			}
			// A new file lands where an ordinary create lands - 0666 narrowed
			// by the umask - and an existing one keeps the mode it had. Neither
			// is visible on stdout, so no corpus case can see either, and
			// neither is a windows concept, so the mode half is POSIX-only.
#ifndef _WIN32
			{
				char probe[320], born[320];
				snprintf(probe, sizeof probe, "%s/probe", tdir);
				snprintf(born, sizeof born, "%s/born.shcl", tdir);
				FILE *pf = fopen(probe, "wb");
				if (!pf || fclose(pf) != 0) fail("file_tier", "probe create failed");
				struct stat ps, ns;
				if (stat(probe, &ps) != 0) fail("file_tier", "probe stat failed");
				if (shcl_save_file(nd, born) != SHCL_SAVE_OK) fail("file_tier", "new file save failed");
				if (stat(born, &ns) != 0) fail("file_tier", "new file stat failed");
				if ((ns.st_mode & 0777) != (ps.st_mode & 0777)) fail("file_tier", "new file mode");
				if (chmod(born, 0640) != 0) fail("file_tier", "chmod failed");
				if (shcl_save_file(nd, born) != SHCL_SAVE_OK) fail("file_tier", "existing file save failed");
				if (stat(born, &ns) != 0) fail("file_tier", "existing file stat failed");
				if ((ns.st_mode & 0777) != 0640) fail("file_tier", "existing file mode");
				// setuid and setgid come over too: applying the mode before
				// the data lets the kernel clear them on the write.
				if (chmod(born, 06750) == 0 && stat(born, &ns) == 0 && (ns.st_mode & 07777) == 06750) {
					if (shcl_save_file(nd, born) != SHCL_SAVE_OK) fail("file_tier", "set-id save failed");
					if (stat(born, &ns) != 0) fail("file_tier", "set-id stat failed");
					if ((ns.st_mode & 07777) != 06750) fail("file_tier", "set-id bits lost");
				}
				remove(probe); remove(born);
			}
#endif
			// Windows-only, and C-only in effect: a path spelled with the platform
			// separator, a name outside the active code page, and a drive-relative
			// target. The fixture above joins with '/', which is why none of these
			// ever failed here - the writer split on '/' alone, and every file call
			// was the code-page one, so a backslash path failed outright and a
			// non-ANSI name went to disk under a mojibake spelling. The other
			// bindings split and open through their runtimes and never had it.
#ifdef _WIN32
			{
				char bsfile[320], u8file[320], drfile[320], cwd[320];
				snprintf(bsfile, sizeof bsfile, "%s\\bs.shcl", tdir);
				for (char *p = bsfile; *p; p++) if (*p == '/') *p = '\\';
				if (shcl_save_file(nd, bsfile) != SHCL_SAVE_OK) fail("file_tier", "backslash path save failed");
				shcl_doc *bb = shcl_load_file(bsfile, &fst);
				if (fst != SHCL_FILE_CLEAN) fail("file_tier", "backslash path load");
				shcl_free(bb); remove(bsfile);
				// Checked through the wide API on purpose: the narrow calls would
				// write and read back the same wrong name, so a round trip through
				// them proves nothing.
				snprintf(u8file, sizeof u8file, "%s/\xe6\x97\xa5.shcl", tdir);
				if (shcl_save_file(nd, u8file) != SHCL_SAVE_OK) fail("file_tier", "utf-8 name save failed");
				wchar_t wname[320];
				if (MultiByteToWideChar(CP_UTF8, 0, u8file, -1, wname, 320) == 0) fail("file_tier", "widen failed");
				if (GetFileAttributesW(wname) == INVALID_FILE_ATTRIBUTES) fail("file_tier", "utf-8 name not on disk under its own spelling");
				bb = shcl_load_file(u8file, &fst);
				if (fst != SHCL_FILE_CLEAN) fail("file_tier", "utf-8 name load");
				shcl_free(bb); _wremove(wname);
				// `C:x` names x in C:'s current directory, so the save has to land
				// beside the fixture's other files once that directory is tdir.
				if (tdir[1] == ':' && _getcwd(cwd, sizeof cwd) && _chdir(tdir) == 0) {
					snprintf(drfile, sizeof drfile, "%c:dr.shcl", tdir[0]);
					if (shcl_save_file(nd, drfile) != SHCL_SAVE_OK) fail("file_tier", "drive-relative save failed");
					if (_chdir(cwd) != 0) fail("file_tier", "chdir back failed");
					snprintf(drfile, sizeof drfile, "%s/dr.shcl", tdir);
					bb = shcl_load_file(drfile, &fst);
					if (fst != SHCL_FILE_CLEAN) fail("file_tier", "drive-relative save landed elsewhere");
					shcl_free(bb); remove(drfile);
				}
			}
#endif
			shcl_free(nd);
			remove(fresh);
		}
		// Content-malformed lines are retained as trivia (lost 0, the line
		// survives a save); position-dependent drops count as lost and make
		// shcl_save_file refuse until the caller opts into the lossy save.
		// Same fixture in every runner.
		const char *kt = "a: 1\nsquare-miles 300\nb: 2\n";
		shcl_doc *kd = shcl_parse(kt, strlen(kt));
		if (shcl_lost_count(kd) != 0) fail("lost", "kept lost_count not 0");
		shcl_str kc = shcl_to_canonical(kd);
		if (!contains(kc.p, kc.n, "square-miles 300\n")) fail("lost", "retained line missing");
		const char *lt2 = "a:\n\tb: 1\n  c: 2\n"; // indent matches no level
		shcl_doc *lo = shcl_parse(lt2, strlen(lt2));
		if (shcl_lost_count(lo) != 1) fail("lost", "lost_count not 1");
		if (shcl_save_file(kd, tfile) != SHCL_SAVE_OK) fail("lost", "kept save failed");
		shcl_doc *kb = shcl_load_file(tfile, &fst);
		shcl_str kbc = shcl_to_canonical(kb);
		if (!contains(kbc.p, kbc.n, "square-miles 300\n")) fail("lost", "retained line lost through save");
		shcl_free(kb);
		if (shcl_save_file(lo, tfile) != SHCL_SAVE_REFUSED) fail("lost", "save did not refuse a lossy save");
		if (shcl_save_file_lossy(lo, tfile) != SHCL_SAVE_OK) fail("lost", "lossy save failed");
		// A refusal and a failed write are separate values, not two spellings of
		// one message, and the gate answers before any i/o - so an unwritable path
		// still reports the refusal. Same fixture in every runner.
		char bad[320];
		snprintf(bad, sizeof bad, "%s/nope/t.shcl", tdir);
		if (shcl_save_file(kd, bad) != SHCL_SAVE_FAILED) fail("lost", "a failed write did not report as one");
		if (shcl_save_file(lo, bad) != SHCL_SAVE_REFUSED) fail("lost", "refusal did not survive an unwritable path");
		shcl_free(lo); shcl_free(kd);
		remove(tfile); rmdir(tdir);
	}
	// set_raw: the body's shared indent survives a reload (the closing fence's
	// indent is what comes off), the info-string is stored as a fence line
	// reads it back, and an info with a line break or an unquoted `#` has no
	// spelling and fails the write. Same fixture in every runner.
	{
		shcl_doc *sd = shcl_new();
		if (!shcl_set_raw(sd, "q", 1, "  a\n  b", 7, " sql ", 5)) fail("set_raw", "set_raw failed");
		shcl_str sc = shcl_to_canonical(sd);
		shcl_doc *back = shcl_parse(sc.p, sc.n);
		shcl_read_str br = shcl_read_raw(back, "q", 1);
		if (br.status != SHCL_GOOD || br.value.n != 7 || memcmp(br.value.p, "  a\n  b", 7) != 0) fail("set_raw", "shared indent did not survive a reload");
		shcl_read_str bi = shcl_read_raw_info(back, "q", 1);
		if (bi.status != SHCL_GOOD || bi.value.n != 3 || memcmp(bi.value.p, "sql", 3) != 0) fail("set_raw", "info not trimmed");
		if (shcl_set_raw(sd, "q", 1, "x", 1, "a\nb", 3)) fail("set_raw", "info with a newline accepted");
		if (shcl_set_raw(sd, "q", 1, "x", 1, "a\rb", 3)) fail("set_raw", "info with a CR accepted");
		br = shcl_read_raw(sd, "q", 1);
		if (br.status != SHCL_GOOD || br.value.n != 7 || memcmp(br.value.p, "  a\n  b", 7) != 0) fail("set_raw", "refused write changed the document");
		if (shcl_set_raw(sd, "q", 1, "x", 1, "a # b", 5)) fail("set_raw", "info with an unquoted # accepted");
		if (!shcl_set_raw(sd, "q", 1, "  a\n  b", 7, "\"a # b\"", 7)) fail("set_raw", "quoted # info refused");
		shcl_free(back);
		sc = shcl_to_canonical(sd);
		back = shcl_parse(sc.p, sc.n);
		br = shcl_read_raw(back, "q", 1);
		if (br.status != SHCL_GOOD || br.value.n != 7 || memcmp(br.value.p, "  a\n  b", 7) != 0) fail("set_raw", "content did not survive the reload");
		bi = shcl_read_raw_info(back, "q", 1);
		if (bi.status != SHCL_GOOD || bi.value.n != 7 || memcmp(bi.value.p, "\"a # b\"", 7) != 0) fail("set_raw", "quoted # info did not round-trip");
		shcl_free(back); shcl_free(sd);
	}
	// A link to a file that is not there yet is written through like any
	// other link: the file appears where the link points and the link stays a
	// link. Same fixture in every POSIX runner.
#ifndef _WIN32
	{
		char ddir[256], dreal[288], dlink[288], dtarget[320];
		snprintf(ddir, sizeof ddir, "%s/shcl-dangling-%ld", tmp_root(), (long)getpid());
		snprintf(dreal, sizeof dreal, "%s/real", ddir);
		snprintf(dlink, sizeof dlink, "%s/c.shcl", ddir);
		snprintf(dtarget, sizeof dtarget, "%s/real/c.shcl", ddir);
		if (mkdir(ddir, 0700) != 0 || mkdir(dreal, 0700) != 0) fail("dangling", "mkdir failed");
		if (symlink("real/c.shcl", dlink) != 0) fail("dangling", "symlink failed");
		shcl_doc *dd = shcl_parse("a: 1\n", 5);
		if (shcl_save_file(dd, dlink) != SHCL_SAVE_OK) fail("dangling", "save through a dangling link failed");
		struct stat ls;
		if (lstat(dlink, &ls) != 0 || !S_ISLNK(ls.st_mode)) fail("dangling", "the link is no longer a link");
		size_t tn; char *tt = read_file(dtarget, &tn);
		if (!tt || tn != 5 || memcmp(tt, "a: 1\n", 5) != 0) fail("dangling", "file not created behind the link");
		free(tt); shcl_free(dd);
		remove(dlink); remove(dtarget); rmdir(dreal); rmdir(ddir);
	}
#endif
	// A read-only target is rewritten, as it is on POSIX, and comes back
	// read-only; no temp file is left behind. Same fixture in every runner.
#ifdef _WIN32
	{
		char rdir[256], rfile[288];
		snprintf(rdir, sizeof rdir, "%s/shcl-readonly-%ld", tmp_root(), (long)getpid());
		snprintf(rfile, sizeof rfile, "%s/ro.shcl", rdir);
		if (_mkdir(rdir) != 0) fail("readonly", "mkdir failed");
		FILE *rf = fopen(rfile, "wb");
		if (!rf || fputs("a: 1\n", rf) == EOF || fclose(rf) != 0) fail("readonly", "seed write failed");
		if (!SetFileAttributesA(rfile, GetFileAttributesA(rfile) | FILE_ATTRIBUTE_READONLY)) fail("readonly", "set read-only failed");
		shcl_doc *rd = shcl_parse("a: 2\n", 5);
		if (shcl_save_file(rd, rfile) != SHCL_SAVE_OK) fail("readonly", "save over a read-only file failed");
		size_t rn; char *rt = read_file(rfile, &rn);
		if (!rt || rn != 5 || memcmp(rt, "a: 2\n", 5) != 0) fail("readonly", "file not rewritten");
		free(rt);
		if (!(GetFileAttributesA(rfile) & FILE_ATTRIBUTE_READONLY)) fail("readonly", "file did not come back read-only");
		DIR *rdd = opendir(rdir); int left = 0; const struct dirent *re;
		while (rdd && (re = readdir(rdd))) if (re->d_name[0] != '.') left++;
		if (rdd) closedir(rdd);
		if (left != 1) fail("readonly", "a temp file was left behind");
		shcl_free(rd);
		SetFileAttributesA(rfile, FILE_ATTRIBUTE_NORMAL);
		remove(rfile); rmdir(rdir);
	}
#endif
	// Reads and saves must not retain: a read of a plain field hands back a
	// slice of the retained input (a million reads once grew a document by
	// 32 MB), and a save emits into scratch (200 saves of 79 KB once grew it by
	// 17 MB). The bound is one arena block, well under either regression.
	{
		char gdir[256], gfile[288];
		snprintf(gdir, sizeof gdir, "%s/shcl-retain-%ld", tmp_root(), (long)getpid());
		snprintf(gfile, sizeof gfile, "%s/g.shcl", gdir);
#ifdef _WIN32
		if (_mkdir(gdir) != 0) fail("retain", "mkdir failed");
#else
		if (mkdir(gdir, 0700) != 0) fail("retain", "mkdir failed");
#endif
		// About 30 KB: 2000 keys, so an unfixed save shows up at once.
		size_t gcap = 2000 * 16 + 1, gn = 0; char *gt = xrealloc(NULL, gcap);
		for (int i = 0; i < 2000; i++) gn += (size_t)snprintf(gt + gn, gcap - gn, "k%04d: v%04d\n", i, i);
		shcl_doc *gd = shcl_parse(gt, gn);
		size_t before = arena_bytes(&gd->arena);
		for (int i = 0; i < 100000; i++) {
			shcl_read_str r = shcl_read_string(gd, "k0042", 5);
			if (r.status != SHCL_GOOD || r.value.n != 5 || memcmp(r.value.p, "v0042", 5) != 0) { fail("retain", "read failed"); break; }
		}
		for (int i = 0; i < 200; i++) if (shcl_save_file(gd, gfile) != SHCL_SAVE_OK) { fail("retain", "save failed"); break; }
		size_t after = arena_bytes(&gd->arena);
		if (after > before + 65536) fail("retain", "reads and saves grew the document arena");
		shcl_free(gd); free(gt);
		remove(gfile); rmdir(gdir);
	}
	// Array reads must not grow the document arena, and the release call has to
	// give back what they do allocate. C-only: the other three hand back owned
	// collections their runtime reclaims, so there is nothing to mirror.
	{
		const char *at = "ports: 80, 443, 8080\n";
		shcl_doc *ad = shcl_parse(at, strlen(at));
		size_t docBefore = arena_bytes(&ad->arena);
		for (int i = 0; i < 20000; i++) {
			shcl_read_i64_arr r = shcl_read_int_array(ad, "ports", 5);
			if (r.status != SHCL_GOOD || r.n != 3 || r.values[1] != 443) { fail("array_retain", "read failed"); break; }
		}
		if (arena_bytes(&ad->arena) > docBefore + 4096) fail("array_retain", "array reads grew the document arena");
		shcl_reads_release(ad);
		if (arena_bytes(&ad->reads) != 0) fail("array_retain", "release did not reclaim the read arena");
		for (int i = 0; i < 20000; i++) {
			shcl_read_i64_arr r = shcl_read_int_array(ad, "ports", 5);
			if (r.status != SHCL_GOOD) { fail("array_retain", "read after release failed"); break; }
			shcl_reads_release(ad);
		}
		if (arena_bytes(&ad->reads) > 4096) fail("array_retain", "releasing per read did not keep the arena flat");
		shcl_free(ad);
	}
	// write_reason: the reason behind a setter's bare 0. Same fixture in every
	// runner.
	{
		const char *wt = "a:\n\tb: 1\n";
		shcl_doc *wd = shcl_parse(wt, strlen(wt));
		if (shcl_write_reason_(wd, "a.b", 3) != SHCL_W_WRITABLE) fail("write_reason", "a.b not writable");
		if (shcl_write_reason_(wd, "a.new[Boston].x", 15) != SHCL_W_WRITABLE) fail("write_reason", "creatable path not writable");
		if (shcl_write_reason_(wd, "", 0) != SHCL_W_BAD_PATH) fail("write_reason", "empty path not bad");
		if (shcl_write_reason_(wd, "a..b", 4) != SHCL_W_BAD_PATH) fail("write_reason", "a..b not bad");
		if (shcl_write_reason_(wd, "a.b: 2", 6) != SHCL_W_VALUE_IN_PATH) fail("write_reason", "value part not flagged");
		if (shcl_write_reason_(wd, "a[*].b", 6) != SHCL_W_WILDCARD) fail("write_reason", "wildcard not flagged");
		if (shcl_write_reason_(wd, "a[#5].b", 7) != SHCL_W_NO_SUCH_INDEX) fail("write_reason", "a[#5] not flagged");
		if (shcl_write_reason_(wd, "nope[#0].b", 10) != SHCL_W_NO_SUCH_INDEX) fail("write_reason", "off-tree index not flagged");
		{
			char deep[1026]; size_t dn = 0; // 513 segments: "d.d.d..."
			for (size_t i = 0; i < 513; i++) { if (i) deep[dn++] = '.'; deep[dn++] = 'd'; }
			if (shcl_write_reason_(wd, deep, dn) != SHCL_W_TOO_DEEP) fail("write_reason", "513 segments not too deep");
		}
		// A literal line break in a SELECTOR: the binding would emit across two
		// lines and reparse as neither, and the value emitter never escapes one.
		// In a NAME it is writable - names emit through the name escaper, which
		// spells a line break \n, so the escaped and literal spellings are one
		// path now. Not corpus-pinnable - an ops line cannot carry a raw newline.
		if (shcl_write_reason_(wd, "a[\"p\nq\"].b", 10) != SHCL_W_BAD_PATH) fail("write_reason", "newline in selector not flagged");
		if (shcl_write_reason_(wd, "\"x\ny\".b", 7) != SHCL_W_WRITABLE) fail("write_reason", "newline in name not writable");
		if (shcl_write_reason_(wd, "\"x\\ny\".b", 8) != SHCL_W_WRITABLE) fail("write_reason", "escaped newline not writable");
		// The probe never creates: the doc is unchanged after all of the above.
		if (shcl_count(wd, "a", 1) != 1) fail("write_reason", "probe created nodes");
		shcl_free(wd);
	}
	// One combined diagnostics list (parse first, then validation) and an
	// error predicate, so recover-and-continue can't read as success by
	// accident. Same fixture in every runner.
	{
		const char *ot = ": nope\nport: x\n";
		const char *os = "field: port\n\ttype: int\n";
		shcl_doc *od = shcl_load_and_validate(ot, strlen(ot), os, strlen(os), SHCL_STANDARD);
		if (shcl_diag_count(od) != 2) fail("oneshot", "diag count not 2");
		else if (strcmp(shcl_diag_code(od, 0), "E014") || strcmp(shcl_diag_code(od, 1), "V003")) fail("oneshot", "codes not E014,V003");
		if (shcl_error_count(od) != 2) fail("oneshot", "error_count not 2");
		shcl_read_str pr = shcl_read_string(od, "port", 4); // doc still usable
		if (pr.status != SHCL_GOOD || pr.value.n != 1 || pr.value.p[0] != 'x') fail("oneshot", "port not readable");
		shcl_free(od);
		// Strict never errors out here; the diagnostics are the answer.
		shcl_doc *sd = shcl_load_and_validate(ot, strlen(ot), os, strlen(os), SHCL_STRICT);
		if (shcl_error_count(sd) < 2) fail("oneshot", "strict error_count < 2");
		shcl_free(sd);
		// An empty schema declares nothing and validates nothing.
		shcl_doc *pd = shcl_load_and_validate("a: 1\n", 5, "", 0, SHCL_STANDARD);
		if (shcl_error_count(pd) != 0 || shcl_diag_count(pd) != 0) fail("oneshot", "plain doc not clean");
		shcl_free(pd);
	}
	if (nfail) { fprintf(stderr, "conformance: %d failure(s)\n", nfail); return 1; }
	printf("conformance: %zu case(s) pass\n", nn);
	return 0;
}
