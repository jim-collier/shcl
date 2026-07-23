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
#include <dirent.h>

static int nfail = 0;
static void fail(const char *at, const char *msg) { fprintf(stderr, "FAIL %s: %s\n", at, msg); nfail++; }

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

// Apply one write-ops line (NUL-terminated, tab-split in place) via the library
// Writer. A "-default" suffix means "only if absent".
static void apply_op_c(shcl_doc *d, char *line) {
	size_t cap = 8, nf = 0; char **f = (char **)xrealloc(NULL, cap * sizeof *f);
	f[nf++] = line;
	for (char *p = line; *p; p++) if (*p == '\t') { *p = '\0'; if (nf == cap) { cap *= 2; f = (char **)xrealloc(f, cap * sizeof *f); } f[nf++] = p + 1; }
	char *op = f[0];
	const char *path = nf > 1 ? f[1] : ""; size_t plen = nf > 1 ? strlen(f[1]) : 0;
	const char *v = nf > 2 ? f[2] : ""; size_t vn = nf > 2 ? strlen(f[2]) : 0;
	size_t an = nf > 2 ? nf - 2 : 0;
	size_t oplen = strlen(op);
	if (oplen >= 8 && !strcmp(op + oplen - 8, "-default")) {
		if (shcl_exists(d, path, plen)) { free(f); return; }
		op[oplen - 8] = '\0';
	}
	if (!strcmp(op, "int")) shcl_set_int(d, path, plen, strtoll(v, NULL, 10));
	else if (!strcmp(op, "float")) shcl_set_float(d, path, plen, strtod(v, NULL));
	else if (!strcmp(op, "bool")) shcl_set_bool(d, path, plen, !strcmp(v, "true"));
	else if (!strcmp(op, "string")) { char *b = (char *)xrealloc(NULL, vn ? vn : 1); size_t m = cf_unescape(v, vn, b); shcl_set_string(d, path, plen, b, m); free(b); }
	else if (!strcmp(op, "datetime")) { shcl_datetime dt; S sv; sv.p = v; sv.n = vn; if (parse_datetime(&d->arena, sv, &dt)) shcl_set_datetime(d, path, plen, &dt); }
	else if (!strcmp(op, "int-array")) {
		int64_t *a = (int64_t *)xrealloc(NULL, (an ? an : 1) * sizeof *a);
		for (size_t i = 0; i < an; i++) a[i] = strtoll(f[2 + i], NULL, 10);
		shcl_set_int_array(d, path, plen, a, an); free(a);
	}
	else if (!strcmp(op, "float-array")) {
		double *a = (double *)xrealloc(NULL, (an ? an : 1) * sizeof *a);
		for (size_t i = 0; i < an; i++) a[i] = strtod(f[2 + i], NULL);
		shcl_set_float_array(d, path, plen, a, an); free(a);
	}
	else if (!strcmp(op, "bool-array")) {
		int *a = (int *)xrealloc(NULL, (an ? an : 1) * sizeof *a);
		for (size_t i = 0; i < an; i++) a[i] = !strcmp(f[2 + i], "true");
		shcl_set_bool_array(d, path, plen, a, an); free(a);
	}
	else if (!strcmp(op, "string-array")) {
		char **sv = (char **)xrealloc(NULL, (an ? an : 1) * sizeof *sv); size_t *sl = (size_t *)xrealloc(NULL, (an ? an : 1) * sizeof *sl);
		for (size_t i = 0; i < an; i++) { size_t L = strlen(f[2 + i]); char *b = (char *)xrealloc(NULL, L ? L : 1); sl[i] = cf_unescape(f[2 + i], L, b); sv[i] = b; }
		shcl_set_string_array(d, path, plen, (const char *const *)sv, sl, an);
		for (size_t i = 0; i < an; i++) free(sv[i]);
		free(sv); free(sl);
	}
	else if (!strcmp(op, "datetime-array")) {
		shcl_datetime *a = (shcl_datetime *)xrealloc(NULL, (an ? an : 1) * sizeof *a); int ok = 1;
		for (size_t i = 0; i < an; i++) { S sv; sv.p = f[2 + i]; sv.n = strlen(f[2 + i]); if (!parse_datetime(&d->arena, sv, &a[i])) ok = 0; }
		if (ok) shcl_set_datetime_array(d, path, plen, a, an);
		free(a);
	}
	else if (!strcmp(op, "raw")) { const char *cont = nf > 3 ? f[3] : ""; size_t cn = nf > 3 ? strlen(f[3]) : 0; char *b = (char *)xrealloc(NULL, cn ? cn : 1); size_t m = cf_unescape(cont, cn, b); shcl_set_raw(d, path, plen, b, m, v, vn); free(b); }
	else if (!strcmp(op, "empty")) shcl_set_empty(d, path, plen);
	else if (!strcmp(op, "comment")) shcl_set_comment(d, path, plen, v, vn);
	else if (!strcmp(op, "remove")) shcl_remove(d, path, plen);
	else { fprintf(stderr, "unknown op %s\n", op); nfail++; }
	free(f);
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
		free(input); free(expected); free(reads);
	}
	for (size_t i = 0; i < nn; i++) free(names[i]);
	free(names);
	if (nfail) { fprintf(stderr, "conformance: %d failure(s)\n", nfail); return 1; }
	printf("conformance: %zu case(s) pass\n", nn);
	return 0;
}
