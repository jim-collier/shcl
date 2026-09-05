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
#include <math.h>
#include <fcntl.h>
#include <stdarg.h>
#ifdef _WIN32
	#include <windows.h>
	#include <shellapi.h>
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
#define VERSION "2.0.0"

static const char *HELP =
	"shcl - Simple Hierarchical Config Language (reference CLI)\n"
	"\n"
	"Usage:\n"
	"  shcl get [type] [options] FILE PATH    read one value (or array) at a path\n"
	"  shcl set [--write|-w] [options] FILE   apply edits (--set, or ops on stdin);\n"
	"                                         print canonical (or rewrite FILE in\n"
	"                                         place with --write)\n"
	"  shcl fmt [--write|-w] FILE             print the canonical form (or rewrite\n"
	"                                         FILE in place with --write)\n"
	"  shcl check [options] FILE              load and print diagnostics\n"
	"                                         (--schema=SCHEMA also validates FILE\n"
	"                                         against a schema, itself a .shcl file)\n"
	"  shcl init [--no-banner] --schema=S     print a commented starter config\n"
	"                                         from a schema (required fields live,\n"
	"                                         optional commented, wildcards noted)\n"
	"  shcl count [options] FILE PATH         number of instances at a path\n"
	"  shcl instances [options] FILE PATH     instance values at a path, one per line\n"
	"  shcl children [options] FILE [PATH]    child field names under a path, one per\n"
	"                                         line (the top level when PATH is left\n"
	"                                         out)\n"
	"  shcl paths [options] FILE              every field path in the document, one\n"
	"                                         per line\n"
	"  shcl help | version                    this help, or the version (also\n"
	"                                         -h/--help, -v/-V/--version)\n"
	"  shcl about | donate                    what shcl is, or how to support it\n"
	"                                         (also --about, --donate)\n"
	"\n"
	"set edits FILE, the base document. Values go in as repeatable --set PATH=VALUE\n"
	"(data) or --set-literal PATH=TEXT (value syntax, so arrays work) options, which\n"
	"persist with --write; given either, no ops are read from stdin. Raw blocks,\n"
	"set-only-if-absent and removal go in as a write-ops script on stdin, one op per\n"
	"line, tab-separated. FILE '-' follows stdin: the document when an option holds\n"
	"the edits, an empty base when the ops script has stdin instead. With --write,\n"
	"a FILE that does not exist yet is created. PATH ends at the first '=' outside\n"
	"quotes and brackets, so a selector may hold one. Ops:\n"
	"  int|float|bool|string|datetime<TAB>PATH<TAB>VALUE       set a scalar\n"
	"  <type>-array<TAB>PATH<TAB>V1<TAB>V2...                  set an inline array\n"
	"  <type>[-array]-default<TAB>...                          set only if absent\n"
	"  literal[-default]<TAB>PATH<TAB>TEXT                     set from value syntax\n"
	"  raw[-default]<TAB>PATH<TAB>INFO<TAB>CONTENT             set a raw block\n"
	"  empty<TAB>PATH   comment<TAB>PATH<TAB>TEXT   remove<TAB>PATH\n"
	"string/raw values decode \\n \\t \\\\; a line starting with # is a script comment.\n"
	"\n"
	"Types (get only; default --string):\n"
	"  --int --float --bool --datetime --string --raw --rawinfo\n"
	"  --array                                read the value as an array of the type\n"
	"  --rawinfo reads a raw block's info-string (the fence tag), not its content\n"
	"\n"
	"Options (the subcommands each belongs to are in parentheses):\n"
	"  --default=VALUE                        (get) value to print when the read is\n"
	"                                         not Good (implies --on-bad=default; for\n"
	"                                         arrays, substituted per bad slot)\n"
	"  --on-bad=error|default|flag            (get) error: fail loudly; default:\n"
	"                                         print the default; flag: print the\n"
	"                                         value anyway and report via exit code\n"
	"                                         (the default)\n"
	"  --slots                                (get) prefix each line with its slot\n"
	"                                         status and a tab (per element, or per\n"
	"                                         wildcard slot)\n"
	"  --no-banner                            (init) leave out the footer naming the\n"
	"                                         format and pointing at its spec\n"
	"  --lossy                                (fmt/set) with --write, rewrite even\n"
	"                                         when the load dropped lines this write\n"
	"                                         would delete; without it the write\n"
	"                                         refuses and nothing is changed\n"
	"  --strictness=loose|standard|strict     (all but init) or 1|2|3 (default\n"
	"                                         standard)\n"
	"  --schema=SCHEMA                        (check/init) validate FILE against a\n"
	"                                         schema; adds V### diagnostics\n"
	"  --layer=FILE                           (all but check/init) merge a\n"
	"                                         lower-priority layer under FILE;\n"
	"                                         repeatable, earlier = lower priority\n"
	"  --set=PATH=VALUE                       (all but check/init) override one path\n"
	"                                         as the top layer, after all files;\n"
	"                                         repeatable. On 'set' it is an edit to\n"
	"                                         the document itself, so it persists\n"
	"                                         with --write. VALUE goes in as data:\n"
	"                                         its type still follows the text (8 is\n"
	"                                         an int), but a comma or quote in it is\n"
	"                                         content, not syntax\n"
	"  --set-literal=PATH=TEXT                (same subcommands) as --set, except\n"
	"                                         TEXT goes in as value\n"
	"                                         syntax the way a file spells it, so\n"
	"                                         'ports=80, 443' writes a two-element\n"
	"                                         array. An unquoted # ends the value;\n"
	"                                         text spanning lines is rejected\n"
	"  --set-default=PATH=VALUE               (same) as --set, but only when nothing\n"
	"  --set-literal-default=PATH=TEXT        is at the path yet - the write-out-\n"
	"                                         defaults half of the writer\n"
	"  --remove=PATH                          (same) delete what is at the path,\n"
	"                                         with its subtree. Removing nothing is\n"
	"                                         not an error\n"
	"The five above share one ordered list, so two of them touching the same path\n"
	"resolve in the order given. Raw blocks still go in through the ops script.\n"
	"\n"
	"Value options accept either spelling: --default=VALUE or --default VALUE. In\n"
	"the space form the next argument is taken as the value whatever it looks like,\n"
	"so --default --int reads --int as the default. Use -- to end the options when a\n"
	"FILE or PATH begins with a dash.\n"
	"An option a subcommand does not use is a usage error, not ignored. Also\n"
	"refused: --write with --layer; --write with --set outside 'set'; --lossy\n"
	"without --write; --layer=- on 'set'; --array with --raw or --rawinfo; '-'\n"
	"named more than once across FILE, --layer and --schema.\n"
	"Every subcommand that loads a document prints the load's diagnostics to stderr,\n"
	"once per run. An in-place write also refuses when the load dropped content the\n"
	"rewrite would delete (--lossy overrides).\n"
	"FILE may be '-' for stdin. With --layer, FILE is the highest file layer and\n"
	"each --layer is merged under it in order; --set applies last. 'fmt' with\n"
	"layers prints the merged canonical document.\n"
	"\n"
	"Exit codes: 0 good, 1 usage error, 2 empty, 3 not found, 4 bad type,\n"
	"5 multiple instances, 6 check failed, strict load failed, or init's schema\n"
	"has faults, 7 in-place write refused (--lossy overrides), 8 a file or stream\n"
	"could not be read or written.\n";

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

// One edit from the --set family: the path, its length, the value, and which
// spelling produced it. The five spellings share one ordered list, so two of
// them touching the same path resolve in the order given.
typedef struct { const char *path; size_t plen; const char *value; const char *opt; } SetOpt;

typedef struct {
	const char *kind;         // int|float|bool|datetime|string|raw
	int array;
	int slots;
	const char *deflt;        // NULL if unset
	const char *on_bad;       // error|default|flag
	// What an explicit --on-bad asked for, whatever the order. --default sets
	// on_bad too, so without this the two options silently overwrote each other
	// and which one survived depended on which came last.
	const char *on_bad_arg;   // NULL if --on-bad was not given
	shcl_strictness strictness;
	int write;
	int lossy;
	int no_banner;
	const char *schema;       // NULL if unset
	const char **layers; int nlayers; // lower-priority layers, in listed order (unbounded)
	SetOpt *sets; int nsets;          // final override layer, in the order given (unbounded)
	const char **args; int nargs;     // positional: FILE [PATH]
	const char *seen[16]; int nseen;  // distinct canonical option names, for per-command validation
} Opts;

// A file or stream that could not be read or written. Its own code since a
// script's remedy - fix the path, the permissions, the disk - has nothing to do
// with the remedy for a usage error, which keeps 1.
#define EXIT_IO 8

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

// realloc that never returns NULL: on OOM, free the old block and exit 70 (was a
// silent segfault on unchecked realloc).
static void *xrealloc(void *p, size_t n) {
	void *q = realloc(p, n);
	if (!q) { free(p); fprintf(stderr, "shcl: out of memory\n"); exit(70); }
	return q;
}

// The library reports an allocation failure by handing back NULL rather than
// ending the process, which is right for an embedder whose process is not the
// library's to end. This one owns its process and has nowhere to carry on to,
// so it takes the same exit as above.
static void *xdoc(void *p) {
	if (!p) { fprintf(stderr, "shcl: out of memory\n"); exit(70); }
	return p;
}

// Reads FILE (or stdin for "-") fully. Returns malloc'd buffer + len, or NULL on
// error (message printed to stderr). Rejects invalid UTF-8 like the reference.
// A narrow fopen reads the path in the active code page. The argv here is
// UTF-8 (see utf8_argv), so the file opens go through the library's wide
// open, which is the only way a name outside the code page reaches its file.
static FILE *open_rb(const char *file) {
#ifdef _WIN32
	return shcl_fopen_rb(file);
#else
	return fopen(file, "rb");
#endif
}

// Nothing at the path at all, as opposed to something there that will not open.
// fopen rather than stat/access so the check needs no platform header: a
// directory opens here and a protected file sets EACCES, so both stay errors.
static int path_absent(const char *file) {
	FILE *p = open_rb(file);
	if (p) { fclose(p); return 0; }
	return errno == ENOENT;
}

// The message for reading a directory is the platform's, and windows spells it
// four different ways depending on the binding. Say it here.
static int is_a_directory(const char *file) {
#ifdef _WIN32
	wchar_t *w = shcl_widen(file);
	if (!w) return 0;
	DWORD a = GetFileAttributesW(w);
	free(w);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st;
	return stat(file, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static char *read_input(const char *file, size_t *len) {
	char *buf = NULL; size_t cap = 0, n = 0;
	int is_stdin = strcmp(file, "-") == 0;
	const char *who = is_stdin ? "stdin" : file;
	if (!is_stdin && is_a_directory(file)) { fprintf(stderr, "%s: Is a directory\n", who); return NULL; }
	FILE *f = is_stdin ? stdin : open_rb(file);
	if (!f) { fprintf(stderr, "%s: %s\n", who, strerror(errno)); return NULL; }
	char chunk[65536]; size_t r;
	errno = 0;
	while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
		if (n + r > cap) { cap = (n + r) * 2; buf = (char *)xrealloc(buf, cap ? cap : 1); }
		memcpy(buf + n, chunk, r); n += r;
	}
	int ferr = ferror(f);
	// A stdin that is not attached at all reads as an empty document, the way
	// every binding treats it. POSIX says EBADF; windows reports a handle a
	// shell closed as an invalid handle or an invalid function, which the C
	// runtime hands back as EINVAL.
	if (ferr && is_stdin && (errno == EBADF || errno == EINVAL)) ferr = 0;
	if (f != stdin) fclose(f);
	if (ferr) { fprintf(stderr, "%s: %s\n", who, strerror(errno)); free(buf); return NULL; }
	if (!buf) { buf = (char *)xrealloc(NULL, 1); }
	if (!utf8_valid(buf, n)) { fprintf(stderr, "%s: stream did not contain valid UTF-8\n", who); free(buf); return NULL; }
	*len = n; return buf;
}

// The per-binding wording behind a setter's bare 0.
static const char *describe_refusal(shcl_doc *d, const char *path, size_t plen) {
	switch (shcl_write_reason_(d, path, plen)) {
	// The path itself is fine, so the value text must be what failed (a
	// literal that does not parse as one value).
	case SHCL_W_WRITABLE: return "the value text is not one value";
	case SHCL_W_BAD_PATH: return "not a usable path";
	case SHCL_W_VALUE_IN_PATH: return "a path with a value part cannot be written";
	case SHCL_W_WILDCARD: return "a wildcard path cannot be written";
	case SHCL_W_NO_SUCH_INDEX: return "no instance at that index";
	case SHCL_W_TOO_DEEP: return "deeper than the nesting cap";
	}
	return "not a usable path";
}

// One diagnostic line in the shape every command uses: `line N: Severity:
// CODE message` (`schema line` for the V090-V093 schema-fault codes).
static void say_diag_from(const char *file, size_t line, shcl_severity sev, const char *code, shcl_str msg);
static void say_diag(size_t line, shcl_severity sev, const char *code, shcl_str msg) {
	say_diag_from("", line, sev, code, msg);
}

// The same, labelled with the file the diagnostic came from. Under --layer
// several files are loaded and their line numbers share one space on the
// screen, so two layers with a bad line 2 printed the same thing twice with
// nothing to tell them apart.
static void say_diag_from(const char *file, size_t line, shcl_severity sev, const char *code, shcl_str msg) {
	/* V090-V095 carry a schema line; V096 and V097 are about generation as a
	   whole and carry line 0, so "schema line 0" named a line space they are not
	   in. V099 stands for a schema that did not load and is line 0 too. */
	const char *space = (!strncmp(code, "V09", 3) && strcmp(code, "V096") && strcmp(code, "V097")
		&& strcmp(code, "V099")) ? "schema line" : "line";
	if (*file) fprintf(stderr, "%s ", file);
	fprintf(stderr, "%s %zu: %s: %s ", space, line, sev == SHCL_SEV_ERROR ? "Error" : "Hint", code);
	fwrite(msg.p, 1, msg.n, stderr); fputc('\n', stderr);
}
// The load's diagnostics, one line each.
static void say_diagnostics_from(const char *file, const shcl_doc *d) {
	size_t n = shcl_diag_count(d);
	for (size_t i = 0; i < n; i++)
		say_diag_from(file, shcl_diag_line(d, i), shcl_diag_severity(d, i), shcl_diag_code(d, i), shcl_diag_message(d, i));
}

// Prints diagnostics to stderr and returns 6 on strict load failure, else 0.
static int strict_gate_from(const char *file, const shcl_doc *d);
static int strict_gate(const shcl_doc *d) { return strict_gate_from("", d); }

// The same, labelled with the file the document came from, so a strict failure
// in one layer of a fold says which layer.
static int strict_gate_from(const char *file, const shcl_doc *d) {
	if (!shcl_strict_failed(d)) return 0;
	say_diagnostics_from(file, d);
	size_t n = shcl_diag_count(d), nerr = 0;
	for (size_t i = 0; i < n; i++) if (shcl_diag_severity(d, i) == SHCL_SEV_ERROR) nerr++;
	fprintf(stderr, "strict load failed: %zu error diagnostic(s)\n", nerr);
	return 6;
}

// Holds a merged doc and the input buffers its nodes still reference (the base
// layer's node strings are not dup'd off its text). The over-layers are kept
// too: a merge does not carry diagnostics over, so their docs are the only
// place the layers' own diagnostics live. Free everything with layered_free.
typedef struct { shcl_doc *doc; shcl_doc **overs; int novers; char **texts; int ntexts;
	const char **names; int nnames; } LayeredDoc;

static void layered_push_text(LayeredDoc *L, char *t) {
	L->texts = (char **)xrealloc(L->texts, ((size_t)L->ntexts + 1) * sizeof *L->texts);
	L->texts[L->ntexts++] = t;
}

// Merge dd under/over the doc built so far, keeping it alive for its diagnostics.
static void layered_push_doc(LayeredDoc *L, shcl_doc *dd) {
	if (!L->doc) { L->doc = dd; return; }
	shcl_merge(L->doc, dd);
	L->overs = (shcl_doc **)xrealloc(L->overs, ((size_t)L->novers + 1) * sizeof *L->overs);
	L->overs[L->novers++] = dd;
}

static void layered_free(LayeredDoc *L) {
	free((void *)L->names); L->names = NULL; L->nnames = 0;
	if (L->doc) shcl_free(L->doc);
	for (int i = 0; i < L->novers; i++) shcl_free(L->overs[i]);
	free(L->overs);
	for (int i = 0; i < L->ntexts; i++) free(L->texts[i]);
	free(L->texts);
	L->doc = NULL; L->overs = NULL; L->novers = 0; L->texts = NULL; L->ntexts = 0;
}

// Every layer's diagnostics, lowest first. Reading them off the merged doc
// alone would drop the ones for FILE itself, which is the one the caller named.
static void say_layered_diagnostics(const LayeredDoc *L) {
	// Each labelled with its own file when there is more than one: the line
	// numbers share a space on the screen otherwise.
	int lbl = L->nnames > 1;
	say_diagnostics_from(lbl ? L->names[0] : "", L->doc);
	for (int i = 0; i < L->novers; i++)
		say_diagnostics_from(lbl && i + 1 < L->nnames ? L->names[i + 1] : "", L->overs[i]);
}

// Load `file` with o's lower-priority --layer files underneath it and its --set
// overrides on top - the layered-load fold. Every layer parses at the requested
// strictness; a strict-load failure on any aborts (exit 6). Returns 0 and fills
// *out on success, else an exit code (nothing to free on failure).
// PATH=VALUE at the first `=` outside quotes and brackets, so a selector
// holding one (`x[a=b].c=1`) still addresses its instance. Returns 0 when
// there is no such `=`.
static int split_set(const char *arg, size_t *plen, const char **val) {
	char in_quote = 0; size_t depth = 0;
	for (size_t i = 0; arg[i]; i++) {
		char b = arg[i];
		if (b == '\\') { if (arg[i + 1]) i++; continue; }
		if (in_quote) { if (b == in_quote) in_quote = 0; continue; }
		if (b == '"' || b == '\'') in_quote = b;
		else if (b == '[') depth++;
		else if (b == ']') { if (depth) depth--; }
		else if (b == '=' && depth == 0) { *plen = i; *val = arg + i + 1; return 1; }
	}
	return 0;
}

// Apply one --set/--set-literal override. Both spellings share a list so they
// apply in the order given, which decides the winner when two target one path.
static int set_apply(shcl_doc *d, const SetOpt *s) {
	size_t vlen = strlen(s->value);
	int ok;
	if (!strcmp(s->opt, "--remove")) {
		// Removing nothing is not a failure, the same as the ops script's
		// `remove`: the point of the option is the path's absence after.
		shcl_remove(d, s->path, s->plen);
		return 1;
	}
	if (!strcmp(s->opt, "--set-literal")) ok = shcl_set_literal(d, s->path, s->plen, s->value, vlen);
	else if (!strcmp(s->opt, "--set-default")) ok = shcl_set_string_default(d, s->path, s->plen, s->value, vlen);
	else if (!strcmp(s->opt, "--set-literal-default")) ok = shcl_set_literal_default(d, s->path, s->plen, s->value, vlen);
	else ok = shcl_set_string(d, s->path, s->plen, s->value, vlen);
	if (!ok) fprintf(stderr, "%s: cannot write %.*s: %s\n", s->opt, (int)s->plen, s->path, describe_refusal(d, s->path, s->plen));
	return ok;
}

static int load_layered(Opts *o, const char *file, LayeredDoc *out) {
	out->doc = NULL; out->overs = NULL; out->novers = 0; out->texts = NULL; out->ntexts = 0;
	out->names = (const char **)xrealloc(NULL, (size_t)(o->nlayers + 1) * sizeof *out->names);
	out->nnames = 0;
	// Lowest -> highest file layer: the --layer files in order, then FILE.
	for (int i = 0; i <= o->nlayers; i++) {
		const char *fname = i < o->nlayers ? o->layers[i] : file;
		out->names[out->nnames++] = fname;
		size_t len; char *t = read_input(fname, &len);
		if (!t) { layered_free(out); return EXIT_IO; }
		layered_push_text(out, t);
		shcl_doc *dd = xdoc(shcl_parse_with(t, len, o->strictness));
		int g = strict_gate_from(o->nlayers ? fname : "", dd);
		if (g) { shcl_free(dd); layered_free(out); return g; }
		layered_push_doc(out, dd);
	}
	// The load's diagnostics belong to the load, so they go out before any edit
	// runs: a refused --set used to return with nothing said about them.
	say_layered_diagnostics(out);
	for (int i = 0; i < o->nsets; i++) {
		if (!set_apply(out->doc, &o->sets[i])) { layered_free(out); return 1; }
	}
	return 0;
}

// The source text, quoted for a message: one line whatever it holds, with
// the same escapes in every binding.
static char *quoted(const char *p, size_t n) {
	size_t cap = n * 8 + 3;
	char *out = (char *)xrealloc(NULL, cap);
	size_t w = 0;
	out[w++] = '"';
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)p[i];
		if (c == '"') { out[w++] = '\\'; out[w++] = '"'; }
		else if (c == '\\') { out[w++] = '\\'; out[w++] = '\\'; }
		else if (c == '\n') { out[w++] = '\\'; out[w++] = 'n'; }
		else if (c == '\r') { out[w++] = '\\'; out[w++] = 'r'; }
		else if (c == '\t') { out[w++] = '\\'; out[w++] = 't'; }
		else if (c < 0x20 || c == 0x7f) w += (size_t)snprintf(out + w, cap - w, "\\u{%x}", c);
		else out[w++] = (char)c;
	}
	out[w++] = '"';
	out[w] = '\0';
	return out;
}

static int do_get(Opts *o) {
	if (o->nargs != 2) { fprintf(stderr, "usage: shcl get [type] [options] FILE PATH (see --help)\n"); return 1; }
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
	// Why the read failed is worth saying even when the exit code already
	// carries it: at the default mode the user otherwise gets an empty line, a
	// nonzero code, and nothing to go on. Stdout is untouched - this only ever
	// goes to stderr. Two silences are deliberate: default mode, because a
	// caller who supplied a fallback has already said the miss is expected, and
	// Empty outside error mode, because an empty value is a legitimate answer
	// here rather than a failure - the same reason shcl_status_ok counts it so.
	if (status != SHCL_GOOD && strcmp(o->on_bad, "default") != 0
	    && (status != SHCL_EMPTY || !strcmp(o->on_bad, "error"))) {
		char tbuf[32];
		snprintf(tbuf, sizeof tbuf, o->array ? "%s array" : "%s", o->kind);
		if (status == SHCL_BAD_TYPE) {
			// The reason quotes the text only when the path resolved to a node;
			// read_string answers Good or Empty exactly then.
			shcl_read_str rs = shcl_read_string(d, path, plen);
			if (rs.status == SHCL_GOOD || rs.status == SHCL_EMPTY) {
				char *q = quoted(rs.value.p, rs.value.n);
				fprintf(stderr, "cannot read %s as %s: value %s is not a valid %s (in %s)\n", path, tbuf, q, tbuf, file);
				free(q);
			} else
				fprintf(stderr, "cannot read %s as %s: value is not a valid %s (in %s)\n", path, tbuf, tbuf, file);
		} else if (status == SHCL_NOT_FOUND) {
			fprintf(stderr, "cannot read %s as %s: no value at that path (in %s)\n", path, tbuf, file);
		} else if (status == SHCL_EMPTY) {
			fprintf(stderr, "cannot read %s as %s: the value is empty (in %s)\n", path, tbuf, file);
		} else {
			fprintf(stderr, "cannot read %s as %s: the path matches multiple instances (in %s)\n", path, tbuf, file);
		}
	}
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
		// The message already went to stderr above; error mode differs only in
		// printing nothing on stdout.
		rc = shcl_status_code(status);
	} else {
		for (size_t i = 0; i < nlines; i++) EMITLINE(i, LINEPTR(i), lines[i].n);
		rc = shcl_status_code(status);
	}
	free(lines); layered_free(&L); return rc;
}

// The in-place half of fmt/set. Overwriting the source is the one place a
// recovered load turns destructive, so the diagnostics go out even though the
// command succeeded, and the save runs through the library's own gate rather
// than a second copy of the rule - the CLI and a consumer program cannot then
// disagree about which rewrites are safe.
// Whether the directory holding `file` can take a new entry - the probe
// behind the temp-create wording on a failed save.
// Can the target's directory take a temp file? Asked by creating one, because
// that is what the library's write does: _waccess reports every existing
// directory writable on windows, so an ACL-protected one got the bare errno
// where the other three name the phase. Only ever called on the failure path,
// and the probe is removed immediately.
static int dir_takes_a_temp(const char *file) {
	const char *slash = strrchr(file, '/');
#ifdef _WIN32
	const char *bs = strrchr(file, '\\');
	if (bs && (!slash || bs > slash)) slash = bs;
#endif
	char probe[4096];
	if (!slash) snprintf(probe, sizeof probe, ".shcl-probe%ld", (long)getpid());
	else if (slash == file) snprintf(probe, sizeof probe, "/.shcl-probe%ld", (long)getpid());
	else snprintf(probe, sizeof probe, "%.*s/.shcl-probe%ld", (int)(slash - file), file, (long)getpid());
#ifdef _WIN32
	wchar_t *w = shcl_widen(probe);
	int fd = w ? _wopen(w, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE) : -1;
	if (fd >= 0) { _close(fd); _wunlink(w); }
	free(w);
	return fd >= 0;
#else
	int fd = open(probe, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd >= 0) { close(fd); unlink(probe); }
	return fd >= 0;
#endif
}

static int write_back(shcl_doc *d, const char *file, Opts *o) {
	shcl_save_result r = o->lossy ? shcl_save_file_lossy(d, file) : shcl_save_file(d, file);
	if (r == SHCL_SAVE_OK) return 0;
	// The rule stays in the library; only the wording is the CLI's, because the
	// override a user has here is a flag, not a function.
	if (r == SHCL_SAVE_REFUSED) {
		fprintf(stderr, "%s: refusing to rewrite: the load dropped %zu line(s)/value(s) this write would delete (--lossy overrides)\n", file, shcl_lost_count(d));
		return 7;
	}
	// The library reports a failed save through errno alone; the phase worth
	// naming is the temp-file create, which is what failed when the target's
	// directory cannot take one.
	int e = errno;
	if (!dir_takes_a_temp(file)) fprintf(stderr, "%s: cannot create temporary file: %s\n", file, strerror(e));
	else fprintf(stderr, "%s: %s\n", file, strerror(e));
	return EXIT_IO;
}

static int do_fmt(Opts *o) {
	if (o->nargs != 1) { fprintf(stderr, "usage: shcl fmt [--write|-w] [options] FILE (see --help)\n"); return 1; }
	const char *file = o->args[0];
	if (o->write && strcmp(file, "-") == 0) {
		fprintf(stderr, "fmt --write cannot rewrite stdin; drop --write to print, or pass a FILE\n");
		return 1;
	}
	LayeredDoc L; int gate = load_layered(o, file, &L);
	if (gate) return gate;
	int rc;
	if (o->write) {
		rc = write_back(L.doc, file, o);
	} else {
		shcl_str c = shcl_to_canonical(L.doc);
		fwrite(c.p, 1, c.n, stdout);
		rc = 0;
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
	// The language's own float reader takes inf and nan, and overflow lands on
	// them too; the document's reader does not, so they are bad values here,
	// the way a bad datetime is.
	char *b = (char *)xrealloc(NULL, n + 1); memcpy(b, p, n); b[n] = 0;
	*out = strtod(b, NULL);
	free(b);
	return isfinite(*out) ? 1 : 0;
}
// Exactly `true` or `false`; anything else is a bad value, like a bad int.
static int g_bool(const char *p, size_t n, int *out) {
	if (n == 4 && memcmp(p, "true", 4) == 0) { *out = 1; return 1; }
	if (n == 5 && memcmp(p, "false", 5) == 0) { *out = 0; return 1; }
	return 0;
}

// The `op line N:` prefix every ops-script error carries.
static void op_err(size_t lineno, const char *fmt, ...) {
	va_list ap;
	fprintf(stderr, "op line %zu: ", lineno);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

// Apply one write-ops line. A "-default" suffix means "only if absent": values
// are gated FIRST (a malformed value fails even when the path already exists,
// matching the reference's argument-evaluation order), then the existence probe
// decides whether the base op runs. Returns 0 or 1 (error).
static int apply_op(shcl_doc *d, const char *line, size_t linelen, size_t lineno) {
	size_t nf = 1;
	for (size_t i = 0; i < linelen; i++) if (line[i] == '\t') nf++;
	const char **fp = (const char **)xrealloc(NULL, nf * sizeof *fp);
	size_t *fn = (size_t *)xrealloc(NULL, nf * sizeof *fn);
	{ size_t k = 0, start = 0; for (size_t i = 0; i <= linelen; i++) if (i == linelen || line[i] == '\t') { fp[k] = line + start; fn[k] = i - start; k++; start = i + 1; } }
	// Every op but the array forms takes a fixed number of tab-separated
	// fields. Extra ones used to be dropped, so a `raw` whose content held a
	// literal tab lost everything after it and still reported success; the
	// escape for a tab inside a value is `\t`.
	{
		int bad = 0;
		static const struct { const char *op; size_t want; } counts[] = {
			{ "empty", 2 }, { "remove", 2 }, { "raw", 4 }, { "raw-default", 4 },
			{ "int", 3 }, { "float", 3 }, { "bool", 3 }, { "string", 3 },
			{ "datetime", 3 }, { "literal", 3 }, { "comment", 3 },
			{ "int-default", 3 }, { "float-default", 3 }, { "bool-default", 3 },
			{ "string-default", 3 }, { "datetime-default", 3 }, { "literal-default", 3 },
		};
		for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
			if (strlen(counts[i].op) != fn[0] || memcmp(counts[i].op, fp[0], fn[0]) != 0) continue;
			if (nf > counts[i].want) {
				fprintf(stderr, "op line %zu: %s takes %zu tab-separated field(s), got %zu\n",
					lineno, counts[i].op, counts[i].want, nf);
				bad = 1;
			}
			break;
		}
		if (bad) { free(fp); free(fn); return 1; }
	}
	const char *path = nf > 1 ? fp[1] : ""; size_t plen = nf > 1 ? fn[1] : 0;
	const char *v = nf > 2 ? fp[2] : ""; size_t vn = nf > 2 ? fn[2] : 0;
	int rc = 0, wrote = 1;
	size_t opn_full = fn[0]; // the op as written, for the unknown-op message
	int only_absent = 0;
	if (fn[0] >= 8 && memcmp(fp[0] + fn[0] - 8, "-default", 8) == 0) {
		only_absent = 1;
		fn[0] -= 8; // strip suffix; the base op handles the actual write
	}
	#define OP(s) (fn[0] == strlen(s) && memcmp(fp[0], s, fn[0]) == 0)
	// A present path still answers what a write there would, like the library's
	// default forms: a wildcard is refused whether or not its slots resolve.
	#define PRESENT (only_absent && shcl_exists(d, path, plen) && ((wrote = shcl_write_reason_(d, path, plen) == SHCL_W_WRITABLE), 1))
	size_t an = nf > 2 ? nf - 2 : 0; // array element count (fields from index 2)
	if (OP("int")) { int64_t x; if (!g_i64(v, vn, &x)) { op_err(lineno, "bad int: %.*s", (int)vn, v); rc = 1; } else if (!PRESENT) wrote = shcl_set_int(d, path, plen, x); }
	else if (OP("float")) { double x; if (!g_f64(v, vn, &x)) { op_err(lineno, "bad float: %.*s", (int)vn, v); rc = 1; } else if (!PRESENT) wrote = shcl_set_float(d, path, plen, x); }
	else if (OP("bool")) { int x; if (!g_bool(v, vn, &x)) { op_err(lineno, "bad bool: %.*s", (int)vn, v); rc = 1; } else if (!PRESENT) wrote = shcl_set_bool(d, path, plen, x); }
	else if (OP("string")) { if (!PRESENT) { char *b = (char *)xrealloc(NULL, vn ? vn : 1); size_t m = unescape_ops(v, vn, b); wrote = shcl_set_string(d, path, plen, b, m); free(b); } }
	else if (OP("datetime")) { shcl_datetime dt; ShclStr sv; sv.p = v; sv.n = vn; if (!parse_datetime(&d->arena, sv, &dt)) { op_err(lineno, "bad datetime: %.*s", (int)vn, v); rc = 1; } else if (!PRESENT) wrote = shcl_set_datetime(d, path, plen, &dt); }
	else if (OP("literal")) { if (!PRESENT) wrote = shcl_set_literal(d, path, plen, v, vn); }
	else if (OP("int-array")) { int64_t *a = (int64_t *)xrealloc(NULL, (an ? an : 1) * sizeof *a); memset(a, 0, (an ? an : 1) * sizeof *a); for (size_t i = 0; i < an && !rc; i++) if (!g_i64(fp[2 + i], fn[2 + i], &a[i])) { op_err(lineno, "bad int: %.*s", (int)fn[2 + i], fp[2 + i]); rc = 1; } if (!rc && !PRESENT) wrote = shcl_set_int_array(d, path, plen, a, an); free(a); }
	else if (OP("float-array")) { double *a = (double *)xrealloc(NULL, (an ? an : 1) * sizeof *a); memset(a, 0, (an ? an : 1) * sizeof *a); for (size_t i = 0; i < an && !rc; i++) if (!g_f64(fp[2 + i], fn[2 + i], &a[i])) { op_err(lineno, "bad float: %.*s", (int)fn[2 + i], fp[2 + i]); rc = 1; } if (!rc && !PRESENT) wrote = shcl_set_float_array(d, path, plen, a, an); free(a); }
	else if (OP("bool-array")) { int *a = (int *)xrealloc(NULL, (an ? an : 1) * sizeof *a); memset(a, 0, (an ? an : 1) * sizeof *a); for (size_t i = 0; i < an && !rc; i++) if (!g_bool(fp[2 + i], fn[2 + i], &a[i])) { op_err(lineno, "bad bool: %.*s", (int)fn[2 + i], fp[2 + i]); rc = 1; } if (!rc && !PRESENT) wrote = shcl_set_bool_array(d, path, plen, a, an); free(a); }
	else if (OP("string-array")) {
		if (!PRESENT) {
			// Zeroed, not just sized: an empty array writes nothing here and the setter
			// reads nothing, but a compiler that inlines the allocator cannot see that
			// and calls the slots uninitialized. gcc 13 does, 14 and 15 do not.
			char **sv = (char **)xrealloc(NULL, (an ? an : 1) * sizeof *sv); size_t *sl = (size_t *)xrealloc(NULL, (an ? an : 1) * sizeof *sl);
			memset(sv, 0, (an ? an : 1) * sizeof *sv); memset(sl, 0, (an ? an : 1) * sizeof *sl);
			for (size_t i = 0; i < an; i++) { char *b = (char *)xrealloc(NULL, fn[2 + i] ? fn[2 + i] : 1); sl[i] = unescape_ops(fp[2 + i], fn[2 + i], b); sv[i] = b; }
			wrote = shcl_set_string_array(d, path, plen, (const char *const *)sv, sl, an);
			for (size_t i = 0; i < an; i++) free(sv[i]);
			free(sv); free(sl);
		}
	}
	else if (OP("datetime-array")) {
		shcl_datetime *a = (shcl_datetime *)xrealloc(NULL, (an ? an : 1) * sizeof *a);
		for (size_t i = 0; i < an && !rc; i++) { ShclStr sv; sv.p = fp[2 + i]; sv.n = fn[2 + i]; if (!parse_datetime(&d->arena, sv, &a[i])) { op_err(lineno, "bad datetime: %.*s", (int)fn[2 + i], fp[2 + i]); rc = 1; } }
		if (!rc && !PRESENT) wrote = shcl_set_datetime_array(d, path, plen, a, an);
		free(a);
	}
	else if (OP("raw")) { if (!PRESENT) { const char *cont = nf > 3 ? fp[3] : ""; size_t contn = nf > 3 ? fn[3] : 0; char *b = (char *)xrealloc(NULL, contn ? contn : 1); size_t m = unescape_ops(cont, contn, b); wrote = shcl_set_raw(d, path, plen, b, m, v, vn); free(b); } }
	else if (OP("empty") && !only_absent) wrote = shcl_set_empty(d, path, plen);
	else if (OP("comment") && !only_absent) wrote = shcl_set_comment(d, path, plen, v, vn);
	else if (OP("remove") && !only_absent) shcl_remove(d, path, plen);
	else { op_err(lineno, "unknown op: %.*s", (int)opn_full, fp[0]); rc = 1; }
	if (rc == 0 && !wrote) { op_err(lineno, "cannot write %.*s: %s", (int)plen, path, describe_refusal(d, path, plen)); rc = 1; }
	#undef PRESENT
	#undef OP
	free(fp); free(fn);
	return rc;
}

static int do_set(Opts *o) {
	if (o->nargs != 1) { fprintf(stderr, "usage: shcl set [--write|-w] [options] FILE (see --help)\n"); return 1; }
	const char *file = o->args[0];
	if (o->write && strcmp(file, "-") == 0) {
		fprintf(stderr, "set --write cannot rewrite stdin; drop --write to print, or pass a FILE\n");
		return 1;
	}
	// Base doc: with the edits given as options no ops script is read, so a '-'
	// file is the document on stdin the way it is everywhere else; only when
	// stdin is the ops script does '-' mean an empty base. Reading neither threw
	// a piped document away at exit 0.
	// Any --layer files sit under it and --set overrides sit on top, before ops.
	// The base layer's node strings are not dup'd off its text, so keep all
	// buffers.
	LayeredDoc L; L.doc = NULL; L.overs = NULL; L.novers = 0; L.texts = NULL; L.ntexts = 0;
	L.names = (const char **)xrealloc(NULL, (size_t)(o->nlayers + 1) * sizeof *L.names);
	L.nnames = 0;
	for (int i = 0; i < o->nlayers; i++) {
		L.names[L.nnames++] = o->layers[i];
		size_t llen; char *lt = read_input(o->layers[i], &llen);
		if (!lt) { layered_free(&L); return EXIT_IO; }
		layered_push_text(&L, lt);
		shcl_doc *dd = xdoc(shcl_parse_with(lt, llen, o->strictness));
		int g = strict_gate(dd);
		if (g) { shcl_free(dd); layered_free(&L); return g; }
		layered_push_doc(&L, dd);
	}
	// --write names the file this command produces, so a FILE that is not there
	// yet is a create and the edits land in a new document. Only under --write,
	// and only when nothing is at the path at all: without --write there is
	// nothing to create, and a file that exists but cannot be read is still an
	// error rather than something to quietly write over. Checked before the
	// read, not after: read_input prints its own diagnostic, and a create has
	// nothing to report.
	int creating = o->write && strcmp(file, "-") != 0 && path_absent(file);
	char *text; size_t len;
	if (creating || (!strcmp(file, "-") && o->nsets == 0)) { text = (char *)xrealloc(NULL, 1); len = 0; }
	else { text = read_input(file, &len); if (!text) { layered_free(&L); return EXIT_IO; } }
	layered_push_text(&L, text);
	L.names[L.nnames++] = file;
	{
		shcl_doc *dd = xdoc(shcl_parse_with(text, len, o->strictness));
		int gate = strict_gate(dd);
		if (gate) { shcl_free(dd); layered_free(&L); return gate; }
		layered_push_doc(&L, dd);
	}
	shcl_doc *d = L.doc;
	// The load's diagnostics belong to the load, so they go out before any edit
	// runs: a refused --set or a failing op used to return with nothing said.
	say_layered_diagnostics(&L);
	for (int i = 0; i < o->nsets; i++) {
		if (!set_apply(d, &o->sets[i])) { layered_free(&L); return 1; }
	}
	// --set carries the edits, so stdin is left alone: reading it here would
	// block on the console for anyone who passed edits as options.
	size_t opslen = 0; char *ops = NULL;
	if (o->nsets == 0) {
		// Say so before blocking. With nothing on stdin this used to sit there
		// silently, which reads as a hang rather than as a prompt; the note is
		// unconditional so a pipeline and a terminal behave identically.
		fprintf(stderr, "shcl: reading write-ops from stdin (one op per line, tab-separated; end with EOF)\n");
		ops = read_all_fp(stdin, &opslen);
		// The ops script gets the same UTF-8 gate as any file input (exit 1).
		if (!utf8_valid(ops, opslen)) {
			fprintf(stderr, "stdin: stream did not contain valid UTF-8\n");
			free(ops); layered_free(&L); return EXIT_IO;
		}
	}
	int rc = 0; size_t start = 0, lineno = 0;
	for (size_t i = 0; i <= opslen; i++) {
		if (i == opslen || ops[i] == '\n') {
			size_t end = i;
			if (end > start && ops[end - 1] == '\r') end--; // match Rust lines() CRLF
			size_t n = end - start;
			lineno++;
			if (n > 0 && ops[start] != '#') { if (apply_op(d, ops + start, n, lineno)) { rc = 1; break; } }
			if (i == opslen) break;
			start = i + 1;
		}
	}
	if (rc == 0) {
		if (o->write) rc = write_back(d, file, o);
		else { shcl_str c = shcl_to_canonical(d); fwrite(c.p, 1, c.n, stdout); }
	}
	free(ops); layered_free(&L); return rc;
}

static int do_check(const Opts *o) {
	if (o->nargs != 1) { fprintf(stderr, "usage: shcl check [options] FILE (see --help)\n"); return 1; }
	size_t len; char *text = read_input(o->args[0], &len);
	if (!text) return EXIT_IO;
	shcl_doc *d = xdoc(shcl_parse_with(text, len, o->strictness));
	// --schema: append validation diagnostics under the same contract. The
	// schema itself always loads at Standard (a program artifact); one that
	// does not load cleanly is a single V099 schema fault.
	shcl_validation *val = NULL;
	shcl_doc *sd = NULL;
	char *stext = NULL;
	int v99 = 0;
	if (!shcl_strict_failed(d) && o->schema) {
		size_t slen; stext = read_input(o->schema, &slen);
		if (!stext) { shcl_free(d); free(text); return EXIT_IO; }
		sd = xdoc(shcl_parse(stext, slen));
		size_t sn = shcl_diag_count(sd);
		for (size_t i = 0; i < sn; i++) if (shcl_diag_severity(sd, i) == SHCL_SEV_ERROR) v99 = 1;
		if (v99) {
			for (size_t i = 0; i < sn; i++) {
				const char *sev = shcl_diag_severity(sd, i) == SHCL_SEV_ERROR ? "Error" : "Hint";
				shcl_str m = shcl_diag_message(sd, i);
				fprintf(stderr, "schema line %zu: %s: %s ", shcl_diag_line(sd, i), sev, shcl_diag_code(sd, i));
				fwrite(m.p, 1, m.n, stderr); fputc('\n', stderr);
			}
		} else {
			/* The schema's own load has something to say too: an H001 on a
			   repeated `allowed` is what explains the V092 below it. On stderr
			   with the schema's own line numbers, the way a V099's are -
			   stdout is the code contract. */
			for (size_t i = 0; i < sn; i++) {
				const char *sev = shcl_diag_severity(sd, i) == SHCL_SEV_ERROR ? "Error" : "Hint";
				shcl_str m = shcl_diag_message(sd, i);
				fprintf(stderr, "schema line %zu: %s: %s ", shcl_diag_line(sd, i), sev, shcl_diag_code(sd, i));
				fwrite(m.p, 1, m.n, stderr); fputc('\n', stderr);
			}
			val = xdoc(shcl_validate(d, sd));
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
		say_diag(shcl_diag_line(d, i), shcl_diag_severity(d, i), shcl_diag_code(d, i), shcl_diag_message(d, i));
	}
	if (v99) {
		printf("line 0: Error: V099\n");
		shcl_str v99m; v99m.p = "schema failed to load"; v99m.n = 21;
		say_diag(0, SHCL_SEV_ERROR, "V099", v99m);
		nerr++;
	}
	for (size_t i = 0; i < nval; i++) {
		const char *sev = shcl_validation_severity(val, i) == SHCL_SEV_ERROR ? "Error" : "Hint";
		if (shcl_validation_severity(val, i) == SHCL_SEV_ERROR) nerr++;
		const char *code = shcl_validation_code(val, i);
		printf("line %zu: %s: %s\n", shcl_validation_line(val, i), sev, code);
		// A V090-V093 line number is a SCHEMA line (the code table says so);
		// the prose names the file so the number spaces cannot be confused.
		say_diag(shcl_validation_line(val, i), shcl_validation_severity(val, i), code, shcl_validation_message(val, i));
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

static int do_init(const Opts *o) {
	if (o->nargs > 0) { fprintf(stderr, "init takes no file argument (see --help)\n"); return 1; }
	if (!o->schema) { fprintf(stderr, "init needs --schema=FILE (see --help)\n"); return 1; }
	size_t slen; char *stext = read_input(o->schema, &slen);
	if (!stext) return EXIT_IO;
	// The schema always loads at Standard - a program artifact, not user data.
	shcl_doc *sd = xdoc(shcl_parse(stext, slen));
	int bad = 0; size_t sn = shcl_diag_count(sd);
	for (size_t i = 0; i < sn; i++) if (shcl_diag_severity(sd, i) == SHCL_SEV_ERROR) bad = 1;
	if (bad) {
		for (size_t i = 0; i < sn; i++) {
			const char *sev = shcl_diag_severity(sd, i) == SHCL_SEV_ERROR ? "Error" : "Hint";
			shcl_str m = shcl_diag_message(sd, i);
			fprintf(stderr, "schema line %zu: %s: %s ", shcl_diag_line(sd, i), sev, shcl_diag_code(sd, i));
			fwrite(m.p, 1, m.n, stderr); fputc('\n', stderr);
		}
		fprintf(stderr, "init: schema failed to load\n");
		// A broken schema is a config-semantics failure, not a usage error:
		// same exit as `check --schema` reporting it.
		shcl_free(sd); free(stext); return 6;
	}
	int ok = 0;
	// Generation-only faults are recorded on the schema document as it runs;
	// anything past this mark came from the call below.
	size_t diagMark = shcl_diag_count(sd);
	shcl_str text = shcl_generate(sd, o->no_banner, &ok);
	if (!ok) {
		size_t nd = shcl_diag_count(sd);
		for (size_t i = diagMark; i < nd; i++)
			say_diag(shcl_diag_line(sd, i), shcl_diag_severity(sd, i), shcl_diag_code(sd, i), shcl_diag_message(sd, i));
		fprintf(stderr, "init: schema has faults\n");
		shcl_free(sd); free(stext); return 6;
	}
	fwrite(text.p, 1, text.n, stdout);
	shcl_free(sd); free(stext); return 0;
}

static int do_enum(Opts *o, int want_count) {
	if (o->nargs != 2) {
		const char *name = want_count ? "count" : "instances";
		fprintf(stderr, "usage: shcl %s [options] FILE PATH (see --help)\n", name);
		return 1;
	}
	const char *file = o->args[0], *path = o->args[1]; size_t plen = strlen(path);
	LayeredDoc L; int gate = load_layered(o, file, &L);
	if (gate) return gate;
	shcl_doc *d = L.doc;
	if (want_count) printf("%zu\n", shcl_count(d, path, plen));
	else { shcl_str *vals; size_t n = shcl_instances(d, path, plen, &vals); for (size_t i = 0; i < n; i++) outln(vals[i].p, vals[i].n); }
	layered_free(&L); return 0;
}

// The type option's kind name (--int -> "int"), or NULL when a is no type
// option.
static const char *kind_from_opt(const char *a) {
	static const char *const kinds[] = { "int", "float", "bool", "datetime", "string", "raw", "rawinfo" };
	if (a[0] != '-' || a[1] != '-') return NULL;
	for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
		if (!strcmp(a + 2, kinds[i])) return kinds[i];
	return NULL;
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

static void set_push(Opts *o, const char *path, size_t plen, const char *value, const char *opt) {
	o->sets = (SetOpt *)xrealloc(o->sets, ((size_t)o->nsets + 1) * sizeof *o->sets);
	o->sets[o->nsets].path = path; o->sets[o->nsets].plen = plen;
	o->sets[o->nsets].value = value; o->sets[o->nsets].opt = opt;
	o->nsets++;
}

static void opts_free(Opts *o) {
	free((void *)o->layers); free(o->sets); free((void *)o->args);
	o->layers = o->args = NULL; o->sets = NULL; o->nlayers = o->nsets = o->nargs = 0;
}


// Apply a value-taking option's value. Returns 0 ok, 1 on a bad value.
static int set_value_opt(Opts *o, const char *name, const char *v) {
	if (!strcmp(name, "--default")) { o->deflt = v; o->on_bad = "default"; opt_seen(o, "--default"); }
	else if (!strcmp(name, "--on-bad")) {
		if (g_ci_eq(v, strlen(v), "error")) o->on_bad = "error";
		else if (g_ci_eq(v, strlen(v), "default")) o->on_bad = "default";
		else if (g_ci_eq(v, strlen(v), "flag")) o->on_bad = "flag";
		else { fprintf(stderr, "bad --on-bad value: %s\n", v); return 1; }
		o->on_bad_arg = o->on_bad;
		opt_seen(o, "--on-bad");
	} else if (!strcmp(name, "--strictness")) {
		if (!shcl_strictness_from_arg(v, strlen(v), &o->strictness)) { fprintf(stderr, "bad --strictness value: %s\n", v); return 1; }
		opt_seen(o, "--strictness");
	} else if (!strcmp(name, "--schema")) {
		o->schema = v; opt_seen(o, "--schema");
	} else if (!strcmp(name, "--layer")) {
		opt_push(&o->layers, &o->nlayers, v); opt_seen(o, "--layer");
	} else if (!strcmp(name, "--remove")) {
		if (!*v) { fprintf(stderr, "bad --remove value (want PATH)\n"); return 1; }
		set_push(o, v, strlen(v), "", "--remove"); opt_seen(o, "--remove");
	} else if (!strcmp(name, "--set") || !strcmp(name, "--set-literal")
	           || !strcmp(name, "--set-default") || !strcmp(name, "--set-literal-default")) {
		size_t plen; const char *val;
		if (!split_set(v, &plen, &val) || plen == 0) { fprintf(stderr, "bad %s value (want PATH=VALUE, quotes and brackets balanced): %s\n", name, v); return 1; }
		set_push(o, v, plen, val, name); opt_seen(o, name);
	}
	return 0;
}

static int parse_opts(int argc, char **argv, int from, Opts *o) {
	o->kind = "string"; o->array = 0; o->slots = 0; o->deflt = NULL; o->on_bad = "flag"; o->on_bad_arg = NULL;
	o->strictness = SHCL_STANDARD; o->write = 0; o->lossy = 0; o->no_banner = 0; o->schema = NULL;
	o->layers = o->args = NULL; o->sets = NULL; o->nlayers = o->nsets = o->nargs = 0; o->nseen = 0;
	// Value-taking options accept both --opt=VALUE and the space form --opt VALUE.
	for (int i = from; i < argc; i++) {
		const char *a = argv[i];
		// Everything after `--` is positional, so a file or path may begin
		// with a dash.
		if (!strcmp(a, "--")) {
			for (int k = i + 1; k < argc; k++) opt_push(&o->args, &o->nargs, argv[k]);
			return 0;
		}
		const char *k = kind_from_opt(a);
		if (k) { o->kind = k; opt_seen(o, "--<type>"); }
		else if (!strcmp(a, "--array")) { o->array = 1; opt_seen(o, "--array"); }
		else if (!strcmp(a, "--slots")) { o->slots = 1; opt_seen(o, "--slots"); }
		else if (!strcmp(a, "--write") || !strcmp(a, "-w")) { o->write = 1; opt_seen(o, "--write"); }
		else if (!strcmp(a, "--lossy")) { o->lossy = 1; opt_seen(o, "--lossy"); }
		else if (!strcmp(a, "--no-banner")) { o->no_banner = 1; opt_seen(o, "--no-banner"); }
		else if (!strcmp(a, "--default") || !strcmp(a, "--on-bad") || !strcmp(a, "--strictness") || !strcmp(a, "--schema") || !strcmp(a, "--layer") || !strcmp(a, "--set") || !strcmp(a, "--set-literal") || !strcmp(a, "--set-default") || !strcmp(a, "--set-literal-default") || !strcmp(a, "--remove")) {
			if (i + 1 >= argc) { fprintf(stderr, "missing value for %s (try %s=VALUE)\n", a, a); return 1; }
			if (set_value_opt(o, a, argv[++i])) return 1;
		}
		else if (!strncmp(a, "--default=", 10)) { if (set_value_opt(o, "--default", a + 10)) return 1; }
		else if (!strncmp(a, "--on-bad=", 9)) { if (set_value_opt(o, "--on-bad", a + 9)) return 1; }
		else if (!strncmp(a, "--strictness=", 13)) { if (set_value_opt(o, "--strictness", a + 13)) return 1; }
		else if (!strncmp(a, "--schema=", 9)) { if (set_value_opt(o, "--schema", a + 9)) return 1; }
		else if (!strncmp(a, "--layer=", 8)) { if (set_value_opt(o, "--layer", a + 8)) return 1; }
		else if (!strncmp(a, "--set-literal=", 14)) { if (set_value_opt(o, "--set-literal", a + 14)) return 1; }
		else if (!strncmp(a, "--set-literal-default=", 22)) { if (set_value_opt(o, "--set-literal-default", a + 22)) return 1; }
		else if (!strncmp(a, "--set-default=", 14)) { if (set_value_opt(o, "--set-default", a + 14)) return 1; }
		else if (!strncmp(a, "--remove=", 9)) { if (set_value_opt(o, "--remove", a + 9)) return 1; }
		else if (!strncmp(a, "--set=", 6)) { if (set_value_opt(o, "--set", a + 6)) return 1; }
		else if (a[0] == '-' && a[1] != '\0') { fprintf(stderr, "unknown option: %s\n", a); return 1; }
		else opt_push(&o->args, &o->nargs, a);
	}
	return 0;
}

// Every option must be meaningful for its subcommand; an option that would be
// silently ignored (`set --write` before it existed, `--schema` on `get`) is a
// usage error instead.
// Child field names under a path, one per line, in file order and with
// duplicates kept. PATH may be left out to enumerate the top level. Each name
// comes out in the form a path accepts, so one holding a dot or a quote splices
// back into a path with no further work.
static int do_children(Opts *o) {
	const char *file, *path = "";
	if (o->nargs == 1) file = o->args[0];
	else if (o->nargs == 2) { file = o->args[0]; path = o->args[1]; }
	else { fprintf(stderr, "usage: shcl children [options] FILE [PATH] (see --help)\n"); return 1; }
	LayeredDoc L; int gate = load_layered(o, file, &L);
	if (gate) return gate;
	shcl_str *names = NULL;
	size_t n = shcl_children(L.doc, path, strlen(path), &names);
	for (size_t i = 0; i < n; i++) {
		shcl_str q = shcl_quote_segment(L.doc, names[i].p, names[i].n);
		fwrite(q.p, 1, q.n, stdout); putchar('\n');
	}
	layered_free(&L);
	return 0;
}

// Every field path in the document, one per line, in file order and
// deduplicated - the whole-document counterpart of do_children.
static int do_paths(Opts *o) {
	if (o->nargs != 1) { fprintf(stderr, "usage: shcl paths [options] FILE (see --help)\n"); return 1; }
	LayeredDoc L; int gate = load_layered(o, o->args[0], &L);
	if (gate) return gate;
	shcl_str *ps = NULL;
	size_t n = shcl_paths(L.doc, &ps);
	for (size_t i = 0; i < n; i++) { fwrite(ps[i].p, 1, ps[i].n, stdout); putchar('\n'); }
	layered_free(&L);
	return 0;
}

static int check_opts(const char *cmd, const Opts *o) {
	static const char *get_ok[] = { "--<type>", "--array", "--slots", "--default", "--on-bad", "--strictness", "--layer", "--set", "--set-literal", "--set-default", "--set-literal-default", "--remove", NULL };
	static const char *set_ok[] = { "--strictness", "--layer", "--set", "--set-literal", "--set-default", "--set-literal-default", "--remove", "--write", "--lossy", NULL };
	static const char *fmt_ok[] = { "--write", "--lossy", "--strictness", "--layer", "--set", "--set-literal", "--set-default", "--set-literal-default", "--remove", NULL };
	static const char *check_ok[] = { "--strictness", "--schema", NULL };
	static const char *init_ok[] = { "--schema", "--no-banner", NULL };
	static const char *enum_ok[] = { "--strictness", "--layer", "--set", "--set-literal", "--set-default", "--set-literal-default", "--remove", NULL };
	static const char *none_ok[] = { NULL };
	const char **allowed = none_ok;
	if (!strcmp(cmd, "get")) allowed = get_ok;
	else if (!strcmp(cmd, "set")) allowed = set_ok;
	else if (!strcmp(cmd, "fmt")) allowed = fmt_ok;
	else if (!strcmp(cmd, "check")) allowed = check_ok;
	else if (!strcmp(cmd, "init")) allowed = init_ok;
	else if (!strcmp(cmd, "count") || !strcmp(cmd, "instances")
	         || !strcmp(cmd, "children") || !strcmp(cmd, "paths")) allowed = enum_ok;
	for (int i = 0; i < o->nseen; i++) {
		int ok = 0;
		for (int k = 0; allowed[k]; k++) if (!strcmp(o->seen[i], allowed[k])) { ok = 1; break; }
		if (!ok) {
			if (!strcmp(o->seen[i], "--<type>")) fprintf(stderr, "type options are not valid for %s (see --help)\n", cmd);
			// The one refusal a user is likely to want anyway: check reports
			// line numbers, and a merged document has no single file to number
			// against. Naming the pipeline turns a dead end into a one-liner.
			// Deliberate, not an oversight: the schema is a program artifact, so
			// it always loads at Standard - the same rule `check --schema`
			// follows for the schema half.
			else if (!strcmp(cmd, "init") && !strcmp(o->seen[i], "--strictness"))
				fprintf(stderr, "option --strictness not valid for init: a schema always loads at standard strictness, being a program artifact rather than user data\n");
			else if (!strcmp(cmd, "check") && (!strcmp(o->seen[i], "--layer") || !strcmp(o->seen[i], "--set") || !strcmp(o->seen[i], "--set-literal")))
				fprintf(stderr, "option %s not valid for check: diagnostics cite line numbers, which a merged document has none of. Pipe instead: shcl fmt %s ... FILE | shcl check --schema=SCHEMA -\n", o->seen[i], o->seen[i]);
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
		fprintf(stderr, "--write cannot be combined with %s (see --help)\n", o->sets[0].opt);
		return 1;
	}
	// --lossy only overrides the in-place write's refusal, so on its own it says
	// --default says "substitute this" and --on-bad=error says "fail instead",
	// so the two together are a contradiction. Each used to overwrite the
	// other's mode, which made the answer depend on the order they were typed
	// in.
	if (o->deflt && o->on_bad_arg && strcmp(o->on_bad_arg, "default") != 0) {
		fprintf(stderr, "--default cannot be combined with --on-bad=%s (see --help)\n", o->on_bad_arg);
		return 1;
	}
	// nothing and would read as protection the command never had.
	if (o->lossy && !o->write) {
		fprintf(stderr, "--lossy is only meaningful with --write (see --help)\n");
		return 1;
	}
	// The ops script already has stdin, so a layer cannot read it too.
	if (!strcmp(cmd, "set")) {
		for (int i = 0; i < o->nlayers; i++) {
			if (!strcmp(o->layers[i], "-")) {
				fprintf(stderr, "--layer=- is not valid for set (stdin carries the ops script or the document)\n");
				return 1;
			}
		}
	}
	// Stdin reads once; a second '-' would silently get an empty document.
	int stdin_uses = 0;
	for (int i = 0; i < o->nlayers; i++) if (!strcmp(o->layers[i], "-")) stdin_uses++;
	if (o->schema && !strcmp(o->schema, "-")) stdin_uses++;
	if (o->nargs > 0 && !strcmp(o->args[0], "-")) stdin_uses++;
	if (stdin_uses > 1) {
		fprintf(stderr, "'-' (stdin) can be named only once across FILE, --layer and --schema\n");
		return 1;
	}
	return 0;
}

// Did the command line ask for one of the informational outputs? Only tokens
// in option position count: the value of a value-taking option and anything
// after `--` are data (a FILE or PATH spelled `-h` needs the `--` anyway,
// since the option parser would refuse it). Scanning values too once let a
// read of a missing path answer with the help text and exit 0.
static const char *asked_for(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-h") || !strcmp(a, "--help")) return "help";
		if (!strcmp(a, "-v") || !strcmp(a, "-V") || !strcmp(a, "--version")) return "version";
		if (!strcmp(a, "--about")) return "about";
		if (!strcmp(a, "--donate")) return "donate";
		if (!strcmp(a, "--")) return NULL;
		if (!strcmp(a, "--default") || !strcmp(a, "--on-bad") || !strcmp(a, "--strictness") || !strcmp(a, "--schema") || !strcmp(a, "--layer") || !strcmp(a, "--set") || !strcmp(a, "--set-literal") || !strcmp(a, "--set-default") || !strcmp(a, "--set-literal-default") || !strcmp(a, "--remove")) i++;
	}
	return NULL;
}

static const char *const COMMANDS[] = { "get", "set", "fmt", "check", "init", "count", "instances", "children", "paths" };
static int is_command(const char *cmd) {
	for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++)
		if (!strcmp(cmd, COMMANDS[i])) return 1;
	return 0;
}

#ifdef _WIN32
// The narrow argv arrives in the active code page, best-fit mapped: a name
// the page cannot spell becomes a different name, and `--write` then rewrites
// a different file. The wide command line is exact; hand it over as UTF-8,
// which is what the library's file tier expects. NULL when the conversion
// fails (nothing sensible is left to run).
static char **utf8_argv(int *argc) {
	int n = 0;
	wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &n);
	if (!wargv) return NULL;
	char **out = (char **)calloc((size_t)n + 1, sizeof *out);
	if (!out) return NULL;
	for (int i = 0; i < n; i++) {
		int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
		if (len <= 0 || !(out[i] = (char *)malloc((size_t)len))) return NULL;
		WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, out[i], len, NULL, NULL);
	}
	LocalFree(wargv);
	*argc = n;
	return out;
}
#endif

static int cli_main(int argc, char **argv) {
	setlocale(LC_ALL, "C"); // strtod/printf must use '.' regardless of environment
#ifndef _WIN32
	// The Rust, Go and Python runtimes point a standard stream that was closed
	// before the start at the null device; C's does not, so every write would
	// fail with EBADF where the other three quietly drop it - and the next
	// file opened would land on fd 1 and be written over.
	for (int fd = 0; fd <= 2; fd++)
		if (fcntl(fd, F_GETFD) == -1 && errno == EBADF) {
			int nfd = open("/dev/null", fd == 0 ? O_RDONLY : O_WRONLY);
			if (nfd != fd && nfd >= 0) close(nfd);
		}
#endif
#ifdef _WIN32
	// Byte-for-byte with the reference: no CRLF translation on any stream.
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);
	if (!(argv = utf8_argv(&argc))) { fprintf(stderr, "cannot read the command line\n"); return 1; }
#endif
	// Reject non-UTF-8 argv up front (exit 1), matching the reference; the parser
	// assumes valid UTF-8, and a garbled arg is a usage error, not a real miss.
	for (int i = 1; i < argc; i++) {
		if (!utf8_valid(argv[i], strlen(argv[i]))) {
			fprintf(stderr, "invalid argument encoding (expected UTF-8)\n");
			return 1;
		}
	}
	const char *asked = asked_for(argc, argv);
	// One convention: asking for the help - by name, by flag, or by asking for
	// nothing at all - prints it and succeeds. The blank lines separate the
	// block from the surrounding prompts. A bare run used to print the same
	// text unpadded and exit 1, which read as neither a help nor an error.
	if (argc <= 1) { printf("\n%s\n", HELP); return 0; }
	if ((asked && !strcmp(asked, "help")) || !strcmp(argv[1], "help")) { printf("\n%s\n", HELP); return 0; }
	if ((asked && !strcmp(asked, "version")) || !strcmp(argv[1], "version")) { printf("shcl %s\n", VERSION); return 0; }
	if ((asked && !strcmp(asked, "about")) || !strcmp(argv[1], "about")) { printf("\n%s\n", ABOUT); return 0; }
	if ((asked && !strcmp(asked, "donate")) || !strcmp(argv[1], "donate")) { printf("\n%s\n", DONATE); return 0; }
	const char *cmd = argv[1];
	if (!is_command(cmd)) {
		// Before the options are judged, so a typo in the command is reported
		// as that and not as an option the wrong command cannot take.
		if (cmd[0] == '-' && strcmp(cmd, "--") != 0) fprintf(stderr, "unknown option: %s (see --help)\n", cmd);
		else fprintf(stderr, "unknown command: %s (see --help)\n", cmd);
		return 1;
	}
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
	else if (!strcmp(cmd, "children")) rc = do_children(&o);
	else if (!strcmp(cmd, "paths")) rc = do_paths(&o);
	// A refusal rather than a fall-through: with one, adding a name to COMMANDS
	// without adding a branch here quietly ran whichever command the last line
	// named, with no message. main gates on COMMANDS first, so this is only
	// reachable through that mistake.
	else { fprintf(stderr, "%s: no dispatch arm (see --help)\n", cmd); rc = 1; }
	opts_free(&o);
	return rc;
}

int main(int argc, char **argv) {
	int code = cli_main(argc, argv);
	// A tail still sitting in the buffer when the work is done fails the same
	// way a write does. A reader that closed early is nothing to report -
	// nobody is there to read it - so that leaves quietly; anything else lost
	// the output, which is the same failure as a file that could not be
	// written.
	if (fflush(stdout) != 0 || ferror(stdout)) {
		if (errno == EPIPE) return 0;
		fprintf(stderr, "stdout: %s\n", strerror(errno));
		return 8;
	}
	return code;
}
