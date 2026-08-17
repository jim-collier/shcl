// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

// shcl CLI - the C binding's command surface. Flags, output, and exit codes
// mirror the Rust reference exactly; the cicd cross-binding check compares them
// byte for byte, so any drift here fails the pipeline.

#ifndef _WIN32
	// fileno/fsync/getpid under -std=c11 need an explicit POSIX feature request.
	// realpath sits behind XSI rather than plain POSIX in glibc, hence both.
	#define _POSIX_C_SOURCE 200809L
	#define _XOPEN_SOURCE 700
#endif

#define SHCL_IMPLEMENTATION
#include "shcl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <fcntl.h>
#ifdef _WIN32
	#include <windows.h>
	#include <io.h>
	#include <process.h>
	#include <sys/stat.h>
	#define getpid _getpid
	#define fdopen _fdopen
	#define close _close
#else
	#include <unistd.h>
	#include <sys/stat.h>
#endif

// Keep in step with source/rust/Cargo.toml, the canonical version source.
#define VERSION "1.2.0"

static const char *HELP =
	"shcl - Simple Hierarchical Config Language (reference CLI)\n"
	"\n"
	"Usage:\n"
	"  shcl get [type] [options] FILE PATH    read one value (or array) at a path\n"
	"  shcl set [--write|-w] [options] FILE   apply edits (--set, or ops on stdin);\n"
	"                                         print canonical (or rewrite FILE in\n"
	"                                         place with --write)\n"
	"  shcl fmt [--write|-w] FILE             print (or rewrite in place) the canonical form\n"
	"  shcl check [options] FILE              load and print diagnostics\n"
	"                                         (--schema=SCHEMA also validates FILE\n"
	"                                         against a schema, itself a .shcl file)\n"
	"  shcl init [--no-banner] --schema=S     print a commented starter config from\n"
	"                                         a schema (required fields live, optional\n"
	"                                         commented, wildcards noted)\n"
	"  shcl count [options] FILE PATH         number of instances at a path\n"
	"  shcl instances [options] FILE PATH     instance values at a path, one per line\n"
	"  shcl help | version                    this help, or the version (also -h/--help, -V/--version)\n"
	"  shcl about | donate                    what shcl is, or how to support it\n"
	"                                         (also --about, --donate)\n"
	"\n"
	"set edits FILE, the base document ('-' = empty base). Values go in as\n"
	"repeatable --set PATH=VALUE (data) or --set-literal PATH=TEXT (value syntax, so\n"
	"arrays work) options, which persist with --write; given either, no ops are read\n"
	"from stdin. Raw blocks, set-only-if-absent and removal go in as a write-ops\n"
	"script on stdin, one op per line, tab-separated. Ops:\n"
	"  int|float|bool|string|datetime<TAB>PATH<TAB>VALUE       set a scalar\n"
	"  <type>-array<TAB>PATH<TAB>V1<TAB>V2...                  set an inline array\n"
	"  <type>[-array]-default<TAB>...                          set only if absent\n"
	"  literal[-default]<TAB>PATH<TAB>TEXT                     set from value syntax\n"
	"  raw<TAB>PATH<TAB>INFO<TAB>CONTENT                       set a raw block\n"
	"  empty<TAB>PATH   comment<TAB>PATH<TAB>TEXT   remove<TAB>PATH\n"
	"string/raw values decode \\n \\t \\\\; a line starting with # is a script comment.\n"
	"\n"
	"Types (default --string):\n"
	"  --int --float --bool --datetime --string --raw --rawinfo\n"
	"  --array                                read the value as an array of the type\n"
	"  --rawinfo reads a raw block's info-string (the fence tag), not its content\n"
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
	"  --no-banner                            (init) leave out the footer naming the\n"
	"                                         format and pointing at its spec\n"
	"  --strictness=loose|standard|strict     or 1|2|3 (default standard)\n"
	"  --schema=SCHEMA                        (check/init) validate FILE against a\n"
	"                                         schema; adds V### diagnostics\n"
	"  --layer=FILE                           (get/fmt/count/instances/set) merge a\n"
	"                                         lower-priority layer under FILE;\n"
	"                                         repeatable, earlier = lower priority\n"
	"  --set=PATH=VALUE                       override one path as the top layer,\n"
	"                                         after all files; repeatable. On 'set'\n"
	"                                         it is an edit to the document itself,\n"
	"                                         so it persists with --write. VALUE\n"
	"                                         goes in as data: its type still\n"
	"                                         follows the text (8 is an int), but a\n"
	"                                         comma or quote in it is content, not\n"
	"                                         syntax\n"
	"  --set-literal=PATH=TEXT                as --set, except TEXT goes in as value\n"
	"                                         syntax the way a file spells it, so\n"
	"                                         'ports=80, 443' writes a two-element\n"
	"                                         array. An unquoted # ends the value;\n"
	"                                         text spanning lines is rejected\n"
	"\n"
	"Value options accept either spelling: --default=VALUE or --default VALUE. In\n"
	"the space form the next argument is taken as the value whatever it looks like,\n"
	"so --default --int reads --int as the default. Use -- to end the options when a\n"
	"FILE or PATH begins with a dash.\n"
	"An option a subcommand does not use is a usage error, not ignored.\n"
	"FILE may be '-' for stdin. With --layer, FILE is the highest file layer and\n"
	"each --layer is merged under it in order; --set applies last. 'fmt' with\n"
	"layers prints the merged canonical document.\n"
	"\n"
	"Exit codes: 0 good, 1 usage or I/O error, 2 empty, 3 not found, 4 bad type,\n"
	"5 multiple instances, 6 check failed or strict load failure.\n";

// About and donate are stdout, so they are byte-for-byte contracts across the
// bindings the same way the help text and the init banner are. The version
// concatenates from the VERSION macro so it cannot drift from `shcl version`.
static const char *ABOUT =
	"shcl v" VERSION "\n"
	"Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞).\n"
	"Project: https://github.com/jim-collier/shcl\n"
	"Licensed under the MIT License. Full text at:\n"
	"  https://spdx.org/licenses/MIT.html\n"
	"No warranty.\n"
	"\n"
	"Simple Hierarchical Config Language. Forgiving to write, predictable to read.\n"
	"Types live in your code, not in the file, so nothing is guessed at parse time.\n"
	"One broken line is skipped with a note instead of taking down the whole file.\n";

static const char *DONATE =
	"shcl is free software under the MIT License, and stays that way.\n"
	"\n"
	"If it saves you time and you want to give something back:\n"
	"  https://github.com/sponsors/jim-collier\n"
	"\n"
	"A star on the project, a clear bug report, or a mention to someone who needs it\n"
	"are worth just as much.\n";

typedef struct {
	const char *kind;         // int|float|bool|datetime|string|raw
	int array;
	int slots;
	const char *deflt;        // NULL if unset
	const char *on_bad;       // error|default|flag
	shcl_strictness strictness;
	int write;
	int no_banner;
	const char *schema;       // NULL if unset
	const char **layers; int nlayers; // lower-priority layers, in listed order (unbounded)
	const char **sets; int nsets;     // final override layer: "path=value" (unbounded)
	const char **set_opts;            // parallel to sets: which spelling produced each
	const char **args; int nargs;     // positional: FILE [PATH]
	const char *seen[16]; int nseen;  // distinct canonical option names, for per-command validation
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

// realloc that never returns NULL: on OOM, free the old block and take the same
// exit-70 path the library arena uses (was a silent segfault on unchecked realloc).
static void *xrealloc(void *p, size_t n) {
	void *q = realloc(p, n);
	if (!q) { free(p); fprintf(stderr, "shcl: out of memory\n"); exit(70); }
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
	if (!buf) { buf = (char *)xrealloc(NULL, 1); }
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

// Holds a merged doc and the input buffers its nodes still reference (the base
// layer's node strings are not dup'd off its text). Free everything with
// layered_free once the doc is done.
typedef struct { shcl_doc *doc; char **texts; int ntexts; } LayeredDoc;

static void layered_push_text(LayeredDoc *L, char *t) {
	L->texts = (char **)xrealloc(L->texts, ((size_t)L->ntexts + 1) * sizeof *L->texts);
	L->texts[L->ntexts++] = t;
}

static void layered_free(LayeredDoc *L) {
	if (L->doc) shcl_free(L->doc);
	for (int i = 0; i < L->ntexts; i++) free(L->texts[i]);
	free(L->texts);
	L->doc = NULL; L->texts = NULL; L->ntexts = 0;
}

// Load `file` with o's lower-priority --layer files underneath it and its --set
// overrides on top - the layered-load fold. Every layer parses at the requested
// strictness; a strict-load failure on any aborts (exit 6). Returns 0 and fills
// *out on success, else an exit code (nothing to free on failure).
// Apply one --set/--set-literal override. Both spellings share a list so they
// apply in the order given, which decides the winner when two target one path.
static int set_apply(shcl_doc *d, const char *spec, const char *opt) {
	const char *eq = strchr(spec, '=');
	size_t plen = (size_t)(eq - spec);
	const char *val = eq + 1; size_t vlen = strlen(val);
	int ok = !strcmp(opt, "--set-literal") ? shcl_set_literal(d, spec, plen, val, vlen)
	                                       : shcl_set_string(d, spec, plen, val, vlen);
	if (!ok) fprintf(stderr, "shcl: cannot write %.*s (from %s)\n", (int)plen, spec, opt);
	return ok;
}

static int load_layered(Opts *o, const char *file, LayeredDoc *out) {
	out->doc = NULL; out->texts = NULL; out->ntexts = 0;
	// Lowest -> highest file layer: the --layer files in order, then FILE.
	for (int i = 0; i <= o->nlayers; i++) {
		const char *fname = i < o->nlayers ? o->layers[i] : file;
		size_t len; char *t = read_input(fname, &len);
		if (!t) { layered_free(out); return 1; }
		layered_push_text(out, t);
		shcl_doc *dd = shcl_parse_with(t, len, o->strictness);
		int g = strict_gate(dd);
		if (g) { shcl_free(dd); layered_free(out); return g; }
		if (!out->doc) out->doc = dd;
		else { shcl_merge(out->doc, dd); shcl_free(dd); }
	}
	for (int i = 0; i < o->nsets; i++) {
		if (!set_apply(out->doc, o->sets[i], o->set_opts[i])) { layered_free(out); return 1; }
	}
	return 0;
}

static int do_get(Opts *o) {
	if (o->nargs != 2) { fprintf(stderr, "get needs FILE and PATH (see --help)\n"); return 1; }
	const char *file = o->args[0], *path = o->args[1]; size_t plen = strlen(path);
	LayeredDoc L; int gate = load_layered(o, file, &L);
	if (gate) return gate;
	shcl_doc *d = L.doc;

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
		else if (!strcmp(o->kind, "raw") || !strcmp(o->kind, "rawinfo")) { fprintf(stderr, "--%s has no --array form\n", o->kind); free(lines); layered_free(&L); return 1; }
		else { shcl_read_str_arr r = shcl_read_string_array(d, path, plen); status = r.status; slotSts = r.statuses; nSlots = r.n; for (size_t i = 0; i < r.n; i++) PUSHLINE_BYTES(r.values[i].p, r.values[i].n); }
	} else {
		if (!strcmp(o->kind, "int")) { shcl_read_i64 r = shcl_read_int(d, path, plen); status = r.status; PUSHLINE_FMT("%" PRId64, r.value); }
		else if (!strcmp(o->kind, "float")) { shcl_read_f64 r = shcl_read_float(d, path, plen); status = r.status; size_t k = shcl_format_f64(r.value, fbuf); PUSHLINE_BUF(fbuf, k); }
		else if (!strcmp(o->kind, "bool")) { shcl_read_bool r = shcl_read_bool_(d, path, plen); status = r.status; PUSHLINE_BYTES(r.value ? "true" : "false", r.value ? 4 : 5); }
		else if (!strcmp(o->kind, "datetime")) { shcl_read_dt r = shcl_read_datetime(d, path, plen); status = r.status; size_t k = shcl_datetime_str(&r.value, fbuf); PUSHLINE_BUF(fbuf, k); }
		else if (!strcmp(o->kind, "raw")) { shcl_read_str r = shcl_read_raw(d, path, plen); status = r.status; PUSHLINE_BYTES(r.value.p, r.value.n); }
		else if (!strcmp(o->kind, "rawinfo")) { shcl_read_str r = shcl_read_raw_info(d, path, plen); status = r.status; PUSHLINE_BYTES(r.value.p, r.value.n); }
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
		char tbuf[32];
		snprintf(tbuf, sizeof tbuf, o->array ? "%s array" : "%s", o->kind);
		if (status == SHCL_BAD_TYPE) {
			shcl_read_str rs = shcl_read_string(d, path, plen);
			if (rs.status == SHCL_GOOD)
				fprintf(stderr, "shcl: cannot read %s as %s: value \"%.*s\" is not a valid %s (in %s)\n", path, tbuf, (int)rs.value.n, rs.value.p, tbuf, file);
			else
				fprintf(stderr, "shcl: cannot read %s as %s: value is not a valid %s (in %s)\n", path, tbuf, tbuf, file);
		} else if (status == SHCL_NOT_FOUND) {
			fprintf(stderr, "shcl: cannot read %s as %s: no value at that path (in %s)\n", path, tbuf, file);
		} else if (status == SHCL_EMPTY) {
			fprintf(stderr, "shcl: cannot read %s as %s: the value is empty (in %s)\n", path, tbuf, file);
		} else {
			fprintf(stderr, "shcl: cannot read %s as %s: the path matches multiple instances (in %s)\n", path, tbuf, file);
		}
		rc = shcl_status_code(status);
	} else {
		for (size_t i = 0; i < nlines; i++) EMITLINE(i, LINEPTR(i), lines[i].n);
		rc = shcl_status_code(status);
	}
	free(lines); layered_free(&L); return rc;
}

// The atomic write itself lives in the library (shcl_write_file_atomic); the
// CLI adds the error report and its 0-ok/1-fail exit convention.
static int write_atomic(const char *file, const char *data, size_t n) {
	if (!shcl_write_file_atomic(file, data, n)) {
		fprintf(stderr, "%s: %s\n", file, strerror(errno));
		return 1;
	}
	return 0;
}

static int do_fmt(Opts *o) {
	if (o->nargs != 1) { fprintf(stderr, "fmt needs FILE (see --help)\n"); return 1; }
	const char *file = o->args[0];
	if (o->write && strcmp(file, "-") == 0) {
		fprintf(stderr, "fmt --write cannot rewrite stdin; drop --write to print, or pass a FILE\n");
		return 1;
	}
	LayeredDoc L; int gate = load_layered(o, file, &L);
	if (gate) return gate;
	shcl_str c = shcl_to_canonical(L.doc);
	int rc = 0;
	if (o->write) {
		rc = write_atomic(file, c.p, c.n);
	} else {
		fwrite(c.p, 1, c.n, stdout);
	}
	layered_free(&L); return rc;
}

// Reads an open stream fully into a malloc'd buffer (ops script; no UTF-8 gate).
static char *read_all_fp(FILE *f, size_t *len) {
	char *buf = NULL; size_t cap = 0, n = 0, r; char chunk[65536];
	while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
		if (n + r > cap) { cap = (n + r) * 2; buf = (char *)xrealloc(buf, cap ? cap : 1); }
		memcpy(buf + n, chunk, r); n += r;
	}
	if (!buf) buf = (char *)xrealloc(NULL, 1);
	*len = n; return buf;
}

// ops-value unescape: \n \t \\ only; other `\x` stays verbatim. out >= inlen.
static size_t unescape_ops(const char *in, size_t inlen, char *out) {
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

// Reference-equivalent op-value gates: the same grammar Rust's i64/f64 FromStr
// accepts, checked before conversion, so `abc`, `0x10`, `1_0`, padded or
// non-ASCII digits, and out-of-range magnitudes are rejected instead of being
// silently coerced (and no fixed staging buffer can truncate a long literal).
static int g_i64(const char *p, size_t n, int64_t *out) {
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
static int g_ci_eq(const char *p, size_t n, const char *kw) {
	size_t kn = strlen(kw);
	if (n != kn) return 0;
	for (size_t i = 0; i < n; i++) { char c = p[i]; if (c >= 'A' && c <= 'Z') c += 32; if (c != kw[i]) return 0; }
	return 1;
}
static int g_f64(const char *p, size_t n, double *out) {
	size_t i = 0;
	if (i < n && (p[i] == '+' || p[i] == '-')) i++;
	if (!(g_ci_eq(p + i, n - i, "inf") || g_ci_eq(p + i, n - i, "infinity") || g_ci_eq(p + i, n - i, "nan"))) {
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
	// Overflow via strtod gives +-inf like Rust, so ERANGE is not an error here.
	char *b = (char *)xrealloc(NULL, n + 1); memcpy(b, p, n); b[n] = 0;
	*out = strtod(b, NULL);
	free(b);
	return 1;
}
static int p_bool(const char *p, size_t n) { return n == 4 && memcmp(p, "true", 4) == 0; }

// Apply one write-ops line. A "-default" suffix means "only if absent": values
// are gated FIRST (a malformed value fails even when the path already exists,
// matching the reference's argument-evaluation order), then the existence probe
// decides whether the base op runs. Returns 0 or 1 (error).
static int apply_op(shcl_doc *d, const char *line, size_t linelen) {
	size_t nf = 1;
	for (size_t i = 0; i < linelen; i++) if (line[i] == '\t') nf++;
	const char **fp = (const char **)xrealloc(NULL, nf * sizeof *fp);
	size_t *fn = (size_t *)xrealloc(NULL, nf * sizeof *fn);
	{ size_t k = 0, start = 0; for (size_t i = 0; i <= linelen; i++) if (i == linelen || line[i] == '\t') { fp[k] = line + start; fn[k] = i - start; k++; start = i + 1; } }
	const char *path = nf > 1 ? fp[1] : ""; size_t plen = nf > 1 ? fn[1] : 0;
	const char *v = nf > 2 ? fp[2] : ""; size_t vn = nf > 2 ? fn[2] : 0;
	int rc = 0, wrote = 1;
	int only_absent = 0;
	if (fn[0] >= 8 && memcmp(fp[0] + fn[0] - 8, "-default", 8) == 0) {
		only_absent = 1;
		fn[0] -= 8; // strip suffix; the base op handles the actual write
	}
	#define OP(s) (fn[0] == strlen(s) && memcmp(fp[0], s, fn[0]) == 0)
	#define PRESENT (only_absent && shcl_exists(d, path, plen))
	size_t an = nf > 2 ? nf - 2 : 0; // array element count (fields from index 2)
	if (OP("int")) { int64_t x; if (!g_i64(v, vn, &x)) { fprintf(stderr, "bad int: %.*s\n", (int)vn, v); rc = 1; } else if (!PRESENT) wrote = shcl_set_int(d, path, plen, x); }
	else if (OP("float")) { double x; if (!g_f64(v, vn, &x)) { fprintf(stderr, "bad float: %.*s\n", (int)vn, v); rc = 1; } else if (!PRESENT) wrote = shcl_set_float(d, path, plen, x); }
	else if (OP("bool")) { if (!PRESENT) wrote = shcl_set_bool(d, path, plen, p_bool(v, vn)); }
	else if (OP("string")) { if (!PRESENT) { char *b = (char *)xrealloc(NULL, vn ? vn : 1); size_t m = unescape_ops(v, vn, b); wrote = shcl_set_string(d, path, plen, b, m); free(b); } }
	else if (OP("datetime")) { shcl_datetime dt; S sv; sv.p = v; sv.n = vn; if (!parse_datetime(&d->arena, sv, &dt)) { fprintf(stderr, "bad datetime: %.*s\n", (int)vn, v); rc = 1; } else if (!PRESENT) wrote = shcl_set_datetime(d, path, plen, &dt); }
	else if (OP("literal")) { if (!PRESENT) wrote = shcl_set_literal(d, path, plen, v, vn); }
	else if (OP("int-array")) { int64_t *a = (int64_t *)xrealloc(NULL, (an ? an : 1) * sizeof *a); for (size_t i = 0; i < an && !rc; i++) if (!g_i64(fp[2 + i], fn[2 + i], &a[i])) { fprintf(stderr, "bad int: %.*s\n", (int)fn[2 + i], fp[2 + i]); rc = 1; } if (!rc && !PRESENT) wrote = shcl_set_int_array(d, path, plen, a, an); free(a); }
	else if (OP("float-array")) { double *a = (double *)xrealloc(NULL, (an ? an : 1) * sizeof *a); for (size_t i = 0; i < an && !rc; i++) if (!g_f64(fp[2 + i], fn[2 + i], &a[i])) { fprintf(stderr, "bad float: %.*s\n", (int)fn[2 + i], fp[2 + i]); rc = 1; } if (!rc && !PRESENT) wrote = shcl_set_float_array(d, path, plen, a, an); free(a); }
	else if (OP("bool-array")) { int *a = (int *)xrealloc(NULL, (an ? an : 1) * sizeof *a); for (size_t i = 0; i < an; i++) a[i] = p_bool(fp[2 + i], fn[2 + i]); if (!PRESENT) wrote = shcl_set_bool_array(d, path, plen, a, an); free(a); }
	else if (OP("string-array")) {
		if (!PRESENT) {
			char **sv = (char **)xrealloc(NULL, (an ? an : 1) * sizeof *sv); size_t *sl = (size_t *)xrealloc(NULL, (an ? an : 1) * sizeof *sl);
			for (size_t i = 0; i < an; i++) { char *b = (char *)xrealloc(NULL, fn[2 + i] ? fn[2 + i] : 1); sl[i] = unescape_ops(fp[2 + i], fn[2 + i], b); sv[i] = b; }
			wrote = shcl_set_string_array(d, path, plen, (const char *const *)sv, sl, an);
			for (size_t i = 0; i < an; i++) free(sv[i]);
			free(sv); free(sl);
		}
	}
	else if (OP("datetime-array")) {
		shcl_datetime *a = (shcl_datetime *)xrealloc(NULL, (an ? an : 1) * sizeof *a);
		for (size_t i = 0; i < an && !rc; i++) { S sv; sv.p = fp[2 + i]; sv.n = fn[2 + i]; if (!parse_datetime(&d->arena, sv, &a[i])) { fprintf(stderr, "bad datetime: %.*s\n", (int)fn[2 + i], fp[2 + i]); rc = 1; } }
		if (!rc && !PRESENT) wrote = shcl_set_datetime_array(d, path, plen, a, an);
		free(a);
	}
	else if (OP("raw")) { if (!PRESENT) { const char *cont = nf > 3 ? fp[3] : ""; size_t contn = nf > 3 ? fn[3] : 0; char *b = (char *)xrealloc(NULL, contn ? contn : 1); size_t m = unescape_ops(cont, contn, b); wrote = shcl_set_raw(d, path, plen, b, m, v, vn); free(b); } }
	else if (OP("empty") && !only_absent) wrote = shcl_set_empty(d, path, plen);
	else if (OP("comment") && !only_absent) wrote = shcl_set_comment(d, path, plen, v, vn);
	else if (OP("remove") && !only_absent) shcl_remove(d, path, plen);
	else { fprintf(stderr, "unknown op\n"); rc = 1; }
	if (rc == 0 && !wrote) { fprintf(stderr, "cannot write %.*s\n", (int)plen, path); rc = 1; }
	#undef PRESENT
	#undef OP
	free(fp); free(fn);
	return rc;
}

static int do_set(Opts *o) {
	if (o->nargs != 1) { fprintf(stderr, "set needs FILE (ops on stdin; see --help)\n"); return 1; }
	const char *file = o->args[0];
	if (o->write && strcmp(file, "-") == 0) {
		fprintf(stderr, "set --write cannot rewrite stdin; drop --write to print, or pass a FILE\n");
		return 1;
	}
	// Base doc: '-' means an empty base, since stdin carries the ops script. Any
	// --layer files sit under it and --set overrides sit on top, before ops. The
	// base layer's node strings are not dup'd off its text, so keep all buffers.
	LayeredDoc L; L.doc = NULL; L.texts = NULL; L.ntexts = 0;
	for (int i = 0; i < o->nlayers; i++) {
		size_t llen; char *lt = read_input(o->layers[i], &llen);
		if (!lt) { layered_free(&L); return 1; }
		layered_push_text(&L, lt);
		shcl_doc *dd = shcl_parse_with(lt, llen, o->strictness);
		int g = strict_gate(dd);
		if (g) { shcl_free(dd); layered_free(&L); return g; }
		if (!L.doc) L.doc = dd; else { shcl_merge(L.doc, dd); shcl_free(dd); }
	}
	char *text; size_t len;
	if (!strcmp(file, "-")) { text = (char *)xrealloc(NULL, 1); len = 0; }
	else { text = read_input(file, &len); if (!text) { layered_free(&L); return 1; } }
	layered_push_text(&L, text);
	{
		shcl_doc *dd = shcl_parse_with(text, len, o->strictness);
		int gate = strict_gate(dd);
		if (gate) { shcl_free(dd); layered_free(&L); return gate; }
		if (!L.doc) L.doc = dd; else { shcl_merge(L.doc, dd); shcl_free(dd); }
	}
	shcl_doc *d = L.doc;
	for (int i = 0; i < o->nsets; i++) {
		if (!set_apply(d, o->sets[i], o->set_opts[i])) { layered_free(&L); return 1; }
	}
	// --set carries the edits, so stdin is left alone: reading it here would
	// block on the console for anyone who passed edits as options.
	size_t opslen = 0; char *ops = NULL;
	if (o->nsets == 0) {
		ops = read_all_fp(stdin, &opslen);
		// The ops script gets the same UTF-8 gate as any file input (exit 1).
		if (!utf8_valid(ops, opslen)) {
			fprintf(stderr, "stdin: stream did not contain valid UTF-8\n");
			free(ops); layered_free(&L); return 1;
		}
	}
	int rc = 0; size_t start = 0;
	for (size_t i = 0; i <= opslen; i++) {
		if (i == opslen || ops[i] == '\n') {
			size_t end = i;
			if (end > start && ops[end - 1] == '\r') end--; // match Rust lines() CRLF
			size_t n = end - start;
			if (n > 0 && ops[start] != '#') { if (apply_op(d, ops + start, n)) { rc = 1; break; } }
			if (i == opslen) break;
			start = i + 1;
		}
	}
	if (rc == 0) {
		shcl_str c = shcl_to_canonical(d);
		if (o->write) rc = write_atomic(file, c.p, c.n);
		else fwrite(c.p, 1, c.n, stdout);
	}
	free(ops); layered_free(&L); return rc;
}

static int do_check(Opts *o) {
	if (o->nargs != 1) { fprintf(stderr, "check needs FILE (see --help)\n"); return 1; }
	size_t len; char *text = read_input(o->args[0], &len);
	if (!text) return 1;
	shcl_doc *d = shcl_parse_with(text, len, o->strictness);
	// --schema: append validation diagnostics under the same contract. The
	// schema itself always loads at Standard (a program artifact); one that
	// does not load cleanly is a single V099 schema fault.
	shcl_validation *val = NULL;
	shcl_doc *sd = NULL;
	char *stext = NULL;
	int v99 = 0;
	if (!shcl_strict_failed(d) && o->schema) {
		size_t slen; stext = read_input(o->schema, &slen);
		if (!stext) { shcl_free(d); free(text); return 1; }
		sd = shcl_parse(stext, slen);
		size_t sn = shcl_diag_count(sd);
		for (size_t i = 0; i < sn; i++) if (shcl_diag_severity(sd, i) == SHCL_SEV_ERROR) v99 = 1;
		if (v99) {
			for (size_t i = 0; i < sn; i++) {
				const char *sev = shcl_diag_severity(sd, i) == SHCL_SEV_ERROR ? "Error" : "Hint";
				shcl_str m = shcl_diag_message(sd, i);
				fprintf(stderr, "schema line %zu: %s: ", shcl_diag_line(sd, i), sev);
				fwrite(m.p, 1, m.n, stderr); fputc('\n', stderr);
			}
		} else {
			val = shcl_validate(d, sd);
			shcl_suppress_declared_repeats(sd, d);
			shcl_suppress_declared_reopens(sd, d);
		}
	}
	size_t n = shcl_diag_count(d), nerr = 0;
	size_t nval = val ? shcl_validation_count(val) : 0;
	size_t total = n + nval + (v99 ? 1 : 0);
	// stdout carries the stable codes - the cross-binding contract. The prose is
	// per-binding voice and goes to stderr (which the differential check drops).
	for (size_t i = 0; i < n; i++) {
		const char *sev = shcl_diag_severity(d, i) == SHCL_SEV_ERROR ? "Error" : "Hint";
		if (shcl_diag_severity(d, i) == SHCL_SEV_ERROR) nerr++;
		printf("line %zu: %s: %s\n", shcl_diag_line(d, i), sev, shcl_diag_code(d, i));
		shcl_str m = shcl_diag_message(d, i);
		fprintf(stderr, "line %zu: %s: ", shcl_diag_line(d, i), sev);
		fwrite(m.p, 1, m.n, stderr); fputc('\n', stderr);
	}
	if (v99) {
		printf("line 0: Error: V099\n");
		fprintf(stderr, "line 0: Error: schema failed to load\n");
		nerr++;
	}
	for (size_t i = 0; i < nval; i++) {
		const char *sev = shcl_validation_severity(val, i) == SHCL_SEV_ERROR ? "Error" : "Hint";
		if (shcl_validation_severity(val, i) == SHCL_SEV_ERROR) nerr++;
		const char *code = shcl_validation_code(val, i);
		printf("line %zu: %s: %s\n", shcl_validation_line(val, i), sev, code);
		// A V090-V093 line number is a SCHEMA line (the code table says so);
		// the prose names the file so the number spaces cannot be confused.
		const char *space = (!strncmp(code, "V09", 3) && strcmp(code, "V099")) ? "schema line" : "line";
		shcl_str m = shcl_validation_message(val, i);
		fprintf(stderr, "%s %zu: %s: ", space, shcl_validation_line(val, i), sev);
		fwrite(m.p, 1, m.n, stderr); fputc('\n', stderr);
	}
	int rc;
	if (shcl_strict_failed(d)) {
		printf("strict load failed: %zu diagnostic(s)\n", total); rc = 6;
	} else if (nerr > 0) {
		// Loaded, but lines were dropped: nonzero so a CI gate on check catches it.
		printf("failed: %zu diagnostic(s), %zu error(s)\n", total, nerr); rc = 6;
	} else {
		printf("ok (%zu diagnostic(s))\n", total); rc = 0;
	}
	shcl_validation_free(val);
	if (sd) shcl_free(sd);
	free(stext);
	shcl_free(d); free(text); return rc;
}

static int do_init(Opts *o) {
	if (o->nargs > 0) { fprintf(stderr, "init takes no file argument (see --help)\n"); return 1; }
	if (!o->schema) { fprintf(stderr, "init needs --schema=FILE (see --help)\n"); return 1; }
	size_t slen; char *stext = read_input(o->schema, &slen);
	if (!stext) return 1;
	// The schema always loads at Standard - a program artifact, not user data.
	shcl_doc *sd = shcl_parse(stext, slen);
	int bad = 0; size_t sn = shcl_diag_count(sd);
	for (size_t i = 0; i < sn; i++) if (shcl_diag_severity(sd, i) == SHCL_SEV_ERROR) bad = 1;
	if (bad) {
		for (size_t i = 0; i < sn; i++) {
			const char *sev = shcl_diag_severity(sd, i) == SHCL_SEV_ERROR ? "Error" : "Hint";
			shcl_str m = shcl_diag_message(sd, i);
			fprintf(stderr, "schema line %zu: %s: ", shcl_diag_line(sd, i), sev);
			fwrite(m.p, 1, m.n, stderr); fputc('\n', stderr);
		}
		fprintf(stderr, "init: schema failed to load\n");
		// A broken schema is a config-semantics failure, not a usage error:
		// same exit as `check --schema` reporting it.
		shcl_free(sd); free(stext); return 6;
	}
	int ok = 0;
	shcl_str text = shcl_generate(sd, o->no_banner, &ok);
	if (!ok) {
		// The generator's ok flag carries no fault detail; validating an empty
		// document against the schema reproduces the same V09x fault list.
		shcl_doc *ed = shcl_parse("", 0);
		shcl_validation *val = shcl_validate(ed, sd);
		size_t nv = shcl_validation_count(val);
		for (size_t i = 0; i < nv; i++) {
			const char *sev = shcl_validation_severity(val, i) == SHCL_SEV_ERROR ? "Error" : "Hint";
			shcl_str m = shcl_validation_message(val, i);
			fprintf(stderr, "schema line %zu: %s: ", shcl_validation_line(val, i), sev);
			fwrite(m.p, 1, m.n, stderr); fputc('\n', stderr);
		}
		shcl_validation_free(val); shcl_free(ed);
		fprintf(stderr, "init: schema has faults\n");
		shcl_free(sd); free(stext); return 6;
	}
	fwrite(text.p, 1, text.n, stdout);
	shcl_free(sd); free(stext); return 0;
}

static int do_enum(Opts *o, int want_count) {
	if (o->nargs != 2) { fprintf(stderr, "count/instances need FILE and PATH (see --help)\n"); return 1; }
	const char *file = o->args[0], *path = o->args[1]; size_t plen = strlen(path);
	LayeredDoc L; int gate = load_layered(o, file, &L);
	if (gate) return gate;
	shcl_doc *d = L.doc;
	if (want_count) printf("%zu\n", shcl_count(d, path, plen));
	else { shcl_str *vals; size_t n = shcl_instances(d, path, plen, &vals); for (size_t i = 0; i < n; i++) outln(vals[i].p, vals[i].n); }
	layered_free(&L); return 0;
}

// Record a distinct canonical option name for per-command validation.
static void opt_seen(Opts *o, const char *name) {
	for (int i = 0; i < o->nseen; i++) if (!strcmp(o->seen[i], name)) return;
	if (o->nseen < 16) o->seen[o->nseen++] = name;
}

static void opt_push(const char ***arr, int *n, const char *v) {
	*arr = (const char **)xrealloc((void *)*arr, ((size_t)*n + 1) * sizeof **arr);
	(*arr)[(*n)++] = v;
}

static void opts_free(Opts *o) {
	free((void *)o->layers); free((void *)o->sets); free((void *)o->set_opts); free((void *)o->args);
	o->layers = o->sets = o->set_opts = o->args = NULL; o->nlayers = o->nsets = o->nargs = 0;
}


// Apply a value-taking option's value. Returns 0 ok, 1 on a bad value.
static int set_value_opt(Opts *o, const char *name, const char *v) {
	if (!strcmp(name, "--default")) { o->deflt = v; o->on_bad = "default"; opt_seen(o, "--default"); }
	else if (!strcmp(name, "--on-bad")) {
		if (strcmp(v, "error") && strcmp(v, "default") && strcmp(v, "flag")) { fprintf(stderr, "bad --on-bad value: %s\n", v); return 1; }
		o->on_bad = v; opt_seen(o, "--on-bad");
	} else if (!strcmp(name, "--strictness")) {
		if (!shcl_strictness_from_arg(v, strlen(v), &o->strictness)) { fprintf(stderr, "bad --strictness value: %s\n", v); return 1; }
		opt_seen(o, "--strictness");
	} else if (!strcmp(name, "--schema")) {
		o->schema = v; opt_seen(o, "--schema");
	} else if (!strcmp(name, "--layer")) {
		opt_push(&o->layers, &o->nlayers, v); opt_seen(o, "--layer");
	} else if (!strcmp(name, "--set") || !strcmp(name, "--set-literal")) {
		if (!strchr(v, '=')) { fprintf(stderr, "bad %s value (want PATH=VALUE): %s\n", name, v); return 1; }
		// set_opts grows in lockstep with sets, so the local count is discarded.
		int nopt = o->nsets;
		opt_push(&o->set_opts, &nopt, name);
		opt_push(&o->sets, &o->nsets, v); opt_seen(o, name);
	}
	return 0;
}

static int parse_opts(int argc, char **argv, int from, Opts *o) {
	o->kind = "string"; o->array = 0; o->slots = 0; o->deflt = NULL; o->on_bad = "flag";
	o->strictness = SHCL_STANDARD; o->write = 0; o->no_banner = 0; o->schema = NULL;
	o->layers = o->sets = o->set_opts = o->args = NULL; o->nlayers = o->nsets = o->nargs = 0; o->nseen = 0;
	// Value-taking options accept both --opt=VALUE and the space form --opt VALUE.
	for (int i = from; i < argc; i++) {
		const char *a = argv[i];
		// Everything after `--` is positional, so a file or path may begin
		// with a dash.
		if (!strcmp(a, "--")) {
			for (int k = i + 1; k < argc; k++) opt_push(&o->args, &o->nargs, argv[k]);
			return 0;
		}
		if (!strcmp(a, "--int") || !strcmp(a, "--float") || !strcmp(a, "--bool") || !strcmp(a, "--datetime") || !strcmp(a, "--string") || !strcmp(a, "--raw") || !strcmp(a, "--rawinfo")) { o->kind = a + 2; opt_seen(o, "--<type>"); }
		else if (!strcmp(a, "--array")) { o->array = 1; opt_seen(o, "--array"); }
		else if (!strcmp(a, "--slots")) { o->slots = 1; opt_seen(o, "--slots"); }
		else if (!strcmp(a, "--write") || !strcmp(a, "-w")) { o->write = 1; opt_seen(o, "--write"); }
		else if (!strcmp(a, "--no-banner")) { o->no_banner = 1; opt_seen(o, "--no-banner"); }
		else if (!strcmp(a, "--default") || !strcmp(a, "--on-bad") || !strcmp(a, "--strictness") || !strcmp(a, "--schema") || !strcmp(a, "--layer") || !strcmp(a, "--set") || !strcmp(a, "--set-literal")) {
			if (i + 1 >= argc) { fprintf(stderr, "missing value for %s (try %s=VALUE)\n", a, a); return 1; }
			if (set_value_opt(o, a, argv[++i])) return 1;
		}
		else if (!strncmp(a, "--default=", 10)) { if (set_value_opt(o, "--default", a + 10)) return 1; }
		else if (!strncmp(a, "--on-bad=", 9)) { if (set_value_opt(o, "--on-bad", a + 9)) return 1; }
		else if (!strncmp(a, "--strictness=", 13)) { if (set_value_opt(o, "--strictness", a + 13)) return 1; }
		else if (!strncmp(a, "--schema=", 9)) { if (set_value_opt(o, "--schema", a + 9)) return 1; }
		else if (!strncmp(a, "--layer=", 8)) { if (set_value_opt(o, "--layer", a + 8)) return 1; }
		else if (!strncmp(a, "--set-literal=", 14)) { if (set_value_opt(o, "--set-literal", a + 14)) return 1; }
		else if (!strncmp(a, "--set=", 6)) { if (set_value_opt(o, "--set", a + 6)) return 1; }
		else if (a[0] == '-' && a[1] != '\0') { fprintf(stderr, "unknown option: %s\n", a); return 1; }
		else opt_push(&o->args, &o->nargs, a);
	}
	return 0;
}

// Every option must be meaningful for its subcommand; an option that would be
// silently ignored (`set --write` before it existed, `--schema` on `get`) is a
// usage error instead.
static int check_opts(const char *cmd, Opts *o) {
	static const char *get_ok[] = { "--<type>", "--array", "--slots", "--default", "--on-bad", "--strictness", "--layer", "--set", "--set-literal", NULL };
	static const char *set_ok[] = { "--strictness", "--layer", "--set", "--set-literal", "--write", NULL };
	static const char *fmt_ok[] = { "--write", "--strictness", "--layer", "--set", "--set-literal", NULL };
	static const char *check_ok[] = { "--strictness", "--schema", NULL };
	static const char *init_ok[] = { "--schema", "--no-banner", NULL };
	static const char *enum_ok[] = { "--strictness", "--layer", "--set", "--set-literal", NULL };
	static const char *none_ok[] = { NULL };
	const char **allowed = none_ok;
	if (!strcmp(cmd, "get")) allowed = get_ok;
	else if (!strcmp(cmd, "set")) allowed = set_ok;
	else if (!strcmp(cmd, "fmt")) allowed = fmt_ok;
	else if (!strcmp(cmd, "check")) allowed = check_ok;
	else if (!strcmp(cmd, "init")) allowed = init_ok;
	else if (!strcmp(cmd, "count") || !strcmp(cmd, "instances")) allowed = enum_ok;
	for (int i = 0; i < o->nseen; i++) {
		int ok = 0;
		for (int k = 0; allowed[k]; k++) if (!strcmp(o->seen[i], allowed[k])) { ok = 1; break; }
		if (!ok) {
			if (!strcmp(o->seen[i], "--<type>")) fprintf(stderr, "type options are not valid for %s (see --help)\n", cmd);
			else fprintf(stderr, "option %s not valid for %s (see --help)\n", o->seen[i], cmd);
			return 1;
		}
	}
	// Writing back the merged document would fold the lower layers permanently
	// into the top file, which is the opposite of what layering is for. On 'set'
	// the --set values are edits to the document rather than a layer over it, so
	// persisting them is the whole point; everywhere else they stay ephemeral.
	if (o->write && o->nlayers > 0) {
		fprintf(stderr, "--write cannot be combined with --layer (see --help)\n");
		return 1;
	}
	if (o->write && o->nsets > 0 && strcmp(cmd, "set")) {
		fprintf(stderr, "--write cannot be combined with --set (see --help)\n");
		return 1;
	}
	// The ops script already has stdin, so a layer cannot read it too.
	if (!strcmp(cmd, "set")) {
		for (int i = 0; i < o->nlayers; i++) {
			if (!strcmp(o->layers[i], "-")) {
				fprintf(stderr, "--layer=- is not valid for set (stdin carries the ops script)\n");
				return 1;
			}
		}
	}
	return 0;
}

// Did the command line ask for one of the informational outputs? Only tokens
// in option position count: a value that happens to read `-h`, and anything
// after the file, are data. Scanning the whole line for them let a read of a
// missing path answer with the help text and exit 0.
static const char *asked_for(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-h") || !strcmp(a, "--help")) return "help";
		if (!strcmp(a, "-V") || !strcmp(a, "--version")) return "version";
		if (!strcmp(a, "--about")) return "about";
		if (!strcmp(a, "--donate")) return "donate";
		if (!strcmp(a, "--")) return NULL;
		if (!strcmp(a, "--default") || !strcmp(a, "--on-bad") || !strcmp(a, "--strictness") || !strcmp(a, "--schema") || !strcmp(a, "--layer") || !strcmp(a, "--set") || !strcmp(a, "--set-literal")) { i++; continue; }
		if (a[0] == '-' && a[1] != '\0') continue;
		// The subcommand, then the file: past that everything is a path.
		if (i > 1) return NULL;
	}
	return NULL;
}

int main(int argc, char **argv) {
	setlocale(LC_ALL, "C"); // strtod/printf must use '.' regardless of environment
	// Reject non-UTF-8 argv up front (exit 1), matching the reference; the parser
	// assumes valid UTF-8, and a garbled arg is a usage error, not a real miss.
	for (int i = 1; i < argc; i++) {
		if (!utf8_valid(argv[i], strlen(argv[i]))) {
			fprintf(stderr, "invalid argument encoding (expected UTF-8)\n");
			return 1;
		}
	}
	const char *asked = asked_for(argc, argv);
	// Bare invocation is a usage error, so it prints the help unpadded. The
	// blank lines below are for a person who asked, to separate the block from
	// the surrounding prompts.
	if (argc <= 1) { fputs(HELP, stdout); return 1; }
	if ((asked && !strcmp(asked, "help")) || !strcmp(argv[1], "help")) { printf("\n%s\n", HELP); return 0; }
	if ((asked && !strcmp(asked, "version")) || !strcmp(argv[1], "version")) { printf("shcl %s\n", VERSION); return 0; }
	if ((asked && !strcmp(asked, "about")) || !strcmp(argv[1], "about")) { printf("\n%s\n", ABOUT); return 0; }
	if ((asked && !strcmp(asked, "donate")) || !strcmp(argv[1], "donate")) { printf("\n%s\n", DONATE); return 0; }
	const char *cmd = argv[1];
	Opts o;
	if (parse_opts(argc, argv, 2, &o)) { opts_free(&o); return 1; }
	int rc;
	if (check_opts(cmd, &o)) rc = 1;
	else if (!strcmp(cmd, "get")) rc = do_get(&o);
	else if (!strcmp(cmd, "set")) rc = do_set(&o);
	else if (!strcmp(cmd, "fmt")) rc = do_fmt(&o);
	else if (!strcmp(cmd, "check")) rc = do_check(&o);
	else if (!strcmp(cmd, "init")) rc = do_init(&o);
	else if (!strcmp(cmd, "count")) rc = do_enum(&o, 1);
	else if (!strcmp(cmd, "instances")) rc = do_enum(&o, 0);
	else { fprintf(stderr, "unknown command: %s (see --help)\n", cmd); rc = 1; }
	opts_free(&o);
	return rc;
}
