// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

// Conformance-corpus runner for the C binding. Same corpus every shipped binding
// must pass; column meanings live in project/conformance/README.md. Exit nonzero
// on any miss. Corpus root is argv[1] (default project/conformance, run from the
// repo root as cicd does).

#define _POSIX_C_SOURCE 200809L // strdup, opendir/readdir under -std=c11
#define SHCL_IMPLEMENTATION
#include "shcl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <dirent.h>

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
	else if (!strcmp(op, "bool")) { if (!PRESENT) wrote = shcl_set_bool(d, path, plen, !strcmp(v, "true")); }
	else if (!strcmp(op, "literal")) { if (!PRESENT) wrote = shcl_set_literal(d, path, plen, v, vn); }
	else if (!strcmp(op, "string")) { if (!PRESENT) { char *b = (char *)xrealloc(NULL, vn ? vn : 1); size_t m = cf_unescape(v, vn, b); wrote = shcl_set_string(d, path, plen, b, m); free(b); } }
	else if (!strcmp(op, "datetime")) { shcl_datetime dt; S sv; sv.p = v; sv.n = vn; if (!parse_datetime(&d->arena, sv, &dt)) rc = 1; else if (!PRESENT) wrote = shcl_set_datetime(d, path, plen, &dt); }
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
		for (size_t i = 0; i < an; i++) a[i] = !strcmp(f[2 + i], "true");
		if (!PRESENT) wrote = shcl_set_bool_array(d, path, plen, a, an);
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
		for (size_t i = 0; i < an && !rc; i++) { S sv; sv.p = f[2 + i]; sv.n = strlen(f[2 + i]); if (!parse_datetime(&d->arena, sv, &a[i])) rc = 1; }
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

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char **)a, *(const char **)b); }

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
	char **names = NULL; size_t nn = 0, cn = 0; struct dirent *de;
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
				if (vv) shcl_suppress_declared_repeats(sd, vd);
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
				struct dirent *de;
				while ((de = readdir(cd))) {
					size_t dn = strlen(de->d_name);
					if (dn > 5 && !strncmp(de->d_name, "layer", 5) && !strcmp(de->d_name + dn - 5, ".shcl") && nlayer < 64)
						snprintf(layerNames[nlayer++], 256, "%s", de->d_name);
				}
				closedir(cd);
			}
			for (int a = 0; a < nlayer; a++) for (int b = a + 1; b < nlayer; b++) if (strcmp(layerNames[a], layerNames[b]) > 0) { char tmp[256]; memcpy(tmp, layerNames[a], 256); memcpy(layerNames[a], layerNames[b], 256); memcpy(layerNames[b], tmp, 256); }
			// Fold: layer0 (base) then each higher layer, then input.shcl.
			char *ltexts[65]; size_t llens[65]; int nt = 0;
			shcl_doc *md = NULL;
			for (int li = 0; li < nlayer; li++) {
				snprintf(path, sizeof path, "%s/%s/%s", corpus, names[ci], layerNames[li]);
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
					char *eq = strchr(slines[li], '=');
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
	// line()/children(): read-surface accessors. Same fixture in every runner
	// (the read structs stay value+status here by design).
	{
		const char *lt = "a: @null\nb: \"@null\"\ncode:\n\thook: 1\n\thook: 2\n\tdone: 3\n";
		shcl_doc *ld = shcl_parse(lt, strlen(lt));
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
		shcl_str *cv; size_t cn = shcl_children(ld, "code", 4, &cv);
		const char *cw[] = { "hook", "hook", "done" };
		if (cn != 3) fail("children", "code count mismatch");
		else for (size_t i = 0; i < 3; i++) if (cv[i].n != strlen(cw[i]) || memcmp(cv[i].p, cw[i], cv[i].n) != 0) { fail("children", "code names mismatch"); break; }
		cn = shcl_children(ld, "", 0, &cv);
		const char *rw[] = { "a", "b", "code" };
		if (cn != 3) fail("children", "root count mismatch");
		else for (size_t i = 0; i < 3; i++) if (cv[i].n != strlen(rw[i]) || memcmp(cv[i].p, rw[i], cv[i].n) != 0) { fail("children", "root names mismatch"); break; }
		if (shcl_children(ld, "missing", 7, &cv) != 0) fail("children", "missing path not empty");
		shcl_free(ld);
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
