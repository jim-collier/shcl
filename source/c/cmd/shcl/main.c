// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

// shcl CLI - the C binding's command surface. Flags, output, and exit codes
// mirror the Rust reference exactly; the cicd cross-binding check compares them
// byte for byte, so any drift here fails the pipeline.

#define SHCL_IMPLEMENTATION
#include "shcl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <errno.h>

// Keep in step with source/rust/Cargo.toml, the canonical version source.
#define VERSION "1.0.0-beta1"

static const char *HELP =
	"shcl - Simple Hierarchical Config Language (reference CLI)\n"
	"\n"
	"Usage:\n"
	"  shcl get [type] [options] FILE PATH    read one value (or array) at a path\n"
	"  shcl fmt [--write] FILE                print (or rewrite) the canonical form\n"
	"  shcl check [options] FILE              load and print diagnostics\n"
	"  shcl count [options] FILE PATH         number of instances at a path\n"
	"  shcl instances [options] FILE PATH     instance values at a path, one per line\n"
	"\n"
	"Types (default --string):\n"
	"  --int --float --bool --datetime --string --raw\n"
	"  --array                                read the value as an array of the type\n"
	"\n"
	"Options:\n"
	"  --default=VALUE                        value to print when the read is not Good\n"
	"                                         (implies --on-bad=default; for arrays,\n"
	"                                         substituted per bad slot)\n"
	"  --on-bad=error|default|flag            error: fail loudly; default: print the\n"
	"                                         default; flag: print the value anyway and\n"
	"                                         report via exit code (the default mode)\n"
	"  --slots                                prefix each line with its slot status and\n"
	"                                         a tab (per element, or per wildcard slot)\n"
	"  --strictness=loose|standard|strict     or 1|2|3 (default standard)\n"
	"\n"
	"FILE may be '-' for stdin.\n"
	"\n"
	"Exit codes: 0 good, 1 usage or I/O error, 2 empty, 3 not found, 4 bad type,\n"
	"5 multiple instances, 6 strict load failure.\n";

typedef struct {
	const char *kind;         // int|float|bool|datetime|string|raw
	int array;
	int slots;
	const char *deflt;        // NULL if unset
	const char *on_bad;       // error|default|flag
	shcl_strictness strictness;
	int write;
	const char *args[8]; int nargs; // positional: FILE [PATH]
} Opts;

static void outln(const char *p, size_t n) { fwrite(p, 1, n, stdout); fputc('\n', stdout); }

// Whole-buffer UTF-8 validation, matching Rust read_to_string rejecting bad bytes.
static int utf8_valid(const char *p, size_t n) {
	size_t i = 0;
	while (i < n) {
		unsigned char c = (unsigned char)p[i];
		if (c < 0x80) { i++; continue; }
		size_t need; uint32_t cp; uint32_t lo;
		if ((c >> 5) == 0x6) { need = 1; cp = c & 0x1F; lo = 0x80; }
		else if ((c >> 4) == 0xE) { need = 2; cp = c & 0x0F; lo = 0x800; }
		else if ((c >> 3) == 0x1E) { need = 3; cp = c & 0x07; lo = 0x10000; }
		else return 0;
		if (i + need >= n) return 0;
		for (size_t k = 1; k <= need; k++) { unsigned char cc = (unsigned char)p[i + k]; if ((cc & 0xC0) != 0x80) return 0; cp = (cp << 6) | (cc & 0x3F); }
		if (cp < lo || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
		i += need + 1;
	}
	return 1;
}

// realloc that never returns NULL: on OOM, free the old block and exit 1.
static void *xrealloc(void *p, size_t n) {
	void *q = realloc(p, n);
	if (!q) { free(p); fprintf(stderr, "out of memory\n"); exit(1); }
	return q;
}

// Reads FILE (or stdin for "-") fully. Returns malloc'd buffer + len, or NULL on
// error (message printed to stderr). Rejects invalid UTF-8 like the reference.
static char *read_input(const char *file, size_t *len) {
	char *buf = NULL; size_t cap = 0, n = 0;
	FILE *f = strcmp(file, "-") == 0 ? stdin : fopen(file, "rb");
	if (!f) { fprintf(stderr, "%s: %s\n", file, strerror(errno)); return NULL; }
	char chunk[65536]; size_t r;
	while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
		if (n + r > cap) { cap = (n + r) * 2; buf = (char *)xrealloc(buf, cap ? cap : 1); }
		memcpy(buf + n, chunk, r); n += r;
	}
	int ferr = ferror(f);
	if (f != stdin) fclose(f);
	if (ferr) { fprintf(stderr, "%s: read error\n", file); free(buf); return NULL; }
	if (!buf) { buf = (char *)malloc(1); }
	if (!utf8_valid(buf, n)) { fprintf(stderr, "%s: stream did not contain valid UTF-8\n", file); free(buf); return NULL; }
	*len = n; return buf;
}

// Prints diagnostics to stderr and returns 6 on strict load failure, else 0.
static int strict_gate(shcl_doc *d) {
	if (!shcl_strict_failed(d)) return 0;
	size_t n = shcl_diag_count(d);
	for (size_t i = 0; i < n; i++) {
		shcl_str m = shcl_diag_message(d, i);
		fprintf(stderr, "line %zu: %s: ", shcl_diag_line(d, i), shcl_diag_severity(d, i) == SHCL_SEV_ERROR ? "Error" : "Hint");
		fwrite(m.p, 1, m.n, stderr); fputc('\n', stderr);
	}
	size_t nerr = 0; for (size_t i = 0; i < n; i++) if (shcl_diag_severity(d, i) == SHCL_SEV_ERROR) nerr++;
	fprintf(stderr, "strict load failed: %zu error diagnostic(s)\n", nerr);
	return 6;
}

static int do_get(Opts *o) {
	if (o->nargs != 2) { fprintf(stderr, "get needs FILE and PATH (see --help)\n"); return 1; }
	const char *file = o->args[0], *path = o->args[1]; size_t plen = strlen(path);
	size_t len; char *text = read_input(file, &len);
	if (!text) return 1;
	shcl_doc *d = shcl_parse_with(text, len, o->strictness);
	int gate = strict_gate(d);
	if (gate) { shcl_free(d); free(text); return gate; }

	shcl_status status = SHCL_GOOD;
	const shcl_status *slotSts = NULL; size_t nSlots = 0;
	char fbuf[SHCL_F64_BUF];
	// Buffer output lines so the on-bad modes can suppress them uniformly. Each
	// line either borrows arena/const memory (owned=0) or is formatted into own[].
	// Owned entries keep p NULL: own[] lives inside the growable array, so a stored
	// self-pointer goes stale on realloc - LINEPTR picks the live one at print time.
	struct { const char *p; size_t n; char own[SHCL_F64_BUF]; int owned; } *lines = NULL;
	size_t nlines = 0, clines = 0;
	#define LINEPTR(I) (lines[I].owned ? lines[I].own : lines[I].p)
	#define PUSHLINE_BYTES(P, N) do { if (nlines == clines) { clines = clines ? clines * 2 : 8; lines = xrealloc(lines, clines * sizeof *lines); } lines[nlines].p = (P); lines[nlines].n = (N); lines[nlines].owned = 0; nlines++; } while (0)
	#define PUSHLINE_FMT(FMT, ...) do { if (nlines == clines) { clines = clines ? clines * 2 : 8; lines = xrealloc(lines, clines * sizeof *lines); } int k = snprintf(lines[nlines].own, SHCL_F64_BUF, FMT, __VA_ARGS__); lines[nlines].p = NULL; lines[nlines].n = (size_t)k; lines[nlines].owned = 1; nlines++; } while (0)
	#define PUSHLINE_BUF(B, N) do { if (nlines == clines) { clines = clines ? clines * 2 : 8; lines = xrealloc(lines, clines * sizeof *lines); } memcpy(lines[nlines].own, (B), (N)); lines[nlines].p = NULL; lines[nlines].n = (N); lines[nlines].owned = 1; nlines++; } while (0)

	if (o->array) {
		if (!strcmp(o->kind, "int")) { shcl_read_i64_arr r = shcl_read_int_array(d, path, plen); status = r.status; slotSts = r.statuses; nSlots = r.n; for (size_t i = 0; i < r.n; i++) PUSHLINE_FMT("%" PRId64, r.values[i]); }
		else if (!strcmp(o->kind, "float")) { shcl_read_f64_arr r = shcl_read_float_array(d, path, plen); status = r.status; slotSts = r.statuses; nSlots = r.n; for (size_t i = 0; i < r.n; i++) { size_t k = shcl_format_f64(r.values[i], fbuf); PUSHLINE_BUF(fbuf, k); } }
		else if (!strcmp(o->kind, "bool")) { shcl_read_bool_arr r = shcl_read_bool_array(d, path, plen); status = r.status; slotSts = r.statuses; nSlots = r.n; for (size_t i = 0; i < r.n; i++) PUSHLINE_BYTES(r.values[i] ? "true" : "false", r.values[i] ? 4 : 5); }
		else if (!strcmp(o->kind, "datetime")) { shcl_read_dt_arr r = shcl_read_datetime_array(d, path, plen); status = r.status; slotSts = r.statuses; nSlots = r.n; for (size_t i = 0; i < r.n; i++) { size_t k = shcl_datetime_str(&r.values[i], fbuf); PUSHLINE_BUF(fbuf, k); } }
		else if (!strcmp(o->kind, "raw")) { fprintf(stderr, "--raw has no --array form\n"); free(lines); shcl_free(d); free(text); return 1; }
		else { shcl_read_str_arr r = shcl_read_string_array(d, path, plen); status = r.status; slotSts = r.statuses; nSlots = r.n; for (size_t i = 0; i < r.n; i++) PUSHLINE_BYTES(r.values[i].p, r.values[i].n); }
	} else {
		if (!strcmp(o->kind, "int")) { shcl_read_i64 r = shcl_read_int(d, path, plen); status = r.status; PUSHLINE_FMT("%" PRId64, r.value); }
		else if (!strcmp(o->kind, "float")) { shcl_read_f64 r = shcl_read_float(d, path, plen); status = r.status; size_t k = shcl_format_f64(r.value, fbuf); PUSHLINE_BUF(fbuf, k); }
		else if (!strcmp(o->kind, "bool")) { shcl_read_bool r = shcl_read_bool_(d, path, plen); status = r.status; PUSHLINE_BYTES(r.value ? "true" : "false", r.value ? 4 : 5); }
		else if (!strcmp(o->kind, "datetime")) { shcl_read_dt r = shcl_read_datetime(d, path, plen); status = r.status; size_t k = shcl_datetime_str(&r.value, fbuf); PUSHLINE_BUF(fbuf, k); }
		else if (!strcmp(o->kind, "raw")) { shcl_read_str r = shcl_read_raw(d, path, plen); status = r.status; PUSHLINE_BYTES(r.value.p, r.value.n); }
		else { shcl_read_str r = shcl_read_string(d, path, plen); status = r.status; PUSHLINE_BYTES(r.value.p, r.value.n); }
	}

	// Per-line slot status: falls back to the aggregate for scalar reads.
	#define SLOT_AT(I) (slotSts && (I) < nSlots ? slotSts[I] : status)
	#define EMITLINE(I, P, N) do { if (o->slots) printf("%s\t", shcl_status_name(SLOT_AT(I))); outln((P), (N)); } while (0)
	int rc;
	int flag_ok = (status == SHCL_GOOD) || (status == SHCL_EMPTY && !strcmp(o->on_bad, "flag"));
	if (flag_ok) {
		for (size_t i = 0; i < nlines; i++) EMITLINE(i, LINEPTR(i), lines[i].n);
		rc = shcl_status_code(status);
	} else if (!strcmp(o->on_bad, "default")) {
		const char *dv = o->deflt ? o->deflt : "";
		if (slotSts && nSlots > 0) {
			// Array read: the default substitutes per bad slot; alignment holds.
			for (size_t i = 0; i < nlines; i++) {
				if (SLOT_AT(i) == SHCL_GOOD) EMITLINE(i, LINEPTR(i), lines[i].n);
				else EMITLINE(i, dv, strlen(dv));
			}
		} else {
			if (o->slots) printf("%s\t", shcl_status_name(status));
			outln(dv, strlen(dv));
		}
		rc = 0;
	} else if (!strcmp(o->on_bad, "error")) {
		fprintf(stderr, "%s: %s\n", path, shcl_status_name(status)); rc = shcl_status_code(status);
	} else {
		for (size_t i = 0; i < nlines; i++) EMITLINE(i, LINEPTR(i), lines[i].n);
		rc = shcl_status_code(status);
	}
	free(lines); shcl_free(d); free(text); return rc;
}

static int do_fmt(Opts *o) {
	if (o->nargs != 1) { fprintf(stderr, "fmt needs FILE (see --help)\n"); return 1; }
	const char *file = o->args[0];
	size_t len; char *text = read_input(file, &len);
	if (!text) return 1;
	shcl_doc *d = shcl_parse_with(text, len, o->strictness);
	int gate = strict_gate(d);
	if (gate) { shcl_free(d); free(text); return gate; }
	shcl_str c = shcl_to_canonical(d);
	int rc = 0;
	if (o->write && strcmp(file, "-") != 0) {
		FILE *f = fopen(file, "wb");
		if (!f) { fprintf(stderr, "%s: %s\n", file, strerror(errno)); rc = 1; }
		else { fwrite(c.p, 1, c.n, f); fclose(f); }
	} else {
		fwrite(c.p, 1, c.n, stdout);
	}
	shcl_free(d); free(text); return rc;
}

static int do_check(Opts *o) {
	if (o->nargs != 1) { fprintf(stderr, "check needs FILE (see --help)\n"); return 1; }
	size_t len; char *text = read_input(o->args[0], &len);
	if (!text) return 1;
	shcl_doc *d = shcl_parse_with(text, len, o->strictness);
	size_t n = shcl_diag_count(d);
	for (size_t i = 0; i < n; i++) {
		shcl_str m = shcl_diag_message(d, i);
		printf("line %zu: %s: ", shcl_diag_line(d, i), shcl_diag_severity(d, i) == SHCL_SEV_ERROR ? "Error" : "Hint");
		fwrite(m.p, 1, m.n, stdout); fputc('\n', stdout);
	}
	int rc;
	if (shcl_strict_failed(d)) {
		size_t nerr = 0; for (size_t i = 0; i < n; i++) if (shcl_diag_severity(d, i) == SHCL_SEV_ERROR) nerr++;
		printf("strict load failed: %zu error diagnostic(s)\n", nerr); rc = 6;
	} else {
		printf("ok (%zu diagnostic(s))\n", n); rc = 0;
	}
	shcl_free(d); free(text); return rc;
}

static int do_enum(Opts *o, int want_count) {
	if (o->nargs != 2) { fprintf(stderr, "count/instances need FILE and PATH (see --help)\n"); return 1; }
	const char *file = o->args[0], *path = o->args[1]; size_t plen = strlen(path);
	size_t len; char *text = read_input(file, &len);
	if (!text) return 1;
	shcl_doc *d = shcl_parse_with(text, len, o->strictness);
	int gate = strict_gate(d);
	if (gate) { shcl_free(d); free(text); return gate; }
	if (want_count) printf("%zu\n", shcl_count(d, path, plen));
	else { shcl_str *vals; size_t n = shcl_instances(d, path, plen, &vals); for (size_t i = 0; i < n; i++) outln(vals[i].p, vals[i].n); }
	shcl_free(d); free(text); return 0;
}

static int parse_opts(int argc, char **argv, int from, Opts *o) {
	o->kind = "string"; o->array = 0; o->slots = 0; o->deflt = NULL; o->on_bad = "flag";
	o->strictness = SHCL_STANDARD; o->write = 0; o->nargs = 0;
	for (int i = from; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--int") || !strcmp(a, "--float") || !strcmp(a, "--bool") || !strcmp(a, "--datetime") || !strcmp(a, "--string") || !strcmp(a, "--raw")) o->kind = a + 2;
		else if (!strcmp(a, "--array")) o->array = 1;
		else if (!strcmp(a, "--slots")) o->slots = 1;
		else if (!strcmp(a, "--write") || !strcmp(a, "-w")) o->write = 1;
		else if (!strncmp(a, "--default=", 10)) { o->deflt = a + 10; o->on_bad = "default"; }
		else if (!strncmp(a, "--on-bad=", 9)) {
			const char *v = a + 9;
			if (strcmp(v, "error") && strcmp(v, "default") && strcmp(v, "flag")) { fprintf(stderr, "bad --on-bad value: %s\n", v); return 1; }
			o->on_bad = v;
		} else if (!strncmp(a, "--strictness=", 13)) {
			const char *v = a + 13;
			if (!shcl_strictness_from_arg(v, strlen(v), &o->strictness)) { fprintf(stderr, "bad --strictness value: %s\n", v); return 1; }
		} else if (a[0] == '-' && a[1] != '\0') { fprintf(stderr, "unknown option: %s\n", a); return 1; }
		else { if (o->nargs < 8) o->args[o->nargs++] = a; }
	}
	return 0;
}

int main(int argc, char **argv) {
	setlocale(LC_ALL, "C"); // strtod/printf must use '.' regardless of environment
	int has_help = 0, has_version = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) has_help = 1;
		if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) has_version = 1;
	}
	if (argc <= 1 || has_help) { fputs(HELP, stdout); return argc <= 1 ? 1 : 0; }
	if (has_version) { printf("shcl %s\n", VERSION); return 0; }
	const char *cmd = argv[1];
	Opts o;
	if (parse_opts(argc, argv, 2, &o)) return 1;
	if (!strcmp(cmd, "get")) return do_get(&o);
	if (!strcmp(cmd, "fmt")) return do_fmt(&o);
	if (!strcmp(cmd, "check")) return do_check(&o);
	if (!strcmp(cmd, "count")) return do_enum(&o, 1);
	if (!strcmp(cmd, "instances")) return do_enum(&o, 0);
	fprintf(stderr, "unknown command: %s (see --help)\n", cmd);
	return 1;
}
