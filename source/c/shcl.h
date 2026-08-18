// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

// SHCL reference implementation for C: parser, accessor, writer/formatter.
// Single-header, drop-in: copy this file into your tree and, in exactly ONE .c,
//   #define SHCL_IMPLEMENTATION
//   #include "shcl.h"
// Everything is byte-for-byte with the Rust reference (source/rust); the cicd
// cross-binding check compares CLI stdout + exit codes across every binding.
// The language spec lives in project/spec.md; project/conformance/ pins behavior.
// Structure deliberately mirrors the reference over C-local shortcuts, so a fix
// there ports here by mechanical diff (parity over idiom - see style-guide.md).
//
// A companion C++ typed veneer (get<int64_t>() etc.) sits in shcl.hpp; it wraps
// this core, it is not a second parser.

// The file tier calls POSIX (fdopen, fileno, fchmod, open, fsync, getpid). Those
// prototypes are feature-gated, and a feature request only counts before the
// first system header - so it goes here rather than beside the code needing it,
// and a consumer who already asked for a level keeps theirs.
#if !defined(SHCL_NO_FILE_IO) && !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
	#define _POSIX_C_SOURCE 200809L
	#define _XOPEN_SOURCE 700
#endif

#ifndef SHCL_H
#define SHCL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A borrowed byte string (length-delimited; text may hold NUL and is UTF-8).
typedef struct { const char *p; size_t n; } shcl_str;

typedef enum { SHCL_LOOSE, SHCL_STANDARD, SHCL_STRICT } shcl_strictness;
typedef enum { SHCL_SEV_ERROR, SHCL_SEV_HINT } shcl_severity;

// Read status sentinels. Empty is informational - the empty value still returns.
typedef enum {
	SHCL_GOOD, SHCL_EMPTY, SHCL_NOT_FOUND, SHCL_BAD_TYPE, SHCL_MULTIPLE
} shcl_status;

// Why a write would fail (shcl_write_reason_()): the distinctions behind a
// setter's bare 0. SHCL_W_WRITABLE = the path passes the writer's validation;
// the rest name the five ways it cannot.
typedef enum {
	SHCL_W_WRITABLE,
	SHCL_W_BAD_PATH,      // empty path, or the scanner rejected it
	SHCL_W_VALUE_IN_PATH, // the path carries a `: value` part; writes take values separately
	SHCL_W_WILDCARD,      // wildcard selectors are query-only
	SHCL_W_NO_SUCH_INDEX, // a `[#k]` instance that does not (and can never) exist
	SHCL_W_TOO_DEEP       // deeper than the nesting cap; the writer never creates past it
} shcl_write_reason;

typedef struct shcl_doc shcl_doc;

// Local (floating) date/time unless a zone suffix was present. has_* fields say
// which parts were written; format via shcl_datetime_str.
typedef enum { SHCL_ZONE_NONE, SHCL_ZONE_UTC, SHCL_ZONE_OFFSET } shcl_zone_kind;
typedef struct {
	int has_date; int32_t year; uint32_t month; uint32_t day;
	int has_time; uint32_t hour; uint32_t minute; int has_sec; uint32_t sec;
	int has_frac; shcl_str frac;      // fractional-second digits, as typed
	shcl_zone_kind zone; int32_t off_min;
} shcl_datetime;

typedef struct { int64_t value;  shcl_status status; } shcl_read_i64;
typedef struct { double  value;  shcl_status status; } shcl_read_f64;
typedef struct { int     value;  shcl_status status; } shcl_read_bool;
typedef struct { shcl_str value; shcl_status status; } shcl_read_str;
typedef struct { shcl_datetime value; shcl_status status; } shcl_read_dt;

// Array results also carry one status per slot (element, or wildcard instance)
// in statuses[0..n); status is then the worst slot. NULL on whole-path errors.
typedef struct { int64_t *values; size_t n; shcl_status status; const shcl_status *statuses; } shcl_read_i64_arr;
typedef struct { double  *values; size_t n; shcl_status status; const shcl_status *statuses; } shcl_read_f64_arr;
typedef struct { int     *values; size_t n; shcl_status status; const shcl_status *statuses; } shcl_read_bool_arr;
typedef struct { shcl_str *values; size_t n; shcl_status status; const shcl_status *statuses; } shcl_read_str_arr;
typedef struct { shcl_datetime *values; size_t n; shcl_status status; const shcl_status *statuses; } shcl_read_dt_arr;

// Maximum nesting depth (levels below the document root), enforced at load and
// by the Writer. Deeper lines are skipped with an E016 error. The cap is what
// keeps the recursive tree walks (emit, merge, clone) safely inside every
// binding's stack, so a hostile or machine-generated document can make a load
// fail but never crash the consumer.
#define SHCL_MAX_DEPTH ((size_t)512)

// Parse never fails: bad lines are skipped and diagnosed. Text need not be NUL
// terminated. Free with shcl_free.
shcl_doc *shcl_parse(const char *text, size_t len);
shcl_doc *shcl_parse_with(const char *text, size_t len, shcl_strictness s);
void shcl_free(shcl_doc *d);

// True when a strict load would fail (strictness==strict and an error diagnostic
// exists). At loose/standard this is always false.
int shcl_strict_failed(const shcl_doc *d);
shcl_strictness shcl_strictness_of(const shcl_doc *d);

// Diagnostics, in emission order (parse-time diagnostics, then repeated-leaf hints).
size_t shcl_diag_count(const shcl_doc *d);
size_t shcl_diag_line(const shcl_doc *d, size_t i);
shcl_severity shcl_diag_severity(const shcl_doc *d, size_t i);
shcl_str shcl_diag_message(const shcl_doc *d, size_t i);
// Stable machine code (E001.., H001..) identifying the diagnostic kind - the
// contract; the message prose is a free, per-binding voice. NUL-terminated.
const char *shcl_diag_code(const shcl_doc *d, size_t i);
// How many error-severity diagnostics the document carries - the "did this
// file have errors?" predicate, so recover-and-continue can't read as success
// by accident. Counts whatever the shcl_diag_* accessors hold (after
// shcl_load_and_validate, that includes validation errors).
size_t shcl_lost_count(const shcl_doc *d);
size_t shcl_error_count(const shcl_doc *d);

// Schema validation (spec.md "Schema validation"): check d against a schema
// document (itself plain SHCL). Zero diagnostics = the document conforms.
// Diagnostic lines are document lines (0 = document scope); schema faults
// (V09x, schema-file lines) come first, and the surviving constraints still
// check the document. The unknown-field sweep runs too, unless a fault cost
// the schema a path spelling (an unreadable `field:` path, or a mount naming
// no declared fragment) - only those can turn declared fields into false
// unknowns; a key-level fault keeps its entry's chain. The result owns copies
// of all its strings - free with shcl_validation_free.
typedef struct shcl_validation shcl_validation;
shcl_validation *shcl_validate(shcl_doc *d, shcl_doc *schema);
size_t shcl_validation_count(const shcl_validation *v);
size_t shcl_validation_line(const shcl_validation *v, size_t i);
shcl_severity shcl_validation_severity(const shcl_validation *v, size_t i);
shcl_str shcl_validation_message(const shcl_validation *v, size_t i);
const char *shcl_validation_code(const shcl_validation *v, size_t i);
void shcl_validation_free(shcl_validation *v);

// Drop the H001 hints a schema disavows: a field whose declared repeat upper
// bound is above 1 repeats BY DESIGN (repetition is its instance mechanism),
// so the repeated-bare-leaf hint is structurally a false positive there and
// trains users to ignore hints. Matching is by leaf name - the filter
// consumers were hand-rolling - which errs toward quiet, for a hint. Used by
// `check --schema` and shcl_load_and_validate; call it wherever doc
// diagnostics and a schema meet. Compacts doc's own diagnostic list in place
// (the C spelling of filtering a caller-held list).
void shcl_suppress_declared_repeats(shcl_doc *schema, shcl_doc *doc);

// Sibling for H002: drop the merge hints a schema disavows via `reopen: true`
// on a section's entry - a section meant to be written in parts. Same leaf-name
// matching and in-place compaction as shcl_suppress_declared_repeats.
void shcl_suppress_declared_reopens(shcl_doc *schema, shcl_doc *doc);

// One-shot load-and-validate: parse at a strictness, validate against a
// schema, and hand back a document whose shcl_diag_* accessors serve ONE
// combined list (parse first, then validation - the order `check --schema`
// prints), so half the errors can't vanish because a caller forgot one of the
// two lists. Never fails: a strict-failing document comes back as the
// document plus its diagnostics (shcl_error_count answers "did it fail"). An
// empty schema text skips validation entirely. H001 hints the schema disavows
// (a declared repeat upper bound above 1) are dropped. Free with shcl_free.
shcl_doc *shcl_load_and_validate(const char *text, size_t len, const char *schema, size_t slen, shcl_strictness s);

// File tier (optional companion; compile out with -DSHCL_NO_FILE_IO to keep
// the core free of file I/O). Load never fails: the document always comes
// back usable (empty when the file could not be read), and the status
// out-param (may be NULL) separates the four cases a consumer's own load path
// otherwise confuses. Save writes the canonical text through a temp file in
// the same directory plus a rename - the same mechanics the CLI's --write
// uses - so an interrupted save can never truncate the config it rewrites.
#ifndef SHCL_NO_FILE_IO
typedef enum {
	SHCL_FILE_CLEAN,      /* read and parsed, no error diagnostics (hints allowed) */
	SHCL_FILE_HAD_ERRORS, /* read and parsed, but error diagnostics are present */
	SHCL_FILE_NOT_FOUND,  /* no file at the path */
	SHCL_FILE_UNREADABLE  /* exists but could not be read (permissions, a directory) */
} shcl_file_status;
shcl_doc *shcl_load_file(const char *path, shcl_file_status *status);
shcl_doc *shcl_load_file_with(const char *path, shcl_strictness s, shcl_file_status *status);
int shcl_save_file(shcl_doc *d, const char *path);
int shcl_save_file_lossy(shcl_doc *d, const char *path);
int shcl_write_file_atomic(const char *path, const char *data, size_t n);
#endif

// Schema-driven generation (`shcl init --schema`): a commented, typed starter
// config from a schema document. Required paths are live (their `default`, or an
// empty value); optional paths are commented out; wildcard paths are listed in a
// trailing comment block. A footer naming the format and pointing at the spec is
// written last unless no_banner; the flag is negative so passing 0 writes the
// footer. *ok is set to 1 on success, 0 if the schema has faults (V09x) - then
// the returned string is empty. Bytes live in the schema's arena.
shcl_str shcl_generate(shcl_doc *schema, int no_banner, int *ok);

// Canonical form (block layout, tabs, insertion order, minimal quoting). The
// returned bytes live in the document's arena; valid until shcl_free. The
// bytes may contain NUL - never hand them to a strlen-based API.
shcl_str shcl_to_canonical(shcl_doc *d);

size_t shcl_count(shcl_doc *d, const char *path, size_t plen);
// Instance display values, in file order. Writes an arena-owned array to *out.
size_t shcl_instances(shcl_doc *d, const char *path, size_t plen, shcl_str **out);
// 1-based source line of the binding at a path, for consumer checks the
// schema cannot express. 0 when the path does not resolve to exactly one
// node, or the node was writer-built. Merged instances cite the first
// binding's line, matching diagnostics.
size_t shcl_line(shcl_doc *d, const char *path, size_t plen);

// The field name at a path exactly as the author spelled it (case unfolded,
// quotes and escapes resolved), so a message can echo SYMBOLS when the file
// said SYMBOLS. Resolution mirrors shcl_line: empty when the path does not
// resolve to exactly one node. Merged instances keep the first binding's
// spelling; a writer-built node keeps the spelling the setter's path used.
// Borrowed from the document's arena; valid until shcl_free.
shcl_str shcl_source_name(shcl_doc *d, const char *path, size_t plen);
// The plural shcl_line: 1-based source lines at a path, in file order, so a
// repeated field - the case that most wants a citable line - yields every
// binding's. Wildcard slots that did not resolve stay in the list as 0, and a
// writer-built node is 0, so indices keep matching shcl_count. Writes an
// arena-owned array to *out.
size_t shcl_lines(shcl_doc *d, const char *path, size_t plen, size_t **out);
// Child field names under a path, in file order, duplicates included - the
// "what keys are in this section?" question shcl_paths (deduplicated,
// path-shaped) cannot answer. An empty or whitespace-only path enumerates the
// top level. Names come back as stored; shcl_quote_segment makes one
// splice-safe in a path. Writes an arena-owned array to *out.
size_t shcl_children(shcl_doc *d, const char *path, size_t plen, shcl_str **out);
// Every field path in the document, in file order, deduplicated - a query
// recipe for tooling. A segment that is not bare-name-safe is emitted quoted
// and escaped - the form the path scanner accepts - so each path is a
// well-formed lookup path and nothing in the document is hidden. Returns the
// count; *out stays valid until shcl_free.
size_t shcl_paths(shcl_doc *d, shcl_str **out);
// Quote one path segment so it can be spliced into a lookup path: a bare name
// passes through, anything else comes back quoted and escaped in the form the
// path scanner accepts. Splicing user-typed text into a path without this is
// path injection - a dotted name silently reads as nesting. Same spelling
// shcl_paths and the canonical emitter produce. Result lives in the
// document's arena; valid until shcl_free.
shcl_str shcl_quote_segment(shcl_doc *d, const char *name, size_t len);

shcl_read_i64  shcl_read_int(shcl_doc *d, const char *path, size_t plen);
shcl_read_f64  shcl_read_float(shcl_doc *d, const char *path, size_t plen);
shcl_read_bool shcl_read_bool_(shcl_doc *d, const char *path, size_t plen);
shcl_read_dt   shcl_read_datetime(shcl_doc *d, const char *path, size_t plen);
shcl_read_str  shcl_read_string(shcl_doc *d, const char *path, size_t plen);
shcl_read_str  shcl_read_raw(shcl_doc *d, const char *path, size_t plen);
shcl_read_str  shcl_read_raw_info(shcl_doc *d, const char *path, size_t plen);

shcl_read_i64_arr  shcl_read_int_array(shcl_doc *d, const char *path, size_t plen);
shcl_read_f64_arr  shcl_read_float_array(shcl_doc *d, const char *path, size_t plen);
shcl_read_bool_arr shcl_read_bool_array(shcl_doc *d, const char *path, size_t plen);
shcl_read_dt_arr   shcl_read_datetime_array(shcl_doc *d, const char *path, size_t plen);
shcl_read_str_arr  shcl_read_string_array(shcl_doc *d, const char *path, size_t plen);

// Convenience tier: the value, or the call-site fallback unless the read is Good
// - so a missing/empty/bad/ambiguous read cannot masquerade as a real zero. The
// string/datetime/raw and array reads keep the shcl_read_* status tier above.
int64_t shcl_get_int(shcl_doc *d, const char *path, size_t plen, int64_t def);
double  shcl_get_float(shcl_doc *d, const char *path, size_t plen, double def);
int     shcl_get_bool(shcl_doc *d, const char *path, size_t plen, int def);

// --- Writer: typed emit, defaults, comments, structural edits ---------------
// The reverse of the reads. Each setter builds the canonical stored text for a
// typed value and places it at a path (creating intermediate nodes). New values
// are copied into the arena, so the caller's buffers need not outlive the call.
// Setters return 1 when the write applied, 0 when the path is unusable
// (wildcard, missing [#N] instance, a value part, or past the depth cap) -
// nothing is created on failure. _default forms return 1 when already present.
shcl_doc *shcl_new(void); // an empty document (start point for generation)
int shcl_exists(shcl_doc *d, const char *path, size_t plen);       // 0/1
size_t shcl_remove(shcl_doc *d, const char *path, size_t plen);    // count deleted
int shcl_set_comment(shcl_doc *d, const char *path, size_t plen, const char *text, size_t tlen);
int shcl_set_empty(shcl_doc *d, const char *path, size_t plen);
// Why a write at this path would fail - the reason behind a setter's bare 0,
// so a consumer's error message need not guess. SHCL_W_WRITABLE means the same
// validation the setters run would pass. Probes only; never creates.
shcl_write_reason shcl_write_reason_(shcl_doc *d, const char *path, size_t plen);

int shcl_set_int(shcl_doc *d, const char *path, size_t plen, int64_t v);
int shcl_set_float(shcl_doc *d, const char *path, size_t plen, double v);
int shcl_set_bool(shcl_doc *d, const char *path, size_t plen, int v);
int shcl_set_string(shcl_doc *d, const char *path, size_t plen, const char *s, size_t slen);
int shcl_set_datetime(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *dt);
int shcl_set_raw(shcl_doc *d, const char *path, size_t plen, const char *content, size_t clen, const char *info, size_t ilen);

int shcl_set_int_array(shcl_doc *d, const char *path, size_t plen, const int64_t *v, size_t n);
int shcl_set_float_array(shcl_doc *d, const char *path, size_t plen, const double *v, size_t n);
int shcl_set_bool_array(shcl_doc *d, const char *path, size_t plen, const int *v, size_t n);
int shcl_set_string_array(shcl_doc *d, const char *path, size_t plen, const char *const *v, const size_t *lens, size_t n);
int shcl_set_datetime_array(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *v, size_t n);

// Binds text as value syntax rather than as data: "80, 443" becomes a
// two-element array where shcl_set_string would store one string that has to be
// quoted. For a caller holding value text - a config line, a user's --set
// argument - that has to be written without knowing its shape first. Returns 0
// for text that could not be one line's value (a line break, or a quote that
// never closes); an unquoted # ends the value as it would in a file.
int shcl_set_literal(shcl_doc *d, const char *path, size_t plen, const char *text, size_t tlen);

// Default (only-if-absent) forms - the "emit defaults" half of the Writer.
int shcl_set_int_default(shcl_doc *d, const char *path, size_t plen, int64_t v);
int shcl_set_float_default(shcl_doc *d, const char *path, size_t plen, double v);
int shcl_set_bool_default(shcl_doc *d, const char *path, size_t plen, int v);
int shcl_set_string_default(shcl_doc *d, const char *path, size_t plen, const char *s, size_t slen);
int shcl_set_datetime_default(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *dt);
int shcl_set_literal_default(shcl_doc *d, const char *path, size_t plen, const char *text, size_t tlen);
int shcl_set_raw_default(shcl_doc *d, const char *path, size_t plen, const char *content, size_t clen, const char *info, size_t ilen);
int shcl_set_int_array_default(shcl_doc *d, const char *path, size_t plen, const int64_t *v, size_t n);
int shcl_set_float_array_default(shcl_doc *d, const char *path, size_t plen, const double *v, size_t n);
int shcl_set_bool_array_default(shcl_doc *d, const char *path, size_t plen, const int *v, size_t n);
int shcl_set_string_array_default(shcl_doc *d, const char *path, size_t plen, const char *const *v, const size_t *lens, size_t n);
int shcl_set_datetime_array_default(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *v, size_t n);

// --- Layered loading --------------------------------------------------------
// Overlay `over` (a higher-priority layer) onto `d` (the lower one). Container
// instances merge by (name, value) like the in-file rule; a leaf name present
// in `over` replaces d's same-named children at that scope - provided those
// base children are leaves too (real override for scalars, arrays, raw blocks;
// a bare section header merges instead of wiping); over-only nodes are
// appended. `over`'s content is deep-copied into d's arena, so d stays valid
// after `over` is freed.
void shcl_merge(shcl_doc *d, const shcl_doc *over);

// CLI/aliases: 1|2|3 or loose|standard|strict. Returns 1 on success.
int shcl_strictness_from_arg(const char *s, size_t n, shcl_strictness *out);

// Format helpers matching the reference's textual output.
// out must be at least SHCL_F64_BUF bytes; returns the byte length written.
#define SHCL_F64_BUF 512
size_t shcl_format_f64(double v, char *out);
// Renders a datetime into out (>= 64 bytes); returns byte length. A frac
// longer than 30 bytes is truncated, and the whole rendering is clamped to 64
// bytes, so a hand-built value cannot overrun the documented buffer (parsed
// input never gets near either limit).
size_t shcl_datetime_str(const shcl_datetime *dt, char *out);
// Status <-> the CLI exit code / textual name.
int shcl_status_code(shcl_status s);
const char *shcl_status_name(shcl_status s);

#ifdef __cplusplus
}
#endif

// ===========================================================================
#ifdef SHCL_IMPLEMENTATION

#ifdef __cplusplus
// The implementation zero-initializes aggregates with the C idiom `{0}`; C++
// -Wextra flags every one as a missing-field-initializer, which would break a
// consumer compiling this header into a C++ TU with -Werror. Scoped to the
// implementation only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <inttypes.h>
#include <locale.h>

// SHCL spells a float with '.', but strtod and printf use whatever the host
// locale calls the decimal point - so in a consumer that has called setlocale
// both directions of float conversion need translating. The CLI never sets a
// locale, which is why only library callers ever saw this.
static const char *dec_point(void) {
	const char *p = localeconv()->decimal_point;
	return (p && *p) ? p : ".";
}

// --- arena (bump allocator; growable vectors grow by copy, bulk-freed) -------

typedef struct ShclBlock { struct ShclBlock *next; size_t used, cap; } ShclBlock;
typedef struct { ShclBlock *head; } Arena;

static void *arena_alloc(Arena *a, size_t n) {
	n = (n + 15u) & ~(size_t)15u;
	if (n == 0) n = 16;
	if (!a->head || a->head->used + n > a->head->cap) {
		size_t cap = n > (size_t)65536 ? n : (size_t)65536;
		ShclBlock *b = (ShclBlock *)malloc(sizeof(ShclBlock) + cap);
		if (!b) { fprintf(stderr, "shcl: out of memory\n"); exit(70); }
		b->next = a->head; b->used = 0; b->cap = cap; a->head = b;
	}
	void *p = (char *)(a->head + 1) + a->head->used;
	a->head->used += n;
	return p;
}
static void arena_free(Arena *a) {
	ShclBlock *b = a->head;
	while (b) { ShclBlock *n = b->next; free(b); b = n; }
	a->head = NULL;
}
static void *arena_grow(Arena *a, void *old, size_t oldcap, size_t newcap, size_t sz) {
	void *p = arena_alloc(a, newcap * sz);
	if (old && oldcap) memcpy(p, old, oldcap * sz);
	return p;
}
// Reset a scratch arena, keeping its newest (largest) block so steady-state
// reads never re-malloc. Bump arenas cannot free per-object, so without this
// every resolver temporary would live until shcl_free - a long-running process
// doing reads would grow without bound.
static void arena_reset(Arena *a) {
	if (!a->head) return;
	ShclBlock *b = a->head->next;
	while (b) { ShclBlock *n = b->next; free(b); b = n; }
	a->head->next = NULL; a->head->used = 0;
}

#define DEFINE_VEC(Name, T) \
	typedef struct { T *data; size_t len, cap; } Name; \
	static void Name##_push(Arena *a, Name *v, T x) { \
		if (v->len == v->cap) { size_t nc = v->cap ? v->cap * 2 : 8; \
			v->data = (T *)arena_grow(a, v->data, v->cap, nc, sizeof(T)); v->cap = nc; } \
		v->data[v->len++] = x; }

// --- byte-string helpers -----------------------------------------------------

typedef shcl_str S;
static S s_lit(const char *z) { S s; s.p = z; s.n = strlen(z); return s; }
static S s_empty(void) { S s; s.p = ""; s.n = 0; return s; }
static int s_eq(S a, S b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
static S s_dup(Arena *a, S x) {
	if (x.n == 0) return s_empty();
	char *m = (char *)arena_alloc(a, x.n); memcpy(m, x.p, x.n);
	S r; r.p = m; r.n = x.n; return r;
}
static S s_slice(S s, size_t from, size_t to) { S r; r.p = s.p + from; r.n = to - from; return r; }
static int s_starts(S s, const char *pre) {
	size_t n = strlen(pre); return s.n >= n && memcmp(s.p, pre, n) == 0;
}

typedef struct { char *data; size_t len, cap; } SB;
static void sb_put(Arena *a, SB *s, const char *p, size_t n) {
	if (!n) return;
	if (s->len + n > s->cap) { size_t nc = s->cap ? s->cap * 2 : 32;
		while (nc < s->len + n) nc *= 2;
		s->data = (char *)arena_grow(a, s->data, s->cap, nc, 1); s->cap = nc; }
	memcpy(s->data + s->len, p, n); s->len += n;
}
static void sb_putc(Arena *a, SB *s, char c) { sb_put(a, s, &c, 1); }
static void sb_puts(Arena *a, SB *s, const char *z) { sb_put(a, s, z, strlen(z)); }
static void sb_putS(Arena *a, SB *s, S x) { sb_put(a, s, x.p, x.n); }
static S sb_S(SB *s) { S r; r.p = s->data ? s->data : ""; r.n = s->len; return r; }

// --- UTF-8 (input is validated at the CLI; here we assume it is well formed) --

static size_t utf8_decode(const char *p, size_t n, size_t i, uint32_t *cp) {
	unsigned char c = (unsigned char)p[i];
	if (c < 0x80) { *cp = c; return 1; }
	if ((c >> 5) == 0x6 && i + 1 < n) {
		*cp = ((uint32_t)(c & 0x1F) << 6) | (p[i + 1] & 0x3F); return 2;
	}
	if ((c >> 4) == 0xE && i + 2 < n) {
		*cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F); return 3;
	}
	if ((c >> 3) == 0x1E && i + 3 < n) {
		*cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[i + 1] & 0x3F) << 12)
			| ((uint32_t)(p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F); return 4;
	}
	*cp = c; return 1;
}
static size_t utf8_encode(uint32_t cp, char out[4]) {
	if (cp < 0x80) { out[0] = (char)cp; return 1; }
	if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
// Byte length of the last codepoint in s (0 if empty); *cp gets its value.
static size_t utf8_last(S s, uint32_t *cp) {
	if (s.n == 0) { *cp = 0; return 0; }
	size_t i = s.n - 1;
	while (i > 0 && ((unsigned char)s.p[i] & 0xC0) == 0x80) i--;
	utf8_decode(s.p, s.n, i, cp);
	return s.n - i;
}
static void sb_put_cp(Arena *a, SB *s, uint32_t cp) {
	char buf[4]; size_t l = utf8_encode(cp, buf); sb_put(a, s, buf, l);
}

// Decode s into a codepoint array with byte offsets (off has n+1 entries).
typedef struct { uint32_t *cp; size_t *off; size_t n; } CPs;
static CPs decode_cps(Arena *a, S s) {
	size_t m = 0;
	for (size_t i = 0; i < s.n;) { uint32_t c; i += utf8_decode(s.p, s.n, i, &c); m++; }
	CPs r; r.n = m;
	r.cp = (uint32_t *)arena_alloc(a, (m ? m : 1) * sizeof(uint32_t));
	r.off = (size_t *)arena_alloc(a, (m + 1) * sizeof(size_t));
	size_t i = 0, k = 0;
	while (i < s.n) { uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c); r.cp[k] = c; r.off[k] = i; i += l; k++; }
	r.off[m] = s.n;
	return r;
}
static S cps_slice(S src, CPs c, size_t from, size_t to) {
	S r; r.p = src.p + c.off[from]; r.n = c.off[to] - c.off[from]; return r;
}

// --- whitespace (Rust char::is_whitespace / Unicode White_Space) + ascii ------

static int is_ws(uint32_t c) {
	switch (c) {
	case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
	case 0x85: case 0xA0: case 0x1680:
	case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
	case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
	case 0x200A: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
		return 1;
	default: return 0;
	}
}
static S trim_start(S s) {
	size_t i = 0;
	while (i < s.n) { uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c); if (!is_ws(c)) break; i += l; }
	return s_slice(s, i, s.n);
}
static S trim_end(S s) {
	while (s.n) { uint32_t c; size_t l = utf8_last(s, &c); if (!is_ws(c)) break; s.n -= l; }
	return s;
}
static S s_trim(S s) { return trim_end(trim_start(s)); }

static int is_adigit(uint32_t c) { return c >= '0' && c <= '9'; }
static int is_ahex(uint32_t c) { return is_adigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int is_aalnum(uint32_t c) {
	return is_adigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int is_bare_name_char(uint32_t c) {
	return (c < 128 && is_aalnum(c)) || c == '-' || c == '_';
}
static int all_adigit0(S s) { for (size_t i = 0; i < s.n; i++) if (!is_adigit((unsigned char)s.p[i])) return 0; return 1; }
static int all_ahex(S s) { for (size_t i = 0; i < s.n; i++) if (!is_ahex((unsigned char)s.p[i])) return 0; return s.n > 0; }
static S ascii_lower(Arena *a, S s) {
	char *m = (char *)arena_alloc(a, s.n ? s.n : 1);
	for (size_t i = 0; i < s.n; i++) { unsigned char c = (unsigned char)s.p[i]; m[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c; }
	S r; r.p = m; r.n = s.n; return r;
}
static S fold_name(Arena *a, S s) { return ascii_lower(a, s); }

// --- in-memory model ---------------------------------------------------------

typedef struct { S text; int quoted; } Element;
DEFINE_VEC(VecEl, Element)

typedef enum { V_EMPTY, V_CELL, V_RAW } vkind;
typedef struct {
	vkind kind;
	Element *els; size_t nels;                 // V_CELL
	size_t cap_els;                            // stacked-list growth only (0 elsewhere)
	S content; S info; unsigned char fence_char; size_t fence_len; // V_RAW
} Value;

DEFINE_VEC(VecSize, size_t)
DEFINE_VEC(VecS, S)

/* One whole-line comment held as trivia, plus whether a blank line preceded
   it - so a blank between comment-only regions survives the round-trip
   (blank runs collapse to one, same as nodes). */
typedef struct { S text; int blank_before; } Lead;
DEFINE_VEC(VecLead, Lead)
static Lead lead_make(S text, int blank_before) { Lead l; l.text = text; l.blank_before = blank_before; return l; }
static Lead lead_plain(S text) { return lead_make(text, 0); }

typedef struct {
	S name;
	/* The name as the author spelled it (case unfolded, quotes and escapes
	   resolved) - what shcl_source_name hands back. Merged instances keep the
	   first binding's spelling, like shcl_line and comments. */
	S name_src;
	Value value;
	VecSize children;
	size_t parent;
	size_t line;
	int star_list;  /* value built from stacked "* " lines */
	int star_mixed; /* mix of "* " and field children already diagnosed */
	/* Comment trivia, verbatim from `#` to end of line. Never part of identity
	   or reads; merged instances concatenate leading, first trailing wins
	   (later ones demote to leading - a canonical line has room for one). */
	VecLead leading;
	S trailing; /* n == 0 = none */
	/* Whole-line comments that followed this node's subtree at a deeper indent
	   than the next binding - they belong to this block, not the next node, so
	   a run trailing a block's last child stays put instead of re-attaching
	   dedented. Emitted after the subtree at this node's depth. */
	VecLead after;
	/* Whole-line comments written inside this node's block when no bound child
	   could take them - a header whose children are all commented still owns
	   those lines. Emitted after the subtree one level deeper than this node. */
	VecLead inside;
	/* Blank-line grouping is the other half of hand-authored layout: set when
	   a blank line preceded this node's binding line (runs collapse to one). */
	int blank_before;
} Node;
DEFINE_VEC(VecNode, Node)

typedef struct { size_t line; shcl_severity sev; S message; const char *code; } Diag;
DEFINE_VEC(VecDiag, Diag)

struct shcl_doc {
	Arena arena;
	// Per-resolve temporaries (path scans, resolver vectors, display strings
	// built only to compare). Reset on entry to each resolve, so read-only use
	// of a long-lived document stays flat; anything HANDED BACK to the caller
	// lives in `arena` (valid until shcl_free, the documented contract).
	Arena scratch;
	VecNode nodes;
	VecDiag diags;
	shcl_strictness strictness;
	VecLead orphans; /* top-level comments after the last binding line */
	/* Lines or values parsing dropped that canonical output cannot re-emit
	   (bad indentation, an unusable selector, past the depth cap, ...).
	   Content-malformed lines are NOT counted - they are retained as trivia
	   and survive a save. shcl_lost_count serves it; shcl_save_file gates on
	   it. */
	size_t lost;
};
#define ROOT ((size_t)0)
#define NODE(d, i) ((d)->nodes.data[i])

/* Merge a later instance into an earlier one under the in-file merge rule:
   children and trivia move over, first trailing wins (a second demotes to a
   leading line), first spelling stays. The caller drops the loser from the
   parent's child list; it keeps its arena slot, unreferenced. */
static void fold_node_into(shcl_doc *d, size_t survivor, size_t loser) {
	Arena *a = &d->arena;
	VecSize kids = NODE(d, loser).children;
	for (size_t k = 0; k < kids.len; k++) {
		NODE(d, kids.data[k]).parent = survivor;
		VecSize_push(a, &NODE(d, survivor).children, kids.data[k]);
	}
	NODE(d, loser).children.len = 0;
	for (size_t k = 0; k < NODE(d, loser).leading.len; k++)
		VecLead_push(a, &NODE(d, survivor).leading, NODE(d, loser).leading.data[k]);
	NODE(d, loser).leading.len = 0;
	if (NODE(d, loser).trailing.n) {
		if (NODE(d, survivor).trailing.n == 0) NODE(d, survivor).trailing = NODE(d, loser).trailing;
		else VecLead_push(a, &NODE(d, survivor).leading, lead_plain(NODE(d, loser).trailing));
		NODE(d, loser).trailing.n = 0;
	}
	for (size_t k = 0; k < NODE(d, loser).after.len; k++)
		VecLead_push(a, &NODE(d, survivor).after, NODE(d, loser).after.data[k]);
	NODE(d, loser).after.len = 0;
	for (size_t k = 0; k < NODE(d, loser).inside.len; k++)
		VecLead_push(a, &NODE(d, survivor).inside, NODE(d, loser).inside.data[k]);
	NODE(d, loser).inside.len = 0;
}

static Value v_empty(void) { Value v; memset(&v, 0, sizeof v); v.kind = V_EMPTY; return v; }
static int v_is_empty(const Value *v) { return v->kind == V_EMPTY; }

// Merge key: nodes with equal (name, key) collapse into one. Each cell element
// (and the raw info-string) is length-prefixed so the joined key is injective: a
// bare NUL separator lets `[a, b]` collide with the single element "a\0b" (NUL is
// legal in a quoted string), silently merging them.
static S value_key(Arena *a, const Value *v) {
	SB s = {0};
	char nb[24];
	if (v->kind == V_EMPTY) { sb_putc(a, &s, 'e'); return sb_S(&s); }
	if (v->kind == V_CELL) {
		sb_puts(a, &s, "c:");
		for (size_t i = 0; i < v->nels; i++) {
			snprintf(nb, sizeof nb, "%zu:", v->els[i].text.n);
			sb_puts(a, &s, nb);
			sb_putS(a, &s, v->els[i].text);
		}
		return sb_S(&s);
	}
	/* Info-string is part of identity (a `sql` and a `python` block are
	   different values even with equal bodies); fence style is not. */
	snprintf(nb, sizeof nb, "%zu:", v->info.n);
	sb_puts(a, &s, "r:"); sb_puts(a, &s, nb); sb_putS(a, &s, v->info); sb_putS(a, &s, v->content);
	return sb_S(&s);
}
static S value_display(Arena *a, const Value *v) {
	if (v->kind == V_EMPTY) return s_empty();
	if (v->kind == V_RAW) return v->content;
	SB s = {0};
	for (size_t i = 0; i < v->nels; i++) { if (i) sb_puts(a, &s, ", "); sb_putS(a, &s, v->els[i].text); }
	return sb_S(&s);
}

// --- lexical helpers ---------------------------------------------------------

// Split off an unquoted trailing comment: returns the content, *comment gets
// the tail from `#` on (n == 0 = none). A backslash shields the next char.
// Comments are kept as trivia.
static S split_comment(S s, S *comment) {
	uint32_t inq = 0; // 0 = none, else the open quote codepoint
	size_t i = 0;
	*comment = s_empty();
	while (i < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c);
		if (c == '\\') { i += l; if (i < s.n) { uint32_t d; i += utf8_decode(s.p, s.n, i, &d); } continue; }
		if (inq) { if (c == inq) inq = 0; }
		else if (c == '"' || c == '\'') inq = c;
		else if (c == '#') { *comment = s_slice(s, i, s.n); return s_slice(s, 0, i); }
		i += l;
	}
	return s;
}
// Split on unquoted commas; backslash shields the next char. Emits byte offsets.
static void split_unquoted_commas(Arena *a, S s, VecSize *offs_start, VecSize *offs_end) {
	uint32_t inq = 0; size_t i = 0, start = 0;
	while (i < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c);
		if (c == '\\') { i += l; if (i < s.n) { uint32_t d; i += utf8_decode(s.p, s.n, i, &d); } continue; }
		if (inq) { if (c == inq) inq = 0; i += l; continue; }
		if (c == '"' || c == '\'') { inq = c; i += l; continue; }
		if (c == ',') { VecSize_push(a, offs_start, start); VecSize_push(a, offs_end, i); start = i + l; i += l; continue; }
		i += l;
	}
	VecSize_push(a, offs_start, start); VecSize_push(a, offs_end, s.n);
}
// Count of comma-split pieces (used where the reference only needs .len()).
static size_t count_unquoted_pieces(S s) {
	uint32_t inq = 0; size_t i = 0, n = 1;
	while (i < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c);
		if (c == '\\') { i += l; if (i < s.n) { uint32_t d; i += utf8_decode(s.p, s.n, i, &d); } continue; }
		if (inq) { if (c == inq) inq = 0; i += l; continue; }
		if (c == '"' || c == '\'') { inq = c; i += l; continue; }
		if (c == ',') { n++; i += l; continue; }
		i += l;
	}
	return n;
}

// A dangling trailing backslash would swallow its separator on re-emit; double it.
static S norm_dangling(Arena *a, S t) {
	size_t run = 0;
	while (run < t.n && t.p[t.n - 1 - run] == '\\') run++;
	if (run % 2 == 1) {
		char *m = (char *)arena_alloc(a, t.n + 1);
		memcpy(m, t.p, t.n); m[t.n] = '\\';
		S r; r.p = m; r.n = t.n + 1; return r;
	}
	return s_dup(a, t);
}

/* True when some piece starts with a quote that never closes (missing or
   escaped). Such a piece stays literal - and the quote-aware comment strip has
   already swallowed any trailing # comment into it - so the parser calls it
   out instead of letting the typo look deliberate. Mid-text apostrophes
   (it's fine) are legal prose and stay silent. */
static int unterminated_quote(Arena *a, S text) {
	VecSize starts = {0}, ends = {0};
	split_unquoted_commas(a, text, &starts, &ends);
	for (size_t i = 0; i < starts.len; i++) {
		S piece; piece.p = text.p + starts.data[i]; piece.n = ends.data[i] - starts.data[i];
		S t = s_trim(piece);
		if (t.n == 0) continue;
		CPs c = decode_cps(a, t);
		uint32_t first = c.cp[0];
		if (first != '"' && first != '\'') continue;
		int closed = 0;
		if (c.n >= 2 && c.cp[c.n - 1] == first) {
			int esc = 0;
			for (size_t k = 1; k < c.n - 1; k++) esc = (c.cp[k] == '\\' && !esc);
			closed = !esc;
		}
		if (!closed) return 1;
	}
	return 0;
}

// Trim, then strip one matching outer quote pair if present. present=0 -> dropped.
static int parse_element(Arena *a, S piece, Element *out) {
	S t = s_trim(piece);
	if (t.n == 0) return 0;
	CPs c = decode_cps(a, t);
	uint32_t first = c.cp[0];
	if ((first == '"' || first == '\'') && c.n >= 2 && c.cp[c.n - 1] == first) {
		int esc = 0;
		for (size_t i = 1; i < c.n - 1; i++) esc = (c.cp[i] == '\\' && !esc);
		if (!esc) {
			out->text = s_dup(a, cps_slice(t, c, 1, c.n - 1));
			out->quoted = 1; return 1;
		}
	}
	out->text = norm_dangling(a, t);
	out->quoted = 0; return 1;
}
// Reads text as the value half of a line - see shcl_set_literal.
static int literal_value(Arena *a, S text, Value *out);

static Value parse_cell(Arena *a, S text) {
	VecSize starts = {0}, ends = {0};
	split_unquoted_commas(a, text, &starts, &ends);
	VecEl els = {0};
	for (size_t i = 0; i < starts.len; i++) {
		Element e;
		if (parse_element(a, s_slice(text, starts.data[i], ends.data[i]), &e)) VecEl_push(a, &els, e);
	}
	if (els.len == 0) return v_empty();
	Value v; memset(&v, 0, sizeof v); v.kind = V_CELL; v.els = els.data; v.nels = els.len;
	return v;
}

// Escape processing (string reads): \t \n \\ \" \'; unknown escapes stay literal.
static S apply_escapes(Arena *a, S s) {
	SB out = {0};
	size_t i = 0;
	while (i < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c); i += l;
		if (c != '\\') { sb_put_cp(a, &out, c); continue; }
		if (i >= s.n) { sb_putc(a, &out, '\\'); break; }
		uint32_t d; size_t l2 = utf8_decode(s.p, s.n, i, &d); i += l2;
		switch (d) {
		case 't': sb_putc(a, &out, '\t'); break;
		case 'n': sb_putc(a, &out, '\n'); break;
		case '\\': sb_putc(a, &out, '\\'); break;
		case '"': sb_putc(a, &out, '"'); break;
		case '\'': sb_putc(a, &out, '\''); break;
		default: sb_putc(a, &out, '\\'); sb_put_cp(a, &out, d); break;
		}
	}
	return sb_S(&out);
}

/* The predicate a `[value]` selector matches with: display form with escapes
   applied on both sides, so `["q\"uote"]` finds `'q"uote'` - a logical-string
   match, not spelling against spelling. */
/* The restriction a QUOTED [value] selector adds on top of the display
   match: quoting selects the scalar spelling only, so the scalar "a, b" and
   the list a, b stop meeting the same selector. */
static int single_scalar(const Value *v) { return v->kind == V_CELL && v->nels == 1; }

static S disp_key(Arena *a, const Value *v) {
	return apply_escapes(a, value_display(a, v));
}

// Opening fence: a run of >=3 backticks or tildes, then an optional info string.
typedef struct { int ok; unsigned char ch; size_t len; S info; } Fence;
static Fence fence_open(Arena *a, S rest) {
	Fence f; f.ok = 0; f.ch = 0; f.len = 0; f.info = s_empty();
	if (rest.n == 0) return f;
	unsigned char first = (unsigned char)rest.p[0];
	if (first != '`' && first != '~') return f;
	size_t run = 0;
	while (run < rest.n && (unsigned char)rest.p[run] == first) run++;
	if (run < 3) return f;
	f.ok = 1; f.ch = first; f.len = run;
	f.info = s_dup(a, s_trim(s_slice(rest, run, rest.n)));
	return f;
}
static int is_fence_close(S line, unsigned char ch, size_t min_len) {
	S t = s_trim(line);
	if (t.n < min_len || t.n == 0) return 0;
	for (size_t i = 0; i < t.n; i++) if ((unsigned char)t.p[i] != ch) return 0;
	return 1;
}

// --- path scanner ------------------------------------------------------------

typedef enum { SEL_NONE, SEL_VALUE, SEL_INDEX, SEL_WILDCARD } seltag;
/* quoted: the selector text was quoted in the path. A quoted selector is
   scalar-only - it matches a single-element value whose logical string equals
   the text - so quoting distinguishes the scalar "a, b" from the two-element
   list a, b, the same way quoting escapes elsewhere. */
typedef struct { seltag tag; S value; uint64_t index; int quoted; } Selector; // u64: width must not vary with target pointer size
typedef struct { S name; S name_src; Selector sel; int star; } Segment; // name_src: as authored (unfolded); star: bare `*` name wildcard; quoted "*" stays a literal name
DEFINE_VEC(VecSeg, Segment)
typedef struct { int ok; VecSeg segs; int has_value; S value_text; S err; } PathScan;

// usize parse: optional single leading '+', >=1 digit, no overflow.
static int parse_u64(S s, uint64_t *out) {
	size_t i = 0;
	if (i < s.n && s.p[i] == '+') i++;
	if (i >= s.n) return 0;
	uint64_t v = 0;
	for (; i < s.n; i++) {
		unsigned char c = (unsigned char)s.p[i];
		if (!is_adigit(c)) return 0;
		if (v > (UINT64_MAX - (c - '0')) / 10) return 0;
		v = v * 10 + (c - '0');
	}
	*out = v; return 1;
}

// Byte-offset cursor over the path text (a decode_cps codepoint array per call
// was a parse hot spot). Positions advance by whole codepoints via the same
// utf8_decode, so the codepoint sequence - and every slice boundary - is
// identical to the old array walk, permissive decoding included.
static void skip_ws_path(S s, size_t *pos) {
	while (*pos < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, *pos, &c);
		if (c != ' ' && c != '\t') break;
		*pos += l;
	}
}
// Read a quoted name/value in a path (escape pairs preserved literally).
static int read_quoted_path(Arena *a, S src, size_t *pos, S *out, S *err) {
	uint32_t q; *pos += utf8_decode(src.p, src.n, *pos, &q);
	SB sb = {0};
	for (;;) {
		if (*pos >= src.n) { *err = s_lit("unterminated quote"); return 0; }
		uint32_t ch; size_t l = utf8_decode(src.p, src.n, *pos, &ch);
		if (ch == '\\' && *pos + l < src.n) {
			uint32_t d; size_t l2 = utf8_decode(src.p, src.n, *pos + l, &d);
			sb_put_cp(a, &sb, ch); sb_put_cp(a, &sb, d);
			*pos += l + l2; continue;
		}
		*pos += l;
		if (ch == q) { *out = sb_S(&sb); return 1; }
		sb_put_cp(a, &sb, ch);
	}
}

static PathScan scan_path_ex(Arena *a, S input, int stars) {
	PathScan ps; ps.ok = 0; memset(&ps.segs, 0, sizeof ps.segs); ps.has_value = 0; ps.value_text = s_empty(); ps.err = s_empty();
	size_t pos = 0;
	for (;;) {
		skip_ws_path(input, &pos);
		if (pos >= input.n) { ps.err = s_lit("empty path"); return ps; }
		// Field name: quoted, bare, or (lookups only) the `*` name wildcard.
		int star = 0;
		S name;
		uint32_t cur; size_t curl = utf8_decode(input.p, input.n, pos, &cur);
		if (cur == '"' || cur == '\'') {
			if (!read_quoted_path(a, input, &pos, &name, &ps.err)) return ps;
		} else if (stars && cur == '*') {
			pos += curl;
			star = 1;
			name = s_lit("*");
		} else {
			size_t start = pos;
			while (pos < input.n) {
				uint32_t bc; size_t bl = utf8_decode(input.p, input.n, pos, &bc);
				if (!is_bare_name_char(bc)) break;
				pos += bl;
			}
			if (pos == start) {
				SB e = {0}; sb_puts(a, &e, "expected field name, found '"); sb_put_cp(a, &e, cur); sb_putc(a, &e, '\'');
				ps.err = sb_S(&e); return ps;
			}
			name = s_slice(input, start, pos);
		}
		Selector sel; sel.tag = SEL_NONE; sel.value = s_empty(); sel.index = 0; sel.quoted = 0;
		skip_ws_path(input, &pos);
		int have_bracket = 0; size_t bracket_end = 0; // byte offset just past the '['
		if (pos < input.n) {
			uint32_t sc; size_t sl = utf8_decode(input.p, input.n, pos, &sc);
			if (sc == '[') { have_bracket = 1; bracket_end = pos + sl; }
			else if (sc == ':') {
				size_t q = pos + sl; skip_ws_path(input, &q);
				if (q < input.n) {
					uint32_t qc; size_t ql = utf8_decode(input.p, input.n, q, &qc);
					if (qc == '[') { have_bracket = 1; bracket_end = q + ql; }
				}
			}
		}
		if (have_bracket) {
			pos = bracket_end;
			skip_ws_path(input, &pos);
			uint32_t oc = 0; size_t ocl = 0;
			if (pos < input.n) ocl = utf8_decode(input.p, input.n, pos, &oc);
			(void)ocl;
			if (pos < input.n && (oc == '"' || oc == '\'')) {
				S v; if (!read_quoted_path(a, input, &pos, &v, &ps.err)) return ps;
				sel.tag = SEL_VALUE; sel.value = v; sel.quoted = 1; // quotes force a value match, even numeric - and scalar-only
			} else {
				size_t start = pos;
				while (pos < input.n) {
					uint32_t bc; size_t bl = utf8_decode(input.p, input.n, pos, &bc);
					if (bc == ']') break;
					pos += bl;
				}
				S body = s_trim(s_slice(input, start, pos));
				uint64_t idx;
				if (body.n == 1 && body.p[0] == '*') {
					sel.tag = SEL_WILDCARD;
				} else if (body.n >= 1 && body.p[0] == '#' && parse_u64(s_slice(body, 1, body.n), &idx)) {
					sel.tag = SEL_INDEX; sel.index = idx;
				} else if (parse_u64(body, &idx)) {
					sel.tag = SEL_INDEX; sel.index = idx;
				} else if (body.n == 0) {
					ps.err = s_lit("empty selector"); return ps;
				} else {
					sel.tag = SEL_VALUE; sel.value = norm_dangling(a, body);
				}
			}
			skip_ws_path(input, &pos);
			uint32_t cc = 0; size_t ccl = 0;
			if (pos < input.n) ccl = utf8_decode(input.p, input.n, pos, &cc);
			if (pos >= input.n || cc != ']') { ps.err = s_lit("unterminated selector"); return ps; }
			pos += ccl;
			skip_ws_path(input, &pos);
		}
		if (star && sel.tag != SEL_NONE) { ps.err = s_lit("selector on a name wildcard"); return ps; }
		Segment seg; seg.name = fold_name(a, name); seg.name_src = name; seg.sel = sel; seg.star = star;
		VecSeg_push(a, &ps.segs, seg);
		if (pos >= input.n) { ps.ok = 1; ps.has_value = 0; return ps; }
		uint32_t dc; size_t dl = utf8_decode(input.p, input.n, pos, &dc);
		if (dc == '.') { pos += dl; continue; }
		if (dc == ':') {
			pos += dl;
			ps.ok = 1; ps.has_value = 1;
			ps.value_text = s_dup(a, s_trim(s_slice(input, pos, input.n)));
			return ps;
		}
		{ SB e = {0}; sb_puts(a, &e, "unexpected '"); sb_put_cp(a, &e, dc); sb_puts(a, &e, "' after field"); ps.err = sb_S(&e); return ps; }
	}
}

static PathScan scan_path(Arena *a, S input) { return scan_path_ex(a, input, 0); }
// Query spelling of scan_path: also accepts a bare `*` segment (the name
// wildcard - any child name). Document lines never take it; only lookups
// (reads, the writer probe, schema paths) do.
static PathScan scan_lookup(Arena *a, S input) { return scan_path_ex(a, input, 1); }

// --- small integer/string helpers used below --------------------------------

static void sb_put_u64(Arena *a, SB *s, uint64_t v) {
	char t[24]; int j = 0;
	if (v == 0) t[j++] = '0';
	while (v) { t[j++] = (char)('0' + (v % 10)); v /= 10; }
	char o[24]; for (int k = 0; k < j; k++) o[k] = t[j - 1 - k];
	// cppcheck-suppress uninitvar  ## j >= 1 always (v==0 writes '0'), so o[0..j-1] is filled
	sb_put(a, s, o, (size_t)j);
}
static int s_contains_char(S s, char c) { for (size_t i = 0; i < s.n; i++) if (s.p[i] == c) return 1; return 0; }

typedef struct { S indent; size_t node; } StackEnt;
DEFINE_VEC(VecStack, StackEnt)
typedef struct { int present; size_t idx; shcl_status miss; } Slot;
DEFINE_VEC(VecSlot, Slot)

// --- coercion ("intelligent but safe"; Loose re-admits a closed list) --------

static const uint32_t SHCL_CURRENCY[] = {
	'$', 0xA2, 0xA3, 0xA4, 0xA5, 0x20A9, 0x20AA, 0x20AB, 0x20AC, 0x20AD,
	0x20AE, 0x20B1, 0x20B2, 0x20B4, 0x20B9, 0x20BA, 0x20BC, 0x20BD, 0x20BE, 0x20BF,
};
static S strip_currency(S t) {
	if (t.n == 0) return t;
	uint32_t c; size_t l = utf8_decode(t.p, t.n, 0, &c);
	for (size_t i = 0; i < sizeof(SHCL_CURRENCY) / sizeof(SHCL_CURRENCY[0]); i++)
		if (c == SHCL_CURRENCY[i]) return s_slice(t, l, t.n);
	return t;
}

// [+/-]digits, fully consumed, no overflow.
static int parse_i64_s(S t, int64_t *out) {
	size_t i = 0; int neg = 0;
	if (i < t.n && (t.p[i] == '+' || t.p[i] == '-')) { neg = (t.p[i] == '-'); i++; }
	if (i >= t.n) return 0;
	uint64_t v = 0; uint64_t lim = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
	for (; i < t.n; i++) {
		unsigned char c = (unsigned char)t.p[i];
		if (!is_adigit(c)) return 0;
		if (v > (lim - (c - '0')) / 10) return 0;
		v = v * 10 + (c - '0');
	}
	if (neg) *out = (v == (uint64_t)INT64_MAX + 1) ? INT64_MIN : -(int64_t)v;
	else *out = (int64_t)v;
	return 1;
}
// magnitude hex in [0, INT64_MAX]; overflow -> fail.
// The magnitude, as u64 (guarded against u64 overflow). The sign range-check is
// the caller's, so the negative i64_min magnitude (0x8000000000000000) reads.
static int parse_hex_u64(S h, uint64_t *out) {
	uint64_t v = 0;
	for (size_t i = 0; i < h.n; i++) {
		unsigned char c = (unsigned char)h.p[i]; int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else return 0;
		if (v > (UINT64_MAX - (uint64_t)d) / 16) return 0;
		v = v * 16 + (uint64_t)d;
	}
	*out = v; return 1;
}
static void split_byte(Arena *a, S s, char sep, VecS *out) {
	size_t start = 0;
	for (size_t i = 0; i <= s.n; i++)
		if (i == s.n || s.p[i] == sep) { VecS_push(a, out, s_slice(s, start, i)); start = i + 1; }
}

static int parse_int_text(Arena *a, const Element *e, shcl_strictness level, int64_t *out);
static int parse_float_text(Arena *a, const Element *e, shcl_strictness level, double *out);

static int float_shape_ok(S t) {
	S body = t;
	if (body.n > 0 && (body.p[0] == '+' || body.p[0] == '-')) body = s_slice(body, 1, body.n);
	if (body.n == 0) return 0;
	S mant = body, exp = s_empty(); int has_exp = 0;
	for (size_t i = 0; i < body.n; i++)
		if (body.p[i] == 'e' || body.p[i] == 'E') { mant = s_slice(body, 0, i); exp = s_slice(body, i + 1, body.n); has_exp = 1; break; }
	if (has_exp) {
		S xb = exp;
		if (xb.n > 0 && (xb.p[0] == '+' || xb.p[0] == '-')) xb = s_slice(xb, 1, xb.n);
		if (xb.n == 0 || !all_adigit0(xb)) return 0;
	}
	S ip = mant, fp = s_empty(); int has_dot = 0;
	for (size_t i = 0; i < mant.n; i++)
		if (mant.p[i] == '.') { ip = s_slice(mant, 0, i); fp = s_slice(mant, i + 1, mant.n); has_dot = 1; break; }
	(void)has_dot;
	if (ip.n == 0 && fp.n == 0) return 0;
	return all_adigit0(ip) && all_adigit0(fp);
}
static int strtod_full(Arena *a, S t, double *out) {
	const char *dp = dec_point(); size_t dn = strlen(dp);
	char *buf = (char *)arena_alloc(a, t.n * dn + 1);
	size_t j = 0;
	for (size_t i = 0; i < t.n; i++) {
		if (t.p[i] == '.') { memcpy(buf + j, dp, dn); j += dn; }
		else buf[j++] = t.p[i];
	}
	buf[j] = '\0';
	char *end; double v = strtod(buf, &end);
	if (end != buf + j) return 0;
	*out = v; return 1;
}
static int parse_float_text(Arena *a, const Element *e, shcl_strictness level, double *out) {
	S t = s_trim(e->text); int percent = 0;
	if (level == SHCL_LOOSE) {
		t = strip_currency(t);
		if (t.n > 0 && t.p[t.n - 1] == '%') { t = trim_end(s_slice(t, 0, t.n - 1)); percent = 1; }
	}
	double v;
	if (float_shape_ok(t)) {
		if (!strtod_full(a, t, &v)) return 0;
	} else {
		Element el; el.text = t; el.quoted = e->quoted;
		int64_t iv;
		if (!parse_int_text(a, &el, SHCL_STANDARD, &iv)) return 0;
		v = (double)iv;
	}
	*out = percent ? v / 100.0 : v; return 1;
}
static int parse_int_text(Arena *a, const Element *e, shcl_strictness level, int64_t *out) {
	S t = s_trim(e->text);
	if (level == SHCL_LOOSE) t = strip_currency(t);
	S body = t;
	if (body.n > 0 && (body.p[0] == '+' || body.p[0] == '-')) body = s_slice(body, 1, body.n);
	if (body.n > 0 && all_adigit0(body)) return parse_i64_s(t, out);
	int neg = 0; S hex = t;
	if (t.n > 0 && t.p[0] == '-') { neg = 1; hex = s_slice(t, 1, t.n); }
	else if (t.n > 0 && t.p[0] == '+') { hex = s_slice(t, 1, t.n); }
	if (s_starts(hex, "0x") || s_starts(hex, "0X")) {
		S h = s_slice(hex, 2, hex.n);
		if (all_ahex(h)) {
			uint64_t m; if (!parse_hex_u64(h, &m)) return 0;
			if (neg) {
				if (m == (uint64_t)INT64_MAX + 1) *out = INT64_MIN;
				else if (m <= (uint64_t)INT64_MAX) *out = -(int64_t)m;
				else return 0;
			} else {
				if (m <= (uint64_t)INT64_MAX) *out = (int64_t)m;
				else return 0;
			}
			return 1;
		}
	}
	if (e->quoted && s_contains_char(t, ',')) {
		S sign_body = t;
		if (sign_body.n > 0 && (sign_body.p[0] == '+' || sign_body.p[0] == '-')) sign_body = s_slice(sign_body, 1, sign_body.n);
		VecS groups = {0}; split_byte(a, sign_body, ',', &groups);
		int wf = groups.len > 1 && groups.data[0].n > 0 && groups.data[0].n <= 3 && all_adigit0(groups.data[0]);
		if (wf) for (size_t k = 1; k < groups.len; k++)
			if (groups.data[k].n != 3 || !all_adigit0(groups.data[k])) { wf = 0; break; }
		if (wf) {
			SB b = {0}; for (size_t i = 0; i < t.n; i++) if (t.p[i] != ',') sb_putc(a, &b, t.p[i]);
			return parse_i64_s(sb_S(&b), out);
		}
	}
	if (level == SHCL_LOOSE) {
		double f;
		if (parse_float_text(a, e, level, &f)) {
			double r = round(f);
			if (r >= -9223372036854775808.0 && r <= 9223372036854775808.0) {
				*out = (r >= 9223372036854775808.0) ? INT64_MAX : (int64_t)r;
				return 1;
			}
		}
	}
	return 0;
}
static int parse_bool_text(Arena *a, S t, shcl_strictness level, int *out) {
	S s = ascii_lower(a, s_trim(t));
	#define SHCL_EQ(z) (s.n == strlen(z) && memcmp(s.p, z, s.n) == 0)
	if (SHCL_EQ("true")) { *out = 1; return 1; }
	if (SHCL_EQ("false")) { *out = 0; return 1; }
	if (level == SHCL_STRICT) return 0;
	if (SHCL_EQ("yes") || SHCL_EQ("on") || SHCL_EQ("1")) { *out = 1; return 1; }
	if (SHCL_EQ("no") || SHCL_EQ("off") || SHCL_EQ("0")) { *out = 0; return 1; }
	if (level == SHCL_LOOSE) {
		if (SHCL_EQ("t") || SHCL_EQ("y") || SHCL_EQ("enable") || SHCL_EQ("enabled")) { *out = 1; return 1; }
		if (SHCL_EQ("f") || SHCL_EQ("n") || SHCL_EQ("disable") || SHCL_EQ("disabled")) { *out = 0; return 1; }
	}
	#undef SHCL_EQ
	return 0;
}

// --- date/time (closed whitelist; shape match, then calendar validation) -----

static uint32_t month_from_name(Arena *a, S s) {
	static const char *names[] = {
		"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec",
		"january","february","march","april","june","july","august","september",
		"october","november","december"
	};
	static const uint32_t vals[] = { 1,2,3,4,5,6,7,8,9,10,11,12, 1,2,3,4,6,7,8,9,10,11,12 };
	S l = ascii_lower(a, s);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (l.n == strlen(names[i]) && memcmp(l.p, names[i], l.n) == 0) return vals[i];
	return 0;
}
static uint32_t days_in_month(int32_t y, uint32_t m) {
	switch (m) {
	case 1: case 3: case 5: case 7: case 8: case 10: case 12: return 31;
	case 4: case 6: case 9: case 11: return 30;
	case 2: return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
	default: return 0;
	}
}
static int valid_date(int32_t y, uint32_t m, uint32_t d) {
	return m >= 1 && m <= 12 && d >= 1 && d <= days_in_month(y, m);
}
static int parse_num2(S s, uint32_t *out) {
	if (!(s.n == 1 || s.n == 2) || !all_adigit0(s)) return 0;
	uint32_t v = 0; for (size_t i = 0; i < s.n; i++) v = v * 10 + (s.p[i] - '0');
	*out = v; return 1;
}
static int parse_year4(S s, int32_t *out) {
	if (s.n != 4 || !all_adigit0(s)) return 0;
	int32_t v = 0; for (size_t i = 0; i < s.n; i++) v = v * 10 + (s.p[i] - '0');
	*out = v; return 1;
}
static int parse_u32_lenient(S s, uint32_t *out) {
	size_t i = 0; if (i < s.n && s.p[i] == '+') i++;
	if (i >= s.n) return 0;
	uint64_t v = 0;
	for (; i < s.n; i++) { unsigned char c = (unsigned char)s.p[i]; if (!is_adigit(c)) return 0; v = v * 10 + (c - '0'); if (v > 0xFFFFFFFFull) return 0; }
	*out = (uint32_t)v; return 1;
}
static void split_ws(Arena *a, S s, VecS *out) {
	size_t i = 0;
	while (i < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c);
		if (is_ws(c)) { i += l; continue; }
		size_t start = i;
		while (i < s.n) { uint32_t d; size_t l2 = utf8_decode(s.p, s.n, i, &d); if (is_ws(d)) break; i += l2; }
		VecS_push(a, out, s_slice(s, start, i));
	}
}
typedef struct { int ok; int32_t y; uint32_t m, d; } DatePart;
static DatePart parse_date_part(Arena *a, S s) {
	DatePart r; r.ok = 0; r.y = 0; r.m = 0; r.d = 0;
	s = s_trim(s);
	if (s.n == 8 && all_adigit0(s)) {
		int32_t y; uint32_t m, d;
		if (parse_year4(s_slice(s, 0, 4), &y) && parse_num2(s_slice(s, 4, 6), &m) && parse_num2(s_slice(s, 6, 8), &d) && valid_date(y, m, d)) { r.ok = 1; r.y = y; r.m = m; r.d = d; }
		return r;
	}
	VecS toks = {0}; split_ws(a, s, &toks);
	if (toks.len == 3) {
		uint32_t mm;
		if ((mm = month_from_name(a, toks.data[0]))) {
			S day_tok = toks.data[1];
			if (day_tok.n > 0 && day_tok.p[day_tok.n - 1] == ',') day_tok = s_slice(day_tok, 0, day_tok.n - 1);
			uint32_t d; int32_t y;
			if (parse_u32_lenient(day_tok, &d) && parse_year4(toks.data[2], &y) && valid_date(y, mm, d)) { r.ok = 1; r.y = y; r.m = mm; r.d = d; }
			return r;
		}
		if ((mm = month_from_name(a, toks.data[1]))) {
			uint32_t d; int32_t y;
			if (parse_u32_lenient(toks.data[0], &d) && parse_year4(toks.data[2], &y) && valid_date(y, mm, d)) { r.ok = 1; r.y = y; r.m = mm; r.d = d; }
			return r;
		}
		return r;
	}
	if (toks.len != 1) return r;
	char delim = 0; int have = 0;
	for (size_t i = 0; i < s.n; i++) if (s.p[i] == '-' || s.p[i] == '/' || s.p[i] == '.') { delim = s.p[i]; have = 1; break; }
	if (!have) return r;
	VecS parts = {0}; split_byte(a, s, delim, &parts);
	if (parts.len != 3) return r;
	for (size_t i = 0; i < parts.len; i++) if (parts.data[i].n == 0) return r;
	size_t dcount = 0; for (size_t i = 0; i < s.n; i++) if (s.p[i] == '-' || s.p[i] == '/' || s.p[i] == '.') dcount++;
	if (dcount != 2) return r;
	if (parts.data[0].n == 4 && all_adigit0(parts.data[0])) {
		int32_t y; uint32_t m, d;
		if (parse_year4(parts.data[0], &y) && parse_num2(parts.data[1], &m) && parse_num2(parts.data[2], &d) && valid_date(y, m, d)) { r.ok = 1; r.y = y; r.m = m; r.d = d; }
		return r;
	}
	uint32_t mm;
	if ((mm = month_from_name(a, parts.data[0]))) {
		uint32_t d; int32_t y;
		if (parse_num2(parts.data[1], &d) && parse_year4(parts.data[2], &y) && valid_date(y, mm, d)) { r.ok = 1; r.y = y; r.m = mm; r.d = d; }
		return r;
	}
	if ((mm = month_from_name(a, parts.data[1]))) {
		uint32_t d; int32_t y;
		if (parse_num2(parts.data[0], &d) && parse_year4(parts.data[2], &y) && valid_date(y, mm, d)) { r.ok = 1; r.y = y; r.m = mm; r.d = d; }
		return r;
	}
	return r;
}
typedef struct {
	int ok; uint32_t h, mi; int has_sec; uint32_t sec;
	int has_frac; S frac; shcl_zone_kind zone; int32_t off;
} TimePart;
static uint32_t low_a(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (uint32_t)(c + 32) : c; }
static TimePart parse_time_part(Arena *a, S s) {
	TimePart r; memset(&r, 0, sizeof r); r.zone = SHCL_ZONE_NONE;
	S t = s_trim(s);
	if (t.n > 0 && (t.p[t.n - 1] == 'Z' || t.p[t.n - 1] == 'z')) {
		r.zone = SHCL_ZONE_UTC; t = trim_end(s_slice(t, 0, t.n - 1));
	} else if (t.n >= 6 && ((unsigned char)t.p[t.n - 6] & 0xC0) != 0x80) {
		S tail = s_slice(t, t.n - 6, t.n);
		unsigned char sign = (unsigned char)tail.p[0];
		if ((sign == '+' || sign == '-') && is_adigit((unsigned char)tail.p[1]) && is_adigit((unsigned char)tail.p[2])
			&& tail.p[3] == ':' && is_adigit((unsigned char)tail.p[4]) && is_adigit((unsigned char)tail.p[5])) {
			int hh = (tail.p[1] - '0') * 10 + (tail.p[2] - '0');
			int mm = (tail.p[4] - '0') * 10 + (tail.p[5] - '0');
			if (hh <= 23 && mm <= 59) {
				int off = hh * 60 + mm; if (sign == '-') off = -off;
				r.zone = SHCL_ZONE_OFFSET; r.off = off;
				t = trim_end(s_slice(t, 0, t.n - 6));
			}
		}
	}
	int meridiem = -1; // 0 = AM, 1 = PM
	if (t.n >= 2 && low_a((unsigned char)t.p[t.n - 1]) == 'm' && low_a((unsigned char)t.p[t.n - 2]) == 'a') { meridiem = 0; t = s_slice(t, 0, t.n - 2); }
	else if (t.n >= 2 && low_a((unsigned char)t.p[t.n - 1]) == 'm' && low_a((unsigned char)t.p[t.n - 2]) == 'p') { meridiem = 1; t = s_slice(t, 0, t.n - 2); }
	t = trim_end(t);
	S hms = t; S frac = s_empty(); int has_frac = 0;
	for (size_t i = 0; i < t.n; i++) if (t.p[i] == '.') {
		hms = s_slice(t, 0, i); S f = s_slice(t, i + 1, t.n);
		if (f.n == 0 || f.n > 9 || !all_adigit0(f)) return r;
		frac = f; has_frac = 1; break;
	}
	VecS parts = {0}; split_byte(a, hms, ':', &parts);
	if (parts.len < 2 || parts.len > 3) return r;
	if (has_frac && parts.len != 3) return r;
	uint32_t h_raw, mi;
	if (!parse_num2(parts.data[0], &h_raw)) return r;
	if (parts.data[1].n != 2 || !parse_num2(parts.data[1], &mi)) return r;
	int has_sec = 0; uint32_t sec = 0;
	if (parts.len == 3) { if (parts.data[2].n != 2 || !parse_num2(parts.data[2], &sec)) return r; has_sec = 1; }
	if (mi > 59 || (has_sec && sec > 59)) return r;
	uint32_t h;
	if (meridiem == -1) { if (h_raw > 23) return r; h = h_raw; }
	else {
		if (h_raw < 1 || h_raw > 12) return r;
		if (meridiem == 0) h = (h_raw == 12) ? 0 : h_raw;
		else h = (h_raw == 12) ? 12 : h_raw + 12;
	}
	r.ok = 1; r.h = h; r.mi = mi; r.has_sec = has_sec; r.sec = sec;
	r.has_frac = has_frac; r.frac = frac;
	return r;
}
static int parse_datetime(Arena *a, S text, shcl_datetime *out) {
	memset(out, 0, sizeof *out); out->zone = SHCL_ZONE_NONE;
	S t = s_trim(text);
	if (t.n == 0) return 0;
	size_t colon = (size_t)-1;
	for (size_t i = 0; i < t.n; i++) if (t.p[i] == ':') { colon = i; break; }
	if (colon != (size_t)-1) {
		size_t k = colon;
		while (k > 0 && is_adigit((unsigned char)t.p[k - 1]) && colon - k < 2) k--;
		if (k == colon) return 0;
		if (k == 0) {
			TimePart tp = parse_time_part(a, t);
			if (!tp.ok) return 0;
			out->has_time = 1; out->hour = tp.h; out->minute = tp.mi;
			out->has_sec = tp.has_sec; out->sec = tp.sec;
			out->has_frac = tp.has_frac; out->frac = tp.frac; out->zone = tp.zone; out->off_min = tp.off;
			return 1;
		}
		uint32_t sepc; size_t sep_len = utf8_last(s_slice(t, 0, k), &sepc);
		if (!(sepc == 'T' || sepc == 't' || sepc == ' ' || sepc == '_' || sepc == '-' || sepc == '/' || sepc == '.')) return 0;
		DatePart dp = parse_date_part(a, s_slice(t, 0, k - sep_len));
		if (!dp.ok) return 0;
		TimePart tp = parse_time_part(a, s_slice(t, k, t.n));
		if (!tp.ok) return 0;
		out->has_date = 1; out->year = dp.y; out->month = dp.m; out->day = dp.d;
		out->has_time = 1; out->hour = tp.h; out->minute = tp.mi;
		out->has_sec = tp.has_sec; out->sec = tp.sec;
		out->has_frac = tp.has_frac; out->frac = tp.frac; out->zone = tp.zone; out->off_min = tp.off;
		return 1;
	}
	DatePart dp = parse_date_part(a, t);
	if (!dp.ok) return 0;
	out->has_date = 1; out->year = dp.y; out->month = dp.m; out->day = dp.d;
	return 1;
}

// --- parser ------------------------------------------------------------------

/* Per-node (name, value-key) -> first matching child. Pure lookup accelerator
   for select_or_create (the linear scan was O(children^2) per parent); the
   children vec keeps the order. Chained buckets, entries arena-allocated; a
   value that mutates in place (empty field filled, star element added) moves
   its entry via remap_child. */
typedef struct CMapEnt { struct CMapEnt *next; uint64_t hash; S name; S key; size_t val; } CMapEnt;
typedef struct { CMapEnt **buckets; size_t cap, len; } CMap;
DEFINE_VEC(VecMap, CMap)

static uint64_t cmap_hash(S name, S key) {
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < name.n; i++) { h ^= (unsigned char)name.p[i]; h *= 1099511628211ull; }
	h ^= 0xFFu; h *= 1099511628211ull; /* separator; equality still compares both parts */
	for (size_t i = 0; i < key.n; i++) { h ^= (unsigned char)key.p[i]; h *= 1099511628211ull; }
	return h;
}
static size_t cmap_get(const CMap *m, uint64_t h, S name, S key) {
	if (!m->cap) return (size_t)-1;
	for (CMapEnt *e = m->buckets[h & (m->cap - 1)]; e; e = e->next)
		if (e->hash == h && s_eq(e->name, name) && s_eq(e->key, key)) return e->val;
	return (size_t)-1;
}
static void cmap_put(Arena *a, CMap *m, uint64_t h, S name, S key, size_t val) {
	if (m->len + 1 > m->cap - m->cap / 4) { /* grow at 75%; also covers cap 0 */
		size_t nc = m->cap ? m->cap * 2 : 8;
		CMapEnt **nb = (CMapEnt **)arena_alloc(a, nc * sizeof(CMapEnt *));
		memset(nb, 0, nc * sizeof(CMapEnt *));
		for (size_t b = 0; b < m->cap; b++)
			for (CMapEnt *e = m->buckets[b], *nx; e; e = nx) {
				nx = e->next;
				size_t d = e->hash & (nc - 1);
				e->next = nb[d]; nb[d] = e;
			}
		m->buckets = nb; m->cap = nc;
	}
	CMapEnt *e = (CMapEnt *)arena_alloc(a, sizeof *e);
	e->hash = h; e->name = name; e->key = key; e->val = val;
	size_t b = h & (m->cap - 1);
	e->next = m->buckets[b]; m->buckets[b] = e;
	m->len++;
}
/* Unlink the (name, key) entry, but only if it points at this node. */
static void cmap_del(CMap *m, uint64_t h, S name, S key, size_t val) {
	if (!m->cap) return;
	for (CMapEnt **pp = &m->buckets[h & (m->cap - 1)]; *pp; pp = &(*pp)->next) {
		CMapEnt *e = *pp;
		if (e->hash == h && e->val == val && s_eq(e->name, name) && s_eq(e->key, key)) { *pp = e->next; m->len--; return; }
	}
}

/* A pending whole-line comment during parse: text, source indent (used only
   to decide whether it hangs on a deeper block), and the blank it consumed. */
typedef struct { S text; S indent; int blank_before; } Pend;
DEFINE_VEC(VecPend, Pend)

/* pending: whole-line comments waiting for the next line that binds a node.
   The source indent is kept only to decide after-attachment (a comment deeper
   than the next binding hangs on the block it sits in).
   star_*: a stacked list defers its merge-key remap while it is the open field
   (rebuilding the key per element is O(list^2) time and arena garbage); the
   deferred remap flushes before any other map lookup. */
/* dmaps: per-node (name, display) -> first matching child - the `[value]`
   selector accelerator (display() is a different, non-injective predicate from
   cmaps' merge key). Same first-wins discipline, same mutation sites. */
// reent_node/reent_line pair up node -> line of the re-open that H002-hinted
// it (linear scan; re-opens are rare). A merge under a hinted container
// combines the same two textual regions, so it hints too even when it lands on
// the newest child at its own scope - that is how every merged level reports,
// not just the outermost. The stored line splits old children (hint) from ones
// the re-opened region itself created (silent).
typedef struct { shcl_doc *d; VecStack stack; VecMap cmaps; VecMap dmaps; VecPend pending; int star_open; size_t star_node; S star_key; S star_disp; int saw_blank; VecSize reent_node; VecSize reent_line; } Parser;

// The one place prose couples to a code, so the wording stays free everywhere else.
static const char *diag_code(shcl_severity sev, S msg) {
	if (s_starts(msg, "merged with ")) return "H002";
	if (sev == SHCL_SEV_HINT) return "H001"; // repeated bare leaf
	if (s_starts(msg, "field mixed with list elements")) return "E001";
	if (s_starts(msg, "value after selector on ")) return "E002";
	if (s_starts(msg, "no instance ")) return "E003";
	if (s_starts(msg, "wildcard selector is query-only")) return "E004";
	if (s_starts(msg, "unterminated raw block")) return "E005";
	if (s_starts(msg, "raw block with no parent field")) return "E006";
	if (s_starts(msg, "list element with no parent field")) return "E007";
	if (s_starts(msg, "list element mixed with field children")) return "E008";
	if (s_starts(msg, "empty list element")) return "E009";
	if (s_starts(msg, "bare comma in list element")) return "E010";
	if (s_starts(msg, "field already has a value")) return "E011";
	if (s_starts(msg, "indentation matches no open level")) return "E012";
	if (s_starts(msg, "malformed line skipped")) return "E014";
	if (s_starts(msg, "malformed line: ")) return "E013";
	if (s_starts(msg, "missing colon")) return "E015";
	if (s_starts(msg, "nesting deeper than")) return "E016";
	if (s_starts(msg, "unterminated quote in value")) return "E017";
	if (s_starts(msg, "unknown field ")) return "V001";
	if (s_starts(msg, "required path missing")) return "V002";
	if (s_starts(msg, "wrong type at ")) return "V003";
	if (s_starts(msg, "value not allowed at ")) return "V004";
	if (s_starts(msg, "value below min at ")) return "V005";
	if (s_starts(msg, "value above max at ")) return "V006";
	if (s_starts(msg, "instance count out of bounds at ")) return "V007";
	if (s_starts(msg, "unknown schema key ")) return "V090";
	if (s_starts(msg, "unknown schema type ")) return "V091";
	if (s_starts(msg, "bad schema constraint ")) return "V092";
	if (s_starts(msg, "bad schema path")) return "V093";
	if (s_starts(msg, "bad schema fragment")) return "V094";
	if (s_starts(msg, "unknown schema fragment ")) return "V095";
	if (s_starts(msg, "schema expands past ")) return "V096";
	if (s_starts(msg, "schema failed to load")) return "V099";
	return "E000";
}
static void push_diag(shcl_doc *d, size_t line, shcl_severity sev, S msg) {
	Diag dg; dg.line = line; dg.sev = sev; dg.message = msg; dg.code = diag_code(sev, msg);
	VecDiag_push(&d->arena, &d->diags, dg);
}
static void p_err(Parser *P, size_t line, S msg) { push_diag(P->d, line, SHCL_SEV_ERROR, msg); }

static void remap_child(Parser *P, size_t node, S old_key, S old_disp);

/* Apply a stacked list's deferred merge-key remap. Runs before any map lookup
   (and at end of parse), so the map is always fresh when queried. */
static void star_flush(Parser *P) {
	if (!P->star_open) return;
	P->star_open = 0;
	remap_child(P, P->star_node, P->star_key, P->star_disp);
}

static size_t select_or_create(Parser *P, size_t parent, S name, S name_src, Value value, size_t line) {
	Arena *a = &P->d->arena;
	star_flush(P);
	S key = value_key(a, &value);
	uint64_t h = cmap_hash(name, key);
	size_t found = cmap_get(&P->cmaps.data[parent], h, name, key);
	if (found != (size_t)-1) return found;
	size_t idx = P->d->nodes.len;
	Node n; memset(&n, 0, sizeof n);
	n.name = s_dup(a, name); n.name_src = s_dup(a, name_src); n.value = value; n.parent = parent; n.line = line; n.star_list = 0; n.star_mixed = 0;
	VecNode_push(a, &P->d->nodes, n);
	VecSize_push(a, &NODE(P->d, parent).children, idx);
	CMap empty; memset(&empty, 0, sizeof empty);
	VecMap_push(a, &P->cmaps, empty);
	CMap dempty; memset(&dempty, 0, sizeof dempty);
	VecMap_push(a, &P->dmaps, dempty);
	/* store the arena-owned name; the caller's may point into the input buffer */
	cmap_put(a, &P->cmaps.data[parent], h, NODE(P->d, idx).name, key, idx);
	S disp = disp_key(a, &NODE(P->d, idx).value);
	uint64_t hd = cmap_hash(NODE(P->d, idx).name, disp);
	if (cmap_get(&P->dmaps.data[parent], hd, NODE(P->d, idx).name, disp) == (size_t)-1)
		cmap_put(a, &P->dmaps.data[parent], hd, NODE(P->d, idx).name, disp, idx);
	return idx;
}

/* A node's value mutated in place: move its map entry from the old key to the
   new one. First-wins on both sides so lookups keep matching the earliest
   sibling, like the scan did. */
static void remap_child(Parser *P, size_t node, S old_key, S old_disp) {
	Arena *a = &P->d->arena;
	size_t parent = NODE(P->d, node).parent;
	S name = NODE(P->d, node).name;
	CMap *m = &P->cmaps.data[parent];
	cmap_del(m, cmap_hash(name, old_key), name, old_key, node);
	S new_key = value_key(a, &NODE(P->d, node).value);
	uint64_t h = cmap_hash(name, new_key);
	if (cmap_get(m, h, name, new_key) == (size_t)-1) cmap_put(a, m, h, name, new_key, node);
	CMap *dm = &P->dmaps.data[parent];
	cmap_del(dm, cmap_hash(name, old_disp), name, old_disp, node);
	S new_disp = disp_key(a, &NODE(P->d, node).value);
	uint64_t hd = cmap_hash(name, new_disp);
	if (cmap_get(dm, hd, name, new_disp) == (size_t)-1) cmap_put(a, dm, hd, name, new_disp, node);
}

/* A value that mutates after its sibling group was keyed - an empty field
   filled by a fence, a stacked list closed - can land on a key an earlier
   sibling already holds, which the keyed lookup can no longer catch. Fold
   those pairs so the tree matches a reparse of its own canonical text.
   Depth-first, since folding can carry duplicates down a level. Grouping
   temporaries live in scratch (dead before the first resolve resets it). */
static void fold_late_dups(Parser *P) {
	shcl_doc *d = P->d;
	Arena *t = &d->scratch;
	VecSize stack = {0};
	VecSize_push(t, &stack, ROOT);
	while (stack.len) {
		size_t parent = stack.data[--stack.len];
		CMap first; memset(&first, 0, sizeof first);
		VecSize *ch = &NODE(d, parent).children;
		size_t w = 0;
		for (size_t k = 0; k < ch->len; k++) {
			size_t c = ch->data[k];
			S key = value_key(t, &NODE(d, c).value);
			uint64_t h = cmap_hash(NODE(d, c).name, key);
			size_t survivor = cmap_get(&first, h, NODE(d, c).name, key);
			if (survivor != (size_t)-1) fold_node_into(d, survivor, c);
			else {
				cmap_put(t, &first, h, NODE(d, c).name, key, c);
				ch->data[w++] = c;
			}
		}
		ch->len = w;
		for (size_t k = 0; k < ch->len; k++) VecSize_push(t, &stack, ch->data[k]);
	}
}

/* Hand pending leading comments (and this line's trailing one) to a node.
   First trailing wins; a later one demotes to leading so nothing is lost.
   Pending text was already dup'd into the arena at push time (the caller's
   text buffer may not outlive the document); the trailing is dup'd here. */
static void attach_trivia(Parser *P, size_t node, S trailing) {
	Arena *a = &P->d->arena;
	for (size_t k = 0; k < P->pending.len; k++) {
		Pend *p = &P->pending.data[k];
		VecLead_push(a, &NODE(P->d, node).leading, lead_make(p->text, p->blank_before));
	}
	P->pending.len = 0;
	if (trailing.n) {
		S t = s_dup(a, trailing);
		if (NODE(P->d, node).trailing.n == 0) NODE(P->d, node).trailing = t;
		else VecLead_push(a, &NODE(P->d, node).leading, lead_plain(t));
	}
}

/* Comments written deeper than the incoming line belong to the block they sit
   in, not to the next binding: hang each on the deepest node whose bound
   indent prefixes the comment's, among the levels the incoming line is
   closing. Written at that node's own level the comment trails it (`after`);
   written deeper it sits inside the node's block (`inside`) - so a header
   whose children are all commented still owns them at their depth. Runs
   before the incoming line resolves (and at end of parse with the empty
   indent, so tail comments keep their block). */
static void hang_deeper_pending(Parser *P, S new_indent) {
	if (P->pending.len == 0) return;
	Arena *a = &P->d->arena;
	size_t w = 0;
	for (size_t k = 0; k < P->pending.len; k++) {
		Pend p = P->pending.data[k];
		if (p.indent.n > new_indent.n) {
			/* A level shallower than the incoming line stays open and may
			   still gain children, so a comment must not hang there - it
			   would emit below the child; keep it pending instead. */
			size_t target = (size_t)-1; int at_own_level = 0;
			for (size_t ii = P->stack.len; ii-- > 0;) {
				S ind = P->stack.data[ii].indent; size_t n = P->stack.data[ii].node;
				if (n != ROOT && ind.n >= new_indent.n && p.indent.n >= ind.n && memcmp(p.indent.p, ind.p, ind.n) == 0) { target = n; at_own_level = ind.n == p.indent.n; break; }
			}
			if (target != (size_t)-1) {
				Lead lead = lead_make(p.text, p.blank_before);
				if (at_own_level) VecLead_push(a, &NODE(P->d, target).after, lead);
				else VecLead_push(a, &NODE(P->d, target).inside, lead);
				continue;
			}
		}
		P->pending.data[w++] = p;
	}
	P->pending.len = w;
}

static int resolve_parent(Parser *P, S indent, size_t *out) {
	size_t top = P->stack.len - 1;
	S ti = P->stack.data[top].indent; size_t tn = P->stack.data[top].node;
	if (indent.n > ti.n && (ti.n == 0 || memcmp(indent.p, ti.p, ti.n) == 0)) { *out = tn; return 1; }
	for (size_t ii = P->stack.len; ii-- > 0;) {
		if (s_eq(P->stack.data[ii].indent, indent)) {
			*out = (ii == 0) ? ROOT : P->stack.data[ii - 1].node;
			P->stack.len = ii ? ii : 1;
			return 1;
		}
	}
	return 0;
}

/* The single H002 wording site: the merge hint and the schema suppressor
   both come here, same discipline as h001_head. */
static S h002_head(Arena *a, S name) {
	SB s = {0};
	sb_puts(a, &s, "merged with '"); sb_putS(a, &s, name); sb_puts(a, &s, "' at ");
	return sb_S(&s);
}
static size_t reent_get(const Parser *P, size_t node) {
	for (size_t i = 0; i < P->reent_node.len; i++)
		if (P->reent_node.data[i] == node) return P->reent_line.data[i];
	return 0;
}
static void reent_set(Parser *P, size_t node, size_t line) {
	Arena *a = &P->d->arena;
	for (size_t i = 0; i < P->reent_node.len; i++)
		if (P->reent_node.data[i] == node) { P->reent_line.data[i] = line; return; }
	VecSize_push(a, &P->reent_node, node); VecSize_push(a, &P->reent_line, line);
}
static int attach_path(Parser *P, size_t parent, Segment *segs, size_t nsegs, Value value, size_t line, size_t *out) {
	Arena *a = &P->d->arena;
	/* Field child under a stacked list: diagnose the mix once, keep the field. */
	star_flush(P);
	if (NODE(P->d, parent).star_list && !NODE(P->d, parent).star_mixed) {
		NODE(P->d, parent).star_mixed = 1;
		p_err(P, line, s_lit("field mixed with list elements"));
	}
	/* Nesting cap: parent depth plus the segments this line adds. Checked
	   before any node is created so a rejected line leaves nothing behind. */
	size_t parent_depth = 0;
	for (size_t up = parent; up != ROOT; up = NODE(P->d, up).parent) parent_depth++;
	if (parent_depth + nsegs > SHCL_MAX_DEPTH) {
		SB m = {0}; sb_puts(a, &m, "nesting deeper than "); sb_put_u64(a, &m, SHCL_MAX_DEPTH); sb_puts(a, &m, " levels; line skipped");
		p_err(P, line, sb_S(&m));
		P->d->lost++;
		return 0;
	}
	size_t cur = parent;
	for (size_t i = 0; i < nsegs; i++) {
		Segment *seg = &segs[i];
		int is_last = (i + 1 == nsegs);
		switch (seg->sel.tag) {
		case SEL_VALUE: {
			/* Same escape-applied display predicate resolution uses, so a
			   selector also selects an array-valued instance instead of
			   creating a spurious second one - via the dmaps accelerator (the
			   inline spelling was quadratic in siblings without it). Create
			   only when nothing matches. */
			S want = apply_escapes(a, seg->sel.value);
			uint64_t hd = cmap_hash(seg->name, want);
			size_t found = cmap_get(&P->dmaps.data[cur], hd, seg->name, want);
			/* A quoted selector is scalar-only, and the accelerator keeps just
			   the first same-display child - a later remap can drop an entry a
			   different sibling still satisfies - so a non-scalar hit and an
			   outright miss both fall to the (rare) fallback scan. */
			if (found != (size_t)-1 && seg->sel.quoted && !single_scalar(&NODE(P->d, found).value)) {
				found = (size_t)-1;
			}
			if (found == (size_t)-1 && seg->sel.quoted) {
				VecSize ch = NODE(P->d, cur).children;
				for (size_t k = 0; k < ch.len; k++) {
					size_t c = ch.data[k];
					if (s_eq(NODE(P->d, c).name, seg->name) && single_scalar(&NODE(P->d, c).value) && s_eq(disp_key(a, &NODE(P->d, c).value), want)) { found = c; break; }
				}
			}
			if (found != (size_t)-1) {
				cur = found;
			} else {
				Value disc; memset(&disc, 0, sizeof disc); disc.kind = V_CELL;
				Element *e = (Element *)arena_alloc(a, sizeof(Element));
				e->text = s_dup(a, seg->sel.value); e->quoted = 0;
				disc.els = e; disc.nels = 1;
				cur = select_or_create(P, cur, seg->name, seg->name_src, disc, line);
			}
			if (is_last && !v_is_empty(&value)) {
				SB m = {0}; sb_puts(a, &m, "value after selector on '"); sb_putS(a, &m, seg->name); sb_puts(a, &m, "' ignored");
				p_err(P, line, sb_S(&m));
				P->d->lost++;
			}
			break;
		}
		case SEL_INDEX: {
			VecSize matches = {0}; VecSize ch = NODE(P->d, cur).children;
			for (size_t k = 0; k < ch.len; k++) { size_t c = ch.data[k]; if (s_eq(NODE(P->d, c).name, seg->name)) VecSize_push(a, &matches, c); }
			if (seg->sel.index < matches.len) cur = matches.data[seg->sel.index];
			else {
				SB m = {0}; sb_puts(a, &m, "no instance "); sb_put_u64(a, &m, seg->sel.index); sb_puts(a, &m, " of '"); sb_putS(a, &m, seg->name); sb_putc(a, &m, '\'');
				p_err(P, line, sb_S(&m)); P->d->lost++; return 0;
			}
			break;
		}
		case SEL_WILDCARD:
			p_err(P, line, s_lit("wildcard selector is query-only")); P->d->lost++; return 0;
		case SEL_NONE: {
			size_t seg_parent = cur;
			size_t before = P->d->nodes.len;
			cur = select_or_create(P, cur, seg->name, seg->name_src, is_last ? value : v_empty(), line);
			/* Two separately-written bindings just combined: legal (the
			   merge rule), but only the parser can see it happened, so
			   say so. Adjacent re-mentions (still the newest binding at
			   this scope) and selector/path-intermediate merges stay
			   silent - those are the deliberate redundant-path idiom.
			   Under a hinted container the newest-child pass does not
			   apply to children the earlier region wrote: those merges
			   combine the same two regions, so every level reports. */
			if (is_last && cur < before && NODE(P->d, cur).line != line) {
				VecSize sib = NODE(P->d, seg_parent).children;
				int non_last = (sib.len == 0 || sib.data[sib.len - 1] != cur);
				size_t rl = reent_get(P, seg_parent);
				int cross_region = (rl != 0 && NODE(P->d, cur).line < rl);
				if (non_last || cross_region) {
					SB m = {0};
					sb_putS(a, &m, h002_head(a, seg->name));
					sb_puts(a, &m, "line "); sb_put_u64(a, &m, NODE(P->d, cur).line);
					sb_puts(a, &m, " (same name and value combine)");
					push_diag(P->d, line, SHCL_SEV_HINT, sb_S(&m));
					reent_set(P, cur, line);
				}
			}
			break;
		}
		}
	}
	*out = cur; return 1;
}

static Value consume_raw(Parser *P, S *lines, size_t nlines, size_t i, size_t open_line, unsigned char ch, size_t len, S info, size_t *next) {
	Arena *a = &P->d->arena;
	VecS content = {0}; int closed = 0;
	while (i < nlines) {
		if (is_fence_close(lines[i], ch, len)) { closed = 1; i++; break; }
		VecS_push(a, &content, lines[i]); i++;
	}
	if (!closed) p_err(P, open_line, s_lit("unterminated raw block"));
	int have_common = 0; S common = s_empty();
	for (size_t k = 0; k < content.len; k++) {
		S l = content.data[k];
		if (s_trim(l).n == 0) continue;
		size_t j = 0; while (j < l.n && (l.p[j] == ' ' || l.p[j] == '\t')) j++;
		S lead = s_slice(l, 0, j);
		if (!have_common) { common = lead; have_common = 1; }
		else { size_t m = 0; while (m < common.n && m < lead.n && common.p[m] == lead.p[m]) m++; common = s_slice(common, 0, m); }
	}
	SB out = {0};
	for (size_t k = 0; k < content.len; k++) {
		if (k) sb_putc(a, &out, '\n');
		S l = content.data[k];
		if (s_trim(l).n == 0) { /* blank -> "" */ }
		else if (common.n > 0 && l.n >= common.n && memcmp(l.p, common.p, common.n) == 0) sb_putS(a, &out, s_slice(l, common.n, l.n));
		else sb_putS(a, &out, l);
	}
	Value v; memset(&v, 0, sizeof v);
	v.kind = V_RAW; v.content = sb_S(&out); v.info = info; v.fence_char = ch; v.fence_len = len;
	*next = i;
	return v;
}

/* Returns the node the block landed on ((size_t)-1 = no parent, diagnosed). */
static size_t bind_block(Parser *P, size_t parent, Value value, size_t line) {
	if (parent == ROOT) { p_err(P, line, s_lit("raw block with no parent field")); P->d->lost++; return (size_t)-1; }
	if (v_is_empty(&NODE(P->d, parent).value)) {
		S old_key = value_key(&P->d->arena, &NODE(P->d, parent).value);
		S old_disp = disp_key(&P->d->arena, &NODE(P->d, parent).value);
		NODE(P->d, parent).value = value;
		remap_child(P, parent, old_key, old_disp);
		return parent;
	}
	S name = NODE(P->d, parent).name; S name_src = NODE(P->d, parent).name_src; size_t gp = NODE(P->d, parent).parent;
	return select_or_create(P, gp, name, name_src, value, line);
}

static void add_star_element(Parser *P, size_t parent, S body, size_t line) {
	Arena *a = &P->d->arena;
	if (parent == ROOT) { p_err(P, line, s_lit("list element with no parent field")); P->d->lost++; return; }
	/* Uniform-or-nothing (spec): a mix with field children is not a block array. */
	if (NODE(P->d, parent).children.len != 0) { p_err(P, line, s_lit("list element mixed with field children; ignored")); P->d->lost++; return; }
	S trimmed = s_trim(body);
	if (trimmed.n == 0) { p_err(P, line, s_lit("empty list element")); P->d->lost++; return; }
	if (count_unquoted_pieces(trimmed) > 1) { p_err(P, line, s_lit("bare comma in list element (one element per line)")); P->d->lost++; return; }
	if (unterminated_quote(a, trimmed)) p_err(P, line, s_lit("unterminated quote in value"));
	Element el;
	if (!parse_element(a, trimmed, &el)) { p_err(P, line, s_lit("empty list element")); P->d->lost++; return; }
	Node *node = &NODE(P->d, parent);
	if (node->value.kind == V_EMPTY) {
		S old_key = value_key(a, &node->value);
		S old_disp = disp_key(a, &node->value);
		/* Seed capacity for geometric growth: a fresh full-size copy per `* `
		   line kept every discarded copy in the arena - quadratic memory. */
		Element *arr = (Element *)arena_alloc(a, 4 * sizeof(Element)); arr[0] = el;
		node->value.kind = V_CELL; node->value.els = arr; node->value.nels = 1; node->value.cap_els = 4;
		node->star_list = 1;
		remap_child(P, parent, old_key, old_disp);
		/* Defer further remaps until the list closes; the map entry made above
		   stays valid because nothing can look this node up until a non-star
		   line binds (which flushes first). */
		P->star_open = 1; P->star_node = parent; P->star_key = value_key(a, &node->value); P->star_disp = disp_key(a, &node->value);
	} else if (node->value.kind == V_CELL && node->star_list) {
		if (!P->star_open || P->star_node != parent) {
			star_flush(P);
			P->star_open = 1; P->star_node = parent; P->star_key = value_key(a, &node->value); P->star_disp = disp_key(a, &node->value);
		}
		if (node->value.nels == node->value.cap_els) {
			size_t nc = node->value.cap_els ? node->value.cap_els * 2 : 4;
			Element *arr = (Element *)arena_alloc(a, nc * sizeof(Element));
			memcpy(arr, node->value.els, node->value.nels * sizeof(Element));
			node->value.els = arr; node->value.cap_els = nc;
		}
		node->value.els[node->value.nels++] = el;
	} else {
		p_err(P, line, s_lit("field already has a value; list element ignored"));
		P->d->lost++;
	}
}

/* The single H001 wording site: the hint builder and the schema suppressor
   both come here, so the suppressor matches the exact head the builder
   emitted - never a re-parse of free prose. (The leaf name cannot ride on
   the diagnostic itself: consumers build diagnostics literally, so the field
   set is frozen.) */
static S h001_head(Arena *a, S name) {
	SB s = {0};
	sb_putc(a, &s, '\'');
	sb_putS(a, &s, name);
	sb_puts(a, &s, "' repeats as a bare leaf - did you mean '");
	sb_putS(a, &s, name);
	sb_puts(a, &s, ": ");
	return sb_S(&s);
}

static void emit_repeated_leaf_hints(Parser *P) {
	Arena *a = &P->d->arena;
	for (size_t parent = 0; parent < P->d->nodes.len; parent++) {
		VecS names = {0}; VecSize *groups = NULL; size_t ngroups = 0, cgroups = 0;
		CMap group_of; memset(&group_of, 0, sizeof group_of);
		VecSize ch = NODE(P->d, parent).children;
		for (size_t k = 0; k < ch.len; k++) {
			size_t c = ch.data[k]; S nm = NODE(P->d, c).name;
			uint64_t h = cmap_hash(nm, s_empty());
			size_t g = cmap_get(&group_of, h, nm, s_empty());
			if (g == (size_t)-1) {
				VecS_push(a, &names, nm);
				if (ngroups == cgroups) { size_t nc = cgroups ? cgroups * 2 : 8; groups = (VecSize *)arena_grow(a, groups, cgroups, nc, sizeof(VecSize)); cgroups = nc; }
				memset(&groups[ngroups], 0, sizeof(VecSize)); g = ngroups++;
				cmap_put(a, &group_of, h, nm, s_empty(), g);
			}
			VecSize_push(a, &groups[g], c);
		}
		for (size_t gi = 0; gi < ngroups; gi++) {
			VecSize grp = groups[gi];
			if (grp.len < 2) continue;
			int all_scalar = 1; size_t maxline = 0;
			for (size_t k = 0; k < grp.len; k++) {
				size_t c = grp.data[k];
				if (!(NODE(P->d, c).children.len == 0 && NODE(P->d, c).value.kind == V_CELL && !NODE(P->d, c).star_list)) { all_scalar = 0; break; }
				if (NODE(P->d, c).line > maxline) maxline = NODE(P->d, c).line;
			}
			if (!all_scalar) continue;
			SB joined = {0};
			for (size_t k = 0; k < grp.len; k++) { if (k) sb_puts(a, &joined, ", "); sb_putS(a, &joined, value_display(a, &NODE(P->d, grp.data[k]).value)); }
			SB m = {0}; sb_putS(a, &m, h001_head(a, names.data[gi])); sb_putS(a, &m, sb_S(&joined)); sb_puts(a, &m, "'?");
			push_diag(P->d, maxline, SHCL_SEV_HINT, sb_S(&m));
		}
	}
}

static shcl_doc *do_parse(const char *text, size_t len, shcl_strictness strict) {
	shcl_doc *d = (shcl_doc *)calloc(1, sizeof *d);
	if (!d) { fprintf(stderr, "shcl: out of memory\n"); exit(70); }
	d->strictness = strict;
	Arena *a = &d->arena;
	Node root; memset(&root, 0, sizeof root); root.value = v_empty(); root.parent = 0; root.line = 0;
	VecNode_push(a, &d->nodes, root);
	Parser P; P.d = d; memset(&P.stack, 0, sizeof P.stack); memset(&P.cmaps, 0, sizeof P.cmaps); memset(&P.dmaps, 0, sizeof P.dmaps); memset(&P.pending, 0, sizeof P.pending);
	P.star_open = 0; P.star_node = 0; P.star_key = s_empty(); P.star_disp = s_empty(); P.saw_blank = 0;
	memset(&P.reent_node, 0, sizeof P.reent_node); memset(&P.reent_line, 0, sizeof P.reent_line);
	StackEnt e0; e0.indent = s_empty(); e0.node = ROOT; VecStack_push(a, &P.stack, e0);
	CMap m0; memset(&m0, 0, sizeof m0); VecMap_push(a, &P.cmaps, m0);
	CMap d0; memset(&d0, 0, sizeof d0); VecMap_push(a, &P.dmaps, d0);

	S full; full.p = text ? text : ""; full.n = len;
	if (full.n >= 3 && (unsigned char)full.p[0] == 0xEF && (unsigned char)full.p[1] == 0xBB && (unsigned char)full.p[2] == 0xBF) full = s_slice(full, 3, full.n);
	VecS lines = {0};
	{
		size_t start = 0;
		for (size_t i = 0; i <= full.n; i++) {
			if (i == full.n || full.p[i] == '\n') {
				S l = s_slice(full, start, i);
				if (l.n > 0 && l.p[l.n - 1] == '\r') l.n--;
				VecS_push(a, &lines, l);
				start = i + 1;
			}
		}
	}
	size_t i = 0;
	while (i < lines.len) {
		size_t lineno = i + 1;
		S line = trim_end(lines.data[i]);
		size_t ind = 0; while (ind < line.n && (line.p[ind] == ' ' || line.p[ind] == '\t')) ind++;
		S indent = s_slice(line, 0, ind);
		S rest = s_slice(line, ind, line.n);
		if (rest.n == 0) { P.saw_blank = 1; i++; continue; }
		/* Whole-line comment: hold it for the next line that binds a node. It
		   consumes a pending blank into its own flag, so a blank between
		   comment-only regions survives the round-trip. Text and indent are
		   dup'd - the caller's buffer may not outlive the document. */
		if (rest.p[0] == '#') {
			Pend pd; pd.text = s_dup(a, rest); pd.indent = s_dup(a, indent); pd.blank_before = P.saw_blank; P.saw_blank = 0;
			VecPend_push(a, &P.pending, pd);
			i++; continue;
		}
		/* Any other line consumes the pending blank; only a field line that
		   binds turns it into grouping. */
		int had_blank = P.saw_blank; P.saw_blank = 0;
		/* A binding line claims the pending comments - but deeper-written ones
		   hang on their own block first. */
		hang_deeper_pending(&P, indent);
		Fence f = fence_open(a, rest);
		if (f.ok) {
			size_t parent;
			if (!resolve_parent(&P, indent, &parent)) { p_err(&P, lineno, s_lit("indentation matches no open level")); d->lost++; i++; continue; }
			size_t next; Value val = consume_raw(&P, lines.data, lines.len, i + 1, lineno, f.ch, f.len, f.info, &next);
			size_t bnode = bind_block(&P, parent, val, lineno);
			if (bnode != (size_t)-1) attach_trivia(&P, bnode, s_empty());
			i = next; continue;
		}
		if (rest.n >= 1 && rest.p[0] == '*') {
			S after = s_slice(rest, 1, rest.n);
			if (after.n >= 1 && (after.p[0] == ' ' || after.p[0] == '\t')) {
				size_t parent;
				if (!resolve_parent(&P, indent, &parent)) { p_err(&P, lineno, s_lit("indentation matches no open level")); d->lost++; i++; continue; }
				S ecomment; S body = split_comment(after, &ecomment);
				/* Elements have no node of their own; trivia rides the field. */
				if (parent != ROOT) attach_trivia(&P, parent, ecomment);
				add_star_element(&P, parent, body, lineno); i++; continue;
			}
			p_err(&P, lineno, s_lit("malformed line: '*' must be followed by a space"));
			/* Content-malformed at any position, so it is safe to retain
			   verbatim as trivia: re-emitted, it re-diagnoses identically and
			   can never read as a live binding. A hand-typo no longer
			   vanishes on the consumer's next save. The one exception is a
			   leading BOM - the file-start strip would rewrite it, so that
			   line counts as lost instead. */
			if (rest.n >= 3 && (unsigned char)rest.p[0] == 0xEF && (unsigned char)rest.p[1] == 0xBB && (unsigned char)rest.p[2] == 0xBF) d->lost++;
			else {
				Pend pd; pd.text = s_dup(a, trim_end(rest)); pd.indent = s_dup(a, indent); pd.blank_before = had_blank;
				VecPend_push(a, &P.pending, pd);
			}
			i++; continue;
		}
		S comment; S before = split_comment(rest, &comment);
		S content = trim_end(before);
		if (content.n == 0) {
			/* Only a comment survived (e.g. an escaped lead-in); keep it. */
			if (comment.n) {
				Pend pd; pd.text = s_dup(a, comment); pd.indent = s_dup(a, indent); pd.blank_before = had_blank;
				VecPend_push(a, &P.pending, pd);
			}
			i++; continue;
		}
		size_t parent;
		if (!resolve_parent(&P, indent, &parent)) { p_err(&P, lineno, s_lit("indentation matches no open level")); d->lost++; i++; continue; }
		PathScan scan = scan_path(a, content);
		if (!scan.ok) {
			SB m = {0}; sb_puts(a, &m, "malformed line skipped: "); sb_putS(a, &m, scan.err); p_err(&P, lineno, sb_S(&m));
			/* Content-malformed at any position - retained as trivia, same
			   rationale (and same BOM exception) as the bad '*' line above. */
			if (rest.n >= 3 && (unsigned char)rest.p[0] == 0xEF && (unsigned char)rest.p[1] == 0xBB && (unsigned char)rest.p[2] == 0xBF) d->lost++;
			else {
				Pend pd; pd.text = s_dup(a, trim_end(rest)); pd.indent = s_dup(a, indent); pd.blank_before = had_blank;
				VecPend_push(a, &P.pending, pd);
			}
			i++; continue;
		}
		size_t next = i + 1;
		Value value;
		if (!scan.has_value) { p_err(&P, lineno, s_lit("missing colon; repaired as an empty value")); value = v_empty(); }
		else if (scan.value_text.n == 0) value = v_empty();
		else {
			Fence vf = fence_open(a, scan.value_text);
			if (vf.ok) value = consume_raw(&P, lines.data, lines.len, i + 1, lineno, vf.ch, vf.len, vf.info, &next);
			else {
				if (unterminated_quote(a, scan.value_text)) p_err(&P, lineno, s_lit("unterminated quote in value"));
				value = parse_cell(a, scan.value_text);
			}
		}
		size_t node;
		if (attach_path(&P, parent, scan.segs.data, scan.segs.len, value, lineno, &node)) {
			if (had_blank) NODE(d, node).blank_before = 1;
			attach_trivia(&P, node, comment);
			StackEnt se; se.indent = s_dup(a, indent); se.node = node; VecStack_push(a, &P.stack, se);
		}
		i = next;
	}
	star_flush(&P);
	fold_late_dups(&P);
	emit_repeated_leaf_hints(&P);
	/* Indented tail comments keep their block; only top-level ones orphan. */
	hang_deeper_pending(&P, s_empty());
	for (size_t k = 0; k < P.pending.len; k++)
		VecLead_push(a, &d->orphans, lead_make(P.pending.data[k].text, P.pending.data[k].blank_before));
	return d;
}

// --- accessor: path resolution ----------------------------------------------

typedef enum { R_NONE, R_ONE, R_MANY, R_SLOTS } rkind;
typedef struct { rkind kind; size_t one; VecSize many; VecSlot slots; } Resolved;

static Resolved resolve_from(shcl_doc *d, size_t *start, size_t nstart, Segment *segs, size_t nsegs) {
	Arena *a = &d->scratch; // candidates, slots, compare strings: dead after the call
	VecSize cur = {0};
	// cppcheck-suppress objectIndex  ## single-element callers pass nstart == 1, so start[i] stays at 0
	for (size_t i = 0; i < nstart; i++) VecSize_push(a, &cur, start[i]);
	for (size_t si = 0; si < nsegs; si++) {
		Segment *seg = &segs[si];
		VecSize next = {0};
		for (size_t k = 0; k < cur.len; k++) {
			VecSize ch = NODE(d, cur.data[k]).children;
			if (seg->star) {
				for (size_t j = 0; j < ch.len; j++) VecSize_push(a, &next, ch.data[j]);
			} else {
				for (size_t j = 0; j < ch.len; j++) { size_t c = ch.data[j]; if (s_eq(NODE(d, c).name, seg->name)) VecSize_push(a, &next, c); }
			}
		}
		if (seg->star) {
			// Name wildcard: same per-slot split as `[*]`, over every child.
			Segment *rest = segs + si + 1; size_t nrest = nsegs - si - 1;
			VecSlot slots = {0};
			for (size_t k = 0; k < next.len; k++) {
				Slot sl; sl.present = 0; sl.idx = 0; sl.miss = SHCL_NOT_FOUND;
				if (nrest == 0) { sl.present = 1; sl.idx = next.data[k]; }
				else {
					size_t inst = next.data[k]; Resolved r = resolve_from(d, &inst, 1, rest, nrest);
					if (r.kind == R_ONE) { sl.present = 1; sl.idx = r.one; }
					else if (r.kind != R_NONE) sl.miss = SHCL_MULTIPLE;
				}
				VecSlot_push(a, &slots, sl);
			}
			Resolved R; R.kind = R_SLOTS; R.slots = slots; memset(&R.many, 0, sizeof R.many); R.one = 0;
			return R;
		}
		switch (seg->sel.tag) {
		case SEL_NONE: cur = next; break;
		case SEL_VALUE: {
			VecSize f = {0};
			S want = apply_escapes(a, seg->sel.value);
			for (size_t k = 0; k < next.len; k++) if (s_eq(disp_key(a, &NODE(d, next.data[k]).value), want) && (!seg->sel.quoted || single_scalar(&NODE(d, next.data[k]).value))) VecSize_push(a, &f, next.data[k]);
			cur = f; break;
		}
		case SEL_INDEX: {
			VecSize f = {0};
			if (seg->sel.index < next.len) VecSize_push(a, &f, next.data[seg->sel.index]);
			cur = f; break;
		}
		case SEL_WILDCARD: {
			Segment *rest = segs + si + 1; size_t nrest = nsegs - si - 1;
			VecSlot slots = {0};
			for (size_t k = 0; k < next.len; k++) {
				Slot sl; sl.present = 0; sl.idx = 0; sl.miss = SHCL_NOT_FOUND;
				if (nrest == 0) { sl.present = 1; sl.idx = next.data[k]; }
				else {
					size_t inst = next.data[k]; Resolved r = resolve_from(d, &inst, 1, rest, nrest);
					if (r.kind == R_ONE) { sl.present = 1; sl.idx = r.one; }
					else if (r.kind != R_NONE) sl.miss = SHCL_MULTIPLE;
				}
				VecSlot_push(a, &slots, sl);
			}
			Resolved R; R.kind = R_SLOTS; R.slots = slots; memset(&R.many, 0, sizeof R.many); R.one = 0;
			return R;
		}
		}
	}
	Resolved R; memset(&R, 0, sizeof R);
	if (cur.len == 0) R.kind = R_NONE;
	else if (cur.len == 1) { R.kind = R_ONE; R.one = cur.data[0]; }
	else { R.kind = R_MANY; R.many = cur; }
	return R;
}
static int resolve(shcl_doc *d, S path, Resolved *out) {
	// Every public read/query funnels through here, so this reset is the
	// scratch lifetime: the previous resolve's temporaries die now, and the
	// Resolved this call fills stays usable until the next resolve.
	arena_reset(&d->scratch);
	PathScan ps = scan_lookup(&d->scratch, path);
	if (!ps.ok || ps.has_value) return 0;
	size_t root = ROOT;
	*out = resolve_from(d, &root, 1, ps.segs.data, ps.segs.len);
	return 1;
}
static shcl_status value_at(shcl_doc *d, S path, Value **out) {
	Resolved r;
	if (!resolve(d, path, &r)) return SHCL_NOT_FOUND;
	if (r.kind == R_NONE) return SHCL_NOT_FOUND;
	if (r.kind == R_MANY || r.kind == R_SLOTS) return SHCL_MULTIPLE;
	*out = &NODE(d, r.one).value; return SHCL_GOOD;
}
static shcl_status scalar_at(shcl_doc *d, S path, Element **el) {
	Value *v; shcl_status st = value_at(d, path, &v);
	if (st != SHCL_GOOD) { *el = NULL; return st; }
	if (v->kind == V_EMPTY) { *el = NULL; return SHCL_EMPTY; }
	if (v->kind == V_RAW) { *el = NULL; return SHCL_BAD_TYPE; }
	if (v->nels == 1) { *el = &v->els[0]; return SHCL_GOOD; }
	*el = NULL; return SHCL_BAD_TYPE;
}

// Element list for array reads plus a per-slot pre-status: NULL entry => the
// slot has no coercible scalar and sts[i] already says why (a present element
// can still turn BadType if coercion fails). Wildcard slots stay aligned - the
// spec never drops one silently. The lists land in `a`: public reads pass the
// doc arena (results live until shcl_free); internal queries pass a private
// arena so probing a caller-owned doc leaves nothing behind.
static shcl_status array_elements(shcl_doc *d, Arena *a, S path, Element ***els, shcl_status **sts, size_t *n) {
	Resolved r;
	*els = NULL; *sts = NULL; *n = 0;
	if (!resolve(d, path, &r)) return SHCL_NOT_FOUND;
	if (r.kind == R_SLOTS) {
		size_t m = r.slots.len;
		Element **arr = (Element **)arena_alloc(a, (m ? m : 1) * sizeof(Element *));
		shcl_status *st = (shcl_status *)arena_alloc(a, (m ? m : 1) * sizeof(shcl_status));
		for (size_t i = 0; i < m; i++) {
			arr[i] = NULL;
			if (!r.slots.data[i].present) { st[i] = r.slots.data[i].miss; continue; }
			Value *v = &NODE(d, r.slots.data[i].idx).value;
			if (v->kind == V_EMPTY) st[i] = SHCL_EMPTY;
			else if (v->kind == V_CELL && v->nels == 1) { arr[i] = &v->els[0]; st[i] = SHCL_GOOD; }
			else st[i] = SHCL_BAD_TYPE; // raw block, or an array is not one scalar
		}
		*els = arr; *sts = st; *n = m; return m == 0 ? SHCL_EMPTY : SHCL_GOOD;
	}
	if (r.kind == R_NONE) return SHCL_NOT_FOUND;
	if (r.kind == R_MANY) return SHCL_MULTIPLE;
	Value *v = &NODE(d, r.one).value;
	if (v->kind == V_EMPTY) return SHCL_EMPTY;
	if (v->kind == V_RAW) return SHCL_BAD_TYPE;
	size_t m = v->nels;
	Element **arr = (Element **)arena_alloc(a, (m ? m : 1) * sizeof(Element *));
	shcl_status *st = (shcl_status *)arena_alloc(a, (m ? m : 1) * sizeof(shcl_status));
	for (size_t i = 0; i < m; i++) { arr[i] = &v->els[i]; st[i] = SHCL_GOOD; }
	*els = arr; *sts = st; *n = m; return SHCL_GOOD;
}

static shcl_status worst_slot(const shcl_status *sts, size_t n, shcl_status floor_) {
	shcl_status w = floor_;
	for (size_t i = 0; i < n; i++) if (sts[i] > w) w = sts[i];
	return w;
}

size_t shcl_count(shcl_doc *d, const char *path, size_t plen) {
	S p; p.p = path; p.n = plen;
	Resolved r; if (!resolve(d, p, &r)) return 0;
	switch (r.kind) { case R_NONE: return 0; case R_ONE: return 1; case R_MANY: return r.many.len; case R_SLOTS: return r.slots.len; }
	return 0;
}
static S emit_name(Arena *a, S name);

size_t shcl_paths(shcl_doc *d, shcl_str **out) {
	Arena *a = &d->arena;
	Arena *t = &d->scratch; // walk stack + dedup set: dead after the call
	typedef struct { size_t node; S prefix; } PEnt;
	PEnt *stack = NULL; size_t sn = 0, sc = 0;
	shcl_str *arr = NULL; size_t n = 0, cap = 0;
	CMap seen; memset(&seen, 0, sizeof seen);
	#define PPUSH(N, P) do { if (sn == sc) { size_t nc = sc ? sc * 2 : 16; stack = (PEnt *)arena_grow(t, stack, sc, nc, sizeof(PEnt)); sc = nc; } stack[sn].node = (N); stack[sn].prefix = (P); sn++; } while (0)
	VecSize top = NODE(d, ROOT).children;
	for (size_t i = top.len; i > 0; i--) PPUSH(top.data[i - 1], s_empty());
	while (sn) {
		PEnt e = stack[--sn];
		S seg = emit_name(a, NODE(d, e.node).name);
		S path;
		if (e.prefix.n == 0) path = seg;
		else { SB b = {0}; sb_putS(a, &b, e.prefix); sb_putc(a, &b, '.'); sb_putS(a, &b, seg); path = sb_S(&b); }
		uint64_t h = cmap_hash(path, s_empty());
		if (cmap_get(&seen, h, path, s_empty()) == (size_t)-1) {
			cmap_put(t, &seen, h, path, s_empty(), n);
			if (n == cap) { size_t nc = cap ? cap * 2 : 16; arr = (shcl_str *)arena_grow(a, arr, cap, nc, sizeof(shcl_str)); cap = nc; }
			arr[n].p = path.p; arr[n].n = path.n; n++;
		}
		VecSize kids = NODE(d, e.node).children;
		for (size_t i = kids.len; i > 0; i--) PPUSH(kids.data[i - 1], path);
	}
	#undef PPUSH
	if (!arr) arr = (shcl_str *)arena_alloc(a, sizeof(shcl_str));
	*out = arr; return n;
}

shcl_str shcl_quote_segment(shcl_doc *d, const char *name, size_t len) {
	S in; in.p = name; in.n = len;
	S q = emit_name(&d->arena, in);
	if (q.p == in.p) q = s_dup(&d->arena, in); // bare passthrough: copy so the result outlives the caller's buffer
	shcl_str out; out.p = q.p; out.n = q.n;
	return out;
}

static size_t instances_in(shcl_doc *d, Arena *a, S p, shcl_str **out) {
	// Wildcard slots that did not resolve stay in the list as "" so indices
	// keep matching shcl_count.
	Resolved r;
	if (!resolve(d, p, &r)) { *out = (shcl_str *)arena_alloc(a, sizeof(shcl_str)); return 0; }
	if (r.kind == R_SLOTS) {
		size_t m = r.slots.len;
		shcl_str *arr = (shcl_str *)arena_alloc(a, (m ? m : 1) * sizeof(shcl_str));
		for (size_t k = 0; k < m; k++)
			arr[k] = r.slots.data[k].present ? value_display(a, &NODE(d, r.slots.data[k].idx).value) : s_empty();
		*out = arr; return m;
	}
	VecSize nodes = {0};
	if (r.kind == R_ONE) VecSize_push(a, &nodes, r.one);
	else if (r.kind == R_MANY) for (size_t k = 0; k < r.many.len; k++) VecSize_push(a, &nodes, r.many.data[k]);
	shcl_str *arr = (shcl_str *)arena_alloc(a, (nodes.len ? nodes.len : 1) * sizeof(shcl_str));
	for (size_t k = 0; k < nodes.len; k++) arr[k] = value_display(a, &NODE(d, nodes.data[k]).value);
	*out = arr; return nodes.len;
}
size_t shcl_instances(shcl_doc *d, const char *path, size_t plen, shcl_str **out) {
	S p; p.p = path; p.n = plen;
	return instances_in(d, &d->arena, p, out);
}

size_t shcl_line(shcl_doc *d, const char *path, size_t plen) {
	S p; p.p = path; p.n = plen;
	Resolved r; if (!resolve(d, p, &r)) return 0;
	if (r.kind != R_ONE) return 0;
	return NODE(d, r.one).line; // writer-built nodes carry 0
}

shcl_str shcl_source_name(shcl_doc *d, const char *path, size_t plen) {
	S p; p.p = path; p.n = plen;
	Resolved r; if (!resolve(d, p, &r)) return s_empty();
	if (r.kind != R_ONE) return s_empty();
	return NODE(d, r.one).name_src;
}

size_t shcl_lines(shcl_doc *d, const char *path, size_t plen, size_t **out) {
	// Wildcard slots that did not resolve stay in the list as 0 so indices
	// keep matching shcl_count.
	Arena *a = &d->arena; S p; p.p = path; p.n = plen;
	Resolved r;
	if (!resolve(d, p, &r)) { *out = (size_t *)arena_alloc(a, sizeof(size_t)); return 0; }
	if (r.kind == R_SLOTS) {
		size_t m = r.slots.len;
		size_t *arr = (size_t *)arena_alloc(a, (m ? m : 1) * sizeof(size_t));
		for (size_t k = 0; k < m; k++)
			arr[k] = r.slots.data[k].present ? NODE(d, r.slots.data[k].idx).line : 0;
		*out = arr; return m;
	}
	VecSize nodes = {0};
	if (r.kind == R_ONE) VecSize_push(a, &nodes, r.one);
	else if (r.kind == R_MANY) for (size_t k = 0; k < r.many.len; k++) VecSize_push(a, &nodes, r.many.data[k]);
	size_t *arr = (size_t *)arena_alloc(a, (nodes.len ? nodes.len : 1) * sizeof(size_t));
	for (size_t k = 0; k < nodes.len; k++) arr[k] = NODE(d, nodes.data[k]).line; // writer-built nodes carry 0
	*out = arr; return nodes.len;
}

size_t shcl_children(shcl_doc *d, const char *path, size_t plen, shcl_str **out) {
	// Names come back as stored (already arena-owned); only the array is new.
	Arena *a = &d->arena; S p; p.p = path; p.n = plen;
	size_t node = ROOT;
	if (s_trim(p).n != 0) {
		Resolved r;
		if (!resolve(d, p, &r) || r.kind != R_ONE) { *out = (shcl_str *)arena_alloc(a, sizeof(shcl_str)); return 0; }
		node = r.one;
	}
	VecSize kids = NODE(d, node).children;
	shcl_str *arr = (shcl_str *)arena_alloc(a, (kids.len ? kids.len : 1) * sizeof(shcl_str));
	for (size_t k = 0; k < kids.len; k++) { arr[k].p = NODE(d, kids.data[k]).name.p; arr[k].n = NODE(d, kids.data[k]).name.n; }
	*out = arr; return kids.len;
}

// --- Writer ------------------------------------------------------------------
// The reverse of the reads. Reads and shcl_to_canonical walk the children vecs,
// so mutating the arena directly is enough - the parser's child map is gone by
// now. New value text is dup'd into the arena; the caller's buffers may go away.

static S w_dupz(Arena *a, const char *p, size_t n) { S s; s.p = p; s.n = n; return s_dup(a, s); }
static S w_int_text(Arena *a, int64_t v) { char b[32]; int n = snprintf(b, sizeof b, "%lld", (long long)v); return w_dupz(a, b, (size_t)n); }
static S w_float_text(Arena *a, double v) { char b[SHCL_F64_BUF]; size_t n = shcl_format_f64(v, b); return w_dupz(a, b, n); }
static S w_bool_text(int v) { return v ? s_lit("true") : s_lit("false"); }
static S w_dt_text(Arena *a, const shcl_datetime *dt) { char b[64]; size_t n = shcl_datetime_str(dt, b); return w_dupz(a, b, n); }

// Inverse of a scalar string read (apply_escapes): only backslash, newline, and
// tab need encoding; emit_element wraps quote/reserved chars, reparse strips it.
static S w_encode_string(Arena *a, S s) {
	SB b = {0};
	for (size_t i = 0; i < s.n; i++) {
		char c = s.p[i];
		if (c == '\\') sb_puts(a, &b, "\\\\");
		else if (c == '\n') sb_puts(a, &b, "\\n");
		else if (c == '\t') sb_puts(a, &b, "\\t");
		else sb_putc(a, &b, c);
	}
	return sb_S(&b);
}

// Pick a backtick fence long enough that no content line closes it early.
static void w_choose_fence(S content, unsigned char *fc, size_t *fl) {
	size_t maxrun = 0, start = 0;
	for (size_t i = 0;; i++) {
		if (i == content.n || content.p[i] == '\n') {
			S t = s_trim(s_slice(content, start, i));
			if (t.n > 0) {
				int all = 1;
				for (size_t k = 0; k < t.n; k++) if (t.p[k] != '`') { all = 0; break; }
				if (all && t.n > maxrun) maxrun = t.n;
			}
			if (i == content.n) break;
			start = i + 1;
		}
	}
	*fc = '`'; *fl = maxrun + 1 < 3 ? 3 : maxrun + 1;
}

static Value w_cell1(Arena *a, S text) {
	Value v; memset(&v, 0, sizeof v); v.kind = V_CELL;
	Element *e = (Element *)arena_alloc(a, sizeof(Element)); e->text = text; e->quoted = 0;
	v.els = e; v.nels = 1; return v;
}
// Inline-array value; the empty array is an empty value (reads back Empty).
static Value w_array(Arena *a, S *texts, size_t n) {
	if (n == 0) return v_empty();
	Value v; memset(&v, 0, sizeof v); v.kind = V_CELL;
	Element *els = (Element *)arena_alloc(a, n * sizeof(Element));
	for (size_t i = 0; i < n; i++) { els[i].text = texts[i]; els[i].quoted = 0; }
	v.els = els; v.nels = n; return v;
}

static size_t w_new_child(shcl_doc *d, size_t parent, S name, S name_src, Value value) {
	Arena *a = &d->arena;
	size_t idx = d->nodes.len;
	Node n; memset(&n, 0, sizeof n);
	n.name = s_dup(a, name); n.name_src = s_dup(a, name_src); n.value = value; n.parent = parent;
	/* Hand-written files separate top-level sections with a blank line;
	   writer-built ones do the same (the emitter never blanks line 1). */
	n.blank_before = (parent == ROOT);
	VecNode_push(a, &d->nodes, n);
	VecSize_push(a, &NODE(d, parent).children, idx);
	return idx;
}

// Why a write at this path would fail - the validation walk w_place runs
// before creating anything. SHCL_W_WRITABLE means w_place's gate would pass;
// nothing is created. Temporaries (scan, compare strings) go into `a`.
static shcl_write_reason w_write_reason(shcl_doc *d, Arena *a, S path) {
	PathScan ps = scan_lookup(a, path);
	if (!ps.ok) return SHCL_W_BAD_PATH;
	if (ps.has_value) return SHCL_W_VALUE_IN_PATH;
	if (ps.segs.len == 0) return SHCL_W_BAD_PATH;
	/* Writer side of the load-time nesting cap: never create deeper. */
	if (ps.segs.len > SHCL_MAX_DEPTH) return SHCL_W_TOO_DEEP;
	/* Once this probe falls off the existing tree, a later `[#k]` can never
	   match (fresh intermediates are created childless), so an index segment
	   past that point is unresolvable. */
	int off = 0; size_t pr = ROOT;
	for (size_t i = 0; i < ps.segs.len; i++) {
		Segment *seg = &ps.segs.data[i];
		if (seg->star) return SHCL_W_WILDCARD;
		if (seg->sel.tag == SEL_WILDCARD) return SHCL_W_WILDCARD;
		if (seg->sel.tag == SEL_INDEX) {
			if (off) return SHCL_W_NO_SUCH_INDEX;
			size_t match = (size_t)-1, cnt = 0;
			VecSize ch = NODE(d, pr).children;
			for (size_t k = 0; k < ch.len; k++) if (s_eq(NODE(d, ch.data[k]).name, seg->name)) { if (cnt == seg->sel.index) { match = ch.data[k]; break; } cnt++; }
			if (match == (size_t)-1) return SHCL_W_NO_SUCH_INDEX;
			pr = match;
		} else if (!off) {
			size_t found = (size_t)-1;
			S want = (seg->sel.tag == SEL_VALUE) ? apply_escapes(a, seg->sel.value) : s_empty();
			VecSize ch = NODE(d, pr).children;
			for (size_t k = 0; k < ch.len; k++) {
				size_t c = ch.data[k];
				if (!s_eq(NODE(d, c).name, seg->name)) continue;
				if (seg->sel.tag == SEL_VALUE && !(s_eq(disp_key(a, &NODE(d, c).value), want) && (!seg->sel.quoted || single_scalar(&NODE(d, c).value)))) continue;
				found = c; break;
			}
			if (found == (size_t)-1) off = 1; else pr = found;
		}
	}
	return SHCL_W_WRITABLE;
}

// Walk (creating as needed) to the node a write targets. Returns 1 + *out, or 0
// if the path is unusable for a write (w_write_reason says why). Validation
// runs first, so a doomed path leaves no half-created intermediates behind.
static int w_place(shcl_doc *d, S path, size_t *out) {
	Arena *a = &d->arena;
	// The probe, the scan, and the compare strings are dead once this returns,
	// so they go through scratch (reset like resolve's; no resolve runs in
	// here) - repeated setters must not grow the doc. Only what w_new_child
	// dups persists.
	Arena *t = &d->scratch;
	arena_reset(t);
	if (w_write_reason(d, t, path) != SHCL_W_WRITABLE) return 0;
	PathScan ps = scan_lookup(t, path);
	size_t cur = ROOT;
	for (size_t i = 0; i < ps.segs.len; i++) {
		Segment *seg = &ps.segs.data[i];
		if (seg->star) return 0; // write_reason gates this; belt only
		if (seg->sel.tag == SEL_NONE) {
			size_t found = (size_t)-1;
			VecSize ch = NODE(d, cur).children;
			for (size_t k = 0; k < ch.len; k++) if (s_eq(NODE(d, ch.data[k]).name, seg->name)) { found = ch.data[k]; break; }
			cur = (found != (size_t)-1) ? found : w_new_child(d, cur, seg->name, seg->name_src, v_empty());
		} else if (seg->sel.tag == SEL_VALUE) {
			size_t found = (size_t)-1;
			S want = apply_escapes(t, seg->sel.value);
			VecSize ch = NODE(d, cur).children;
			for (size_t k = 0; k < ch.len; k++) { size_t c = ch.data[k]; if (s_eq(NODE(d, c).name, seg->name) && s_eq(disp_key(t, &NODE(d, c).value), want) && (!seg->sel.quoted || single_scalar(&NODE(d, c).value))) { found = c; break; } }
			cur = (found != (size_t)-1) ? found : w_new_child(d, cur, seg->name, seg->name_src, w_cell1(a, s_dup(a, seg->sel.value)));
		} else if (seg->sel.tag == SEL_INDEX) {
			size_t match = (size_t)-1, cnt = 0;
			VecSize ch = NODE(d, cur).children;
			for (size_t k = 0; k < ch.len; k++) if (s_eq(NODE(d, ch.data[k]).name, seg->name)) { if (cnt == seg->sel.index) { match = ch.data[k]; break; } cnt++; }
			if (match == (size_t)-1) return 0;
			cur = match;
		} else return 0; // wildcard is query-only
	}
	*out = cur; return 1;
}

/* A written value may now collide with a same-named sibling under the in-file
   merge rule; fold the pair the way a reparse would (earlier sibling survives,
   later one folds children and trivia in) so Writer output stays a formatter
   fixpoint. */
static void w_collapse_dup(shcl_doc *d, size_t node) {
	Arena *a = &d->arena;
	size_t parent = NODE(d, node).parent;
	S name = NODE(d, node).name;
	S key = value_key(a, &NODE(d, node).value);
	VecSize ch = NODE(d, parent).children;
	size_t other = (size_t)-1, pos_node = (size_t)-1, pos_other = (size_t)-1;
	for (size_t k = 0; k < ch.len; k++) {
		size_t c = ch.data[k];
		if (c == node) { pos_node = k; continue; }
		if (other == (size_t)-1 && s_eq(NODE(d, c).name, name) && s_eq(value_key(a, &NODE(d, c).value), key)) { other = c; pos_other = k; }
	}
	if (other == (size_t)-1) return;
	size_t survivor = (pos_other < pos_node) ? other : node;
	size_t loser = (survivor == node) ? other : node;
	fold_node_into(d, survivor, loser);
	VecSize *pk = &NODE(d, parent).children;
	size_t w = 0;
	for (size_t k = 0; k < pk->len; k++) if (pk->data[k] != loser) pk->data[w++] = pk->data[k];
	pk->len = w;
}

static int w_set(shcl_doc *d, S path, Value v) {
	size_t idx;
	if (!w_place(d, path, &idx)) return 0;
	NODE(d, idx).value = v;
	w_collapse_dup(d, idx);
	return 1;
}

shcl_doc *shcl_new(void) { return shcl_parse("", 0); }

int shcl_exists(shcl_doc *d, const char *path, size_t plen) {
	S p; p.p = path; p.n = plen; Resolved r;
	if (!resolve(d, p, &r)) return 0;
	if (r.kind == R_ONE || r.kind == R_MANY) return 1;
	if (r.kind == R_SLOTS) for (size_t i = 0; i < r.slots.len; i++) if (r.slots.data[i].present) return 1;
	return 0;
}

size_t shcl_remove(shcl_doc *d, const char *path, size_t plen) {
	Arena *a = &d->arena; S p; p.p = path; p.n = plen; Resolved r;
	if (!resolve(d, p, &r)) return 0;
	VecSize targets = {0};
	if (r.kind == R_ONE) VecSize_push(a, &targets, r.one);
	else if (r.kind == R_MANY) targets = r.many;
	else if (r.kind == R_SLOTS) for (size_t i = 0; i < r.slots.len; i++) if (r.slots.data[i].present) VecSize_push(a, &targets, r.slots.data[i].idx);
	for (size_t i = 0; i < targets.len; i++) {
		size_t t = targets.data[i]; VecSize *kids = &NODE(d, NODE(d, t).parent).children;
		size_t w = 0;
		for (size_t k = 0; k < kids->len; k++) if (kids->data[k] != t) kids->data[w++] = kids->data[k];
		kids->len = w;
	}
	return targets.len;
}

shcl_write_reason shcl_write_reason_(shcl_doc *d, const char *path, size_t plen) {
	S p; p.p = path; p.n = plen;
	// A probe, not a write: temporaries go into scratch (reset like resolve's -
	// the previous query's die now), never permanently into the doc arena.
	arena_reset(&d->scratch);
	return w_write_reason(d, &d->scratch, p);
}

int shcl_set_comment(shcl_doc *d, const char *path, size_t plen, const char *text, size_t tlen) {
	Arena *a = &d->arena; S p; p.p = path; p.n = plen; size_t idx;
	if (!w_place(d, p, &idx)) return 0;
	S line; line.p = text; line.n = tlen;
	for (size_t i = 0; i < line.n; i++) if (line.p[i] == '\n') { line.n = i; break; }
	S out;
	if (line.n == 0 || line.p[0] != '#') { SB b = {0}; sb_puts(a, &b, "# "); sb_putS(a, &b, line); out = sb_S(&b); }
	else out = s_dup(a, line);
	VecLead_push(a, &NODE(d, idx).leading, lead_plain(out));
	return 1;
}

int shcl_set_empty(shcl_doc *d, const char *path, size_t plen) { S p; p.p = path; p.n = plen; return w_set(d, p, v_empty()); }
int shcl_set_int(shcl_doc *d, const char *path, size_t plen, int64_t v) { Arena *a = &d->arena; S p; p.p = path; p.n = plen; return w_set(d, p, w_cell1(a, w_int_text(a, v))); }
int shcl_set_float(shcl_doc *d, const char *path, size_t plen, double v) { Arena *a = &d->arena; S p; p.p = path; p.n = plen; return w_set(d, p, w_cell1(a, w_float_text(a, v))); }
int shcl_set_bool(shcl_doc *d, const char *path, size_t plen, int v) { Arena *a = &d->arena; S p; p.p = path; p.n = plen; return w_set(d, p, w_cell1(a, w_bool_text(v))); }
int shcl_set_string(shcl_doc *d, const char *path, size_t plen, const char *s, size_t slen) { Arena *a = &d->arena; S p; p.p = path; p.n = plen; S in; in.p = s; in.n = slen; return w_set(d, p, w_cell1(a, w_encode_string(a, in))); }

static int literal_value(Arena *a, S text, Value *out) {
	for (size_t i = 0; i < text.n; i++) { if (text.p[i] == '\n' || text.p[i] == '\r') return 0; }
	S comment; S v = s_trim(split_comment(text, &comment));
	if (unterminated_quote(a, v)) return 0;
	*out = parse_cell(a, v);
	return 1;
}

int shcl_set_literal(shcl_doc *d, const char *path, size_t plen, const char *text, size_t tlen) {
	Arena *a = &d->arena; S p; p.p = path; p.n = plen; S in; in.p = text; in.n = tlen;
	Value v;
	if (!literal_value(a, in, &v)) return 0;
	return w_set(d, p, v);
}
int shcl_set_datetime(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *dt) { Arena *a = &d->arena; S p; p.p = path; p.n = plen; return w_set(d, p, w_cell1(a, w_dt_text(a, dt))); }
int shcl_set_raw(shcl_doc *d, const char *path, size_t plen, const char *content, size_t clen, const char *info, size_t ilen) {
	Arena *a = &d->arena; S p; p.p = path; p.n = plen;
	S c = w_dupz(a, content, clen), inf = w_dupz(a, info, ilen);
	unsigned char fc; size_t fl; w_choose_fence(c, &fc, &fl);
	Value v; memset(&v, 0, sizeof v); v.kind = V_RAW; v.content = c; v.info = inf; v.fence_char = fc; v.fence_len = fl;
	return w_set(d, p, v);
}

int shcl_set_int_array(shcl_doc *d, const char *path, size_t plen, const int64_t *v, size_t n) {
	Arena *a = &d->arena; S p; p.p = path; p.n = plen; S *t = (S *)arena_alloc(a, (n ? n : 1) * sizeof(S));
	for (size_t i = 0; i < n; i++) t[i] = w_int_text(a, v[i]);
	return w_set(d, p, w_array(a, t, n));
}
int shcl_set_float_array(shcl_doc *d, const char *path, size_t plen, const double *v, size_t n) {
	Arena *a = &d->arena; S p; p.p = path; p.n = plen; S *t = (S *)arena_alloc(a, (n ? n : 1) * sizeof(S));
	for (size_t i = 0; i < n; i++) t[i] = w_float_text(a, v[i]);
	return w_set(d, p, w_array(a, t, n));
}
int shcl_set_bool_array(shcl_doc *d, const char *path, size_t plen, const int *v, size_t n) {
	Arena *a = &d->arena; S p; p.p = path; p.n = plen; S *t = (S *)arena_alloc(a, (n ? n : 1) * sizeof(S));
	for (size_t i = 0; i < n; i++) t[i] = w_bool_text(v[i]);
	return w_set(d, p, w_array(a, t, n));
}
int shcl_set_string_array(shcl_doc *d, const char *path, size_t plen, const char *const *v, const size_t *lens, size_t n) {
	Arena *a = &d->arena; S p; p.p = path; p.n = plen; S *t = (S *)arena_alloc(a, (n ? n : 1) * sizeof(S));
	for (size_t i = 0; i < n; i++) { S in; in.p = v[i]; in.n = lens[i]; t[i] = w_encode_string(a, in); }
	return w_set(d, p, w_array(a, t, n));
}
int shcl_set_datetime_array(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *v, size_t n) {
	Arena *a = &d->arena; S p; p.p = path; p.n = plen; S *t = (S *)arena_alloc(a, (n ? n : 1) * sizeof(S));
	for (size_t i = 0; i < n; i++) t[i] = w_dt_text(a, &v[i]);
	return w_set(d, p, w_array(a, t, n));
}

int shcl_set_int_default(shcl_doc *d, const char *path, size_t plen, int64_t v) { if (!shcl_exists(d, path, plen)) return shcl_set_int(d, path, plen, v); return 1; }
int shcl_set_float_default(shcl_doc *d, const char *path, size_t plen, double v) { if (!shcl_exists(d, path, plen)) return shcl_set_float(d, path, plen, v); return 1; }
int shcl_set_bool_default(shcl_doc *d, const char *path, size_t plen, int v) { if (!shcl_exists(d, path, plen)) return shcl_set_bool(d, path, plen, v); return 1; }
int shcl_set_string_default(shcl_doc *d, const char *path, size_t plen, const char *s, size_t slen) { if (!shcl_exists(d, path, plen)) return shcl_set_string(d, path, plen, s, slen); return 1; }
int shcl_set_literal_default(shcl_doc *d, const char *path, size_t plen, const char *text, size_t tlen) { if (!shcl_exists(d, path, plen)) return shcl_set_literal(d, path, plen, text, tlen); return 1; }
int shcl_set_datetime_default(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *dt) { if (!shcl_exists(d, path, plen)) return shcl_set_datetime(d, path, plen, dt); return 1; }
int shcl_set_raw_default(shcl_doc *d, const char *path, size_t plen, const char *content, size_t clen, const char *info, size_t ilen) { if (!shcl_exists(d, path, plen)) return shcl_set_raw(d, path, plen, content, clen, info, ilen); return 1; }
int shcl_set_int_array_default(shcl_doc *d, const char *path, size_t plen, const int64_t *v, size_t n) { if (!shcl_exists(d, path, plen)) return shcl_set_int_array(d, path, plen, v, n); return 1; }
int shcl_set_float_array_default(shcl_doc *d, const char *path, size_t plen, const double *v, size_t n) { if (!shcl_exists(d, path, plen)) return shcl_set_float_array(d, path, plen, v, n); return 1; }
int shcl_set_bool_array_default(shcl_doc *d, const char *path, size_t plen, const int *v, size_t n) { if (!shcl_exists(d, path, plen)) return shcl_set_bool_array(d, path, plen, v, n); return 1; }
int shcl_set_string_array_default(shcl_doc *d, const char *path, size_t plen, const char *const *v, const size_t *lens, size_t n) { if (!shcl_exists(d, path, plen)) return shcl_set_string_array(d, path, plen, v, lens, n); return 1; }
int shcl_set_datetime_array_default(shcl_doc *d, const char *path, size_t plen, const shcl_datetime *v, size_t n) { if (!shcl_exists(d, path, plen)) return shcl_set_datetime_array(d, path, plen, v, n); return 1; }

// --- Layered loading: overlay a higher-priority document onto a lower one ----

// Deep-copy a value from `over`'s arena into `d`'s, so the merged doc is
// self-contained (over may be freed after the merge).
static Value w_dup_value(Arena *a, const Value *v) {
	Value r; memset(&r, 0, sizeof r); r.kind = v->kind;
	if (v->kind == V_CELL) {
		r.nels = v->nels;
		r.els = (Element *)arena_alloc(a, (v->nels ? v->nels : 1) * sizeof(Element));
		for (size_t i = 0; i < v->nels; i++) { r.els[i].text = s_dup(a, v->els[i].text); r.els[i].quoted = v->els[i].quoted; }
	} else if (v->kind == V_RAW) {
		r.content = s_dup(a, v->content); r.info = s_dup(a, v->info);
		r.fence_char = v->fence_char; r.fence_len = v->fence_len;
	}
	return r;
}

// Deep-copy over's subtree at `oi` into d's arena under `parent`. d->nodes may
// reallocate on push, so parent's children vec is fetched only via NODE().
static size_t w_clone_subtree(shcl_doc *d, const shcl_doc *over, size_t oi, size_t parent) {
	Arena *a = &d->arena;
	const Node *src = &over->nodes.data[oi];
	Node n; memset(&n, 0, sizeof n);
	n.name = s_dup(a, src->name);
	n.name_src = s_dup(a, src->name_src);
	n.value = w_dup_value(a, &src->value);
	n.parent = parent;
	n.line = src->line;
	n.star_list = src->star_list;
	n.star_mixed = src->star_mixed;
	n.blank_before = src->blank_before;
	n.trailing = s_dup(a, src->trailing);
	for (size_t i = 0; i < src->leading.len; i++) VecLead_push(a, &n.leading, lead_make(s_dup(a, src->leading.data[i].text), src->leading.data[i].blank_before));
	for (size_t i = 0; i < src->after.len; i++) VecLead_push(a, &n.after, lead_make(s_dup(a, src->after.data[i].text), src->after.data[i].blank_before));
	for (size_t i = 0; i < src->inside.len; i++) VecLead_push(a, &n.inside, lead_make(s_dup(a, src->inside.data[i].text), src->inside.data[i].blank_before));
	size_t idx = d->nodes.len;
	VecNode_push(a, &d->nodes, n);
	// Snapshot the source children (const, stable) before recursing.
	size_t nk = over->nodes.data[oi].children.len;
	for (size_t i = 0; i < nk; i++) {
		size_t ok = over->nodes.data[oi].children.data[i];
		size_t c = w_clone_subtree(d, over, ok, idx);
		VecSize_push(a, &NODE(d, idx).children, c);
	}
	return idx;
}

/* A matched instance keeps the base node, so the over side's comments have to
   move onto it or they are lost. Same rule as an in-file merge: leading
   concatenates in layer order, first trailing wins. Text is dup'd into d's
   arena - over may be freed after the merge. */
static void adopt_trivia(shcl_doc *d, size_t base, const shcl_doc *over, size_t ok) {
	Arena *a = &d->arena;
	const Node *src = &over->nodes.data[ok];
	for (size_t i = 0; i < src->leading.len; i++)
		VecLead_push(a, &NODE(d, base).leading, lead_make(s_dup(a, src->leading.data[i].text), src->leading.data[i].blank_before));
	if (src->trailing.n) {
		if (NODE(d, base).trailing.n == 0) NODE(d, base).trailing = s_dup(a, src->trailing);
		else VecLead_push(a, &NODE(d, base).leading, lead_plain(s_dup(a, src->trailing)));
	}
	for (size_t i = 0; i < src->after.len; i++)
		VecLead_push(a, &NODE(d, base).after, lead_make(s_dup(a, src->after.data[i].text), src->after.data[i].blank_before));
	for (size_t i = 0; i < src->inside.len; i++)
		VecLead_push(a, &NODE(d, base).inside, lead_make(s_dup(a, src->inside.data[i].text), src->inside.data[i].blank_before));
}

// One grouping pass over each side, then a single children rebuild: the old
// shape re-filtered the over side per distinct name and re-scanned (and
// re-keyed) the base side per over node - three O(K^2) terms at one parent,
// plus a fresh children vector per replaced name.
static void w_overlay(shcl_doc *d, size_t bp, const shcl_doc *over, size_t op) {
	Arena *a = &d->arena;
	Arena *t = &d->scratch;
	VecSize okids = over->nodes.data[op].children; // const doc: stable
	// Over side: name -> bucket, in first-appearance order.
	VecS order = {0}; VecSize *buckets = NULL; size_t nb = 0, cb = 0;
	CMap group_of; memset(&group_of, 0, sizeof group_of);
	for (size_t i = 0; i < okids.len; i++) {
		size_t k = okids.data[i]; S nm = over->nodes.data[k].name;
		uint64_t h = cmap_hash(nm, s_empty());
		size_t g = cmap_get(&group_of, h, nm, s_empty());
		if (g == (size_t)-1) {
			if (nb == cb) { size_t nc = cb ? cb * 2 : 8; buckets = (VecSize *)arena_grow(t, buckets, cb, nc, sizeof(VecSize)); cb = nc; }
			memset(&buckets[nb], 0, sizeof buckets[nb]);
			g = nb++;
			cmap_put(t, &group_of, h, nm, s_empty(), g);
			VecS_push(t, &order, nm);
		}
		VecSize_push(t, &buckets[g], k);
	}
	// Base side, one pass: does the name exist / have a container instance,
	// and which child carries each (name, key) - every key computed once.
	VecSize base = {0};
	{ VecSize bk = NODE(d, bp).children; for (size_t i = 0; i < bk.len; i++) VecSize_push(t, &base, bk.data[i]); }
	CMap in_base, has_cont, by_key;
	memset(&in_base, 0, sizeof in_base); memset(&has_cont, 0, sizeof has_cont); memset(&by_key, 0, sizeof by_key);
	for (size_t i = 0; i < base.len; i++) {
		size_t b = base.data[i]; S nm = NODE(d, b).name;
		uint64_t hn = cmap_hash(nm, s_empty());
		if (cmap_get(&in_base, hn, nm, s_empty()) == (size_t)-1) cmap_put(t, &in_base, hn, nm, s_empty(), 1);
		if (NODE(d, b).children.len > 0 && cmap_get(&has_cont, hn, nm, s_empty()) == (size_t)-1) cmap_put(t, &has_cont, hn, nm, s_empty(), 1);
		S key = value_key(t, &NODE(d, b).value);
		uint64_t hk = cmap_hash(nm, key);
		if (cmap_get(&by_key, hk, nm, key) == (size_t)-1) cmap_put(t, &by_key, hk, nm, key, b);
	}
	// Decide per name. A name whose over-side nodes are all leaves is an
	// override - but only when the base side of the group is leaf-shaped too.
	// Against a base container, a childless over-node is a wrapper mention,
	// not a leaf, so it falls through to the instance merge: a bare section
	// header in a higher layer never wipes the subtree below it. Replaced
	// groups splice in the rebuild; everything appended (unmatched instances,
	// and replaced names base never had) keeps processing order.
	VecSize *rep = (VecSize *)arena_alloc(t, (nb ? nb : 1) * sizeof(VecSize));
	int *is_rep = (int *)arena_alloc(t, (nb ? nb : 1) * sizeof(int));
	VecSize appended = {0};
	int any_rep = 0;
	for (size_t gi = 0; gi < nb; gi++) {
		S name = order.data[gi];
		VecSize grp = buckets[gi];
		memset(&rep[gi], 0, sizeof rep[gi]); is_rep[gi] = 0;
		int over_leafy = 1;
		for (size_t i = 0; i < grp.len; i++) if (over->nodes.data[grp.data[i]].children.len > 0) { over_leafy = 0; break; }
		uint64_t hn = cmap_hash(name, s_empty());
		int inb = cmap_get(&in_base, hn, name, s_empty()) != (size_t)-1;
		int bc = cmap_get(&has_cont, hn, name, s_empty()) != (size_t)-1;
		if (over_leafy && !bc) {
			for (size_t i = 0; i < grp.len; i++) {
				size_t c = w_clone_subtree(d, over, grp.data[i], bp);
				VecSize_push(t, inb ? &rep[gi] : &appended, c);
			}
			if (inb) { is_rep[gi] = 1; any_rep = 1; }
		} else {
			for (size_t i = 0; i < grp.len; i++) {
				size_t ok = grp.data[i];
				S okey = value_key(t, &over->nodes.data[ok].value);
				uint64_t hk = cmap_hash(name, okey);
				size_t b = cmap_get(&by_key, hk, name, okey);
				if (b != (size_t)-1) { adopt_trivia(d, b, over, ok); w_overlay(d, b, over, ok); }
				else VecSize_push(t, &appended, w_clone_subtree(d, over, ok, bp));
			}
		}
	}
	if (!any_rep && appended.len == 0) return;
	// Rebuild once: each replaced group lands at its name's first original
	// position (dropped nodes stay in the arena, unreferenced - reads and
	// emit walk children from the root), appends go at the end.
	CMap spliced; memset(&spliced, 0, sizeof spliced);
	VecSize nw = {0};
	for (size_t i = 0; i < base.len; i++) {
		size_t b = base.data[i]; S nm = NODE(d, b).name;
		uint64_t hn = cmap_hash(nm, s_empty());
		size_t g = cmap_get(&group_of, hn, nm, s_empty());
		if (g != (size_t)-1 && is_rep[g]) {
			if (cmap_get(&spliced, hn, nm, s_empty()) == (size_t)-1) {
				cmap_put(t, &spliced, hn, nm, s_empty(), 1);
				for (size_t k = 0; k < rep[g].len; k++) VecSize_push(a, &nw, rep[g].data[k]);
			}
		} else {
			VecSize_push(a, &nw, b);
		}
	}
	for (size_t k = 0; k < appended.len; k++) VecSize_push(a, &nw, appended.data[k]);
	NODE(d, bp).children = nw;
}

void shcl_merge(shcl_doc *d, const shcl_doc *over) {
	d->lost += over->lost;
	Arena *a = &d->arena;
	arena_reset(&d->scratch); // merge temporaries (compare keys, clone lists) die here
	w_overlay(d, ROOT, over, ROOT);
	// Layers commonly share a footer; keeping one copy of each keeps a stack
	// of files from repeating it once per layer.
	for (size_t i = 0; i < over->orphans.len; i++) {
		S ot = over->orphans.data[i].text;
		int dup = 0;
		for (size_t k = 0; k < d->orphans.len; k++) if (s_eq(d->orphans.data[k].text, ot)) { dup = 1; break; }
		if (!dup) VecLead_push(a, &d->orphans, lead_make(s_dup(a, ot), over->orphans.data[i].blank_before));
	}
}

int64_t shcl_get_int(shcl_doc *d, const char *path, size_t plen, int64_t def) {
	shcl_read_i64 r = shcl_read_int(d, path, plen); return r.status == SHCL_GOOD ? r.value : def;
}
double shcl_get_float(shcl_doc *d, const char *path, size_t plen, double def) {
	shcl_read_f64 r = shcl_read_float(d, path, plen); return r.status == SHCL_GOOD ? r.value : def;
}
int shcl_get_bool(shcl_doc *d, const char *path, size_t plen, int def) {
	shcl_read_bool r = shcl_read_bool_(d, path, plen); return r.status == SHCL_GOOD ? r.value : def;
}

shcl_read_i64 shcl_read_int(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_i64 R; S p; p.p = path; p.n = plen; Element *el; shcl_status st = scalar_at(d, p, &el);
	if (st != SHCL_GOOD) { R.value = 0; R.status = st; return R; }
	int64_t v; if (parse_int_text(&d->scratch, el, d->strictness, &v)) { R.value = v; R.status = SHCL_GOOD; }
	else { R.value = 0; R.status = SHCL_BAD_TYPE; }
	return R;
}
shcl_read_f64 shcl_read_float(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_f64 R; S p; p.p = path; p.n = plen; Element *el; shcl_status st = scalar_at(d, p, &el);
	if (st != SHCL_GOOD) { R.value = 0; R.status = st; return R; }
	double v; if (parse_float_text(&d->scratch, el, d->strictness, &v)) { R.value = v; R.status = SHCL_GOOD; }
	else { R.value = 0; R.status = SHCL_BAD_TYPE; }
	return R;
}
shcl_read_bool shcl_read_bool_(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_bool R; S p; p.p = path; p.n = plen; Element *el; shcl_status st = scalar_at(d, p, &el);
	if (st != SHCL_GOOD) { R.value = 0; R.status = st; return R; }
	int v; if (parse_bool_text(&d->scratch, el->text, d->strictness, &v)) { R.value = v; R.status = SHCL_GOOD; }
	else { R.value = 0; R.status = SHCL_BAD_TYPE; }
	return R;
}
shcl_read_dt shcl_read_datetime(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_dt R; S p; p.p = path; p.n = plen; Element *el; shcl_status st = scalar_at(d, p, &el);
	memset(&R.value, 0, sizeof R.value); R.value.zone = SHCL_ZONE_NONE;
	if (st != SHCL_GOOD) { R.status = st; return R; }
	// scratch, not the doc arena: parse_datetime only allocates split temporaries
	// there, and the frac it hands back slices el->text, which outlives the call.
	if (parse_datetime(&d->scratch, el->text, &R.value)) R.status = SHCL_GOOD;
	else { memset(&R.value, 0, sizeof R.value); R.value.zone = SHCL_ZONE_NONE; R.status = SHCL_BAD_TYPE; }
	return R;
}
static S emit_element(Arena *a, const Element *e);

shcl_read_str shcl_read_string(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_str R; S p; p.p = path; p.n = plen; Value *v; shcl_status st = value_at(d, p, &v);
	if (st != SHCL_GOOD) { R.value = s_empty(); R.status = st; return R; }
	if (v->kind == V_EMPTY) { R.value = s_empty(); R.status = SHCL_EMPTY; }
	else if (v->kind == V_RAW) { R.value = v->content; R.status = SHCL_GOOD; }
	else if (v->nels == 1) { R.value = apply_escapes(&d->arena, v->els[0].text); R.status = SHCL_GOOD; }
	else {
		/* Canonical inline form (quoting + escapes intact), so the string
		   re-parses to the same array - not the bare display join. */
		Arena *a = &d->arena; SB s = {0};
		for (size_t i = 0; i < v->nels; i++) { if (i) sb_puts(a, &s, ", "); sb_putS(a, &s, emit_element(a, &v->els[i])); }
		R.value = sb_S(&s); R.status = SHCL_GOOD;
	}
	return R;
}
shcl_read_str shcl_read_raw(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_str R; S p; p.p = path; p.n = plen; Value *v; shcl_status st = value_at(d, p, &v);
	if (st != SHCL_GOOD) { R.value = s_empty(); R.status = st; return R; }
	if (v->kind == V_RAW) { R.value = v->content; R.status = SHCL_GOOD; }
	else if (v->kind == V_EMPTY) { R.value = s_empty(); R.status = SHCL_EMPTY; }
	else { R.value = s_empty(); R.status = SHCL_BAD_TYPE; }
	return R;
}
shcl_read_str shcl_read_raw_info(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_str R; S p; p.p = path; p.n = plen; Value *v; shcl_status st = value_at(d, p, &v);
	if (st != SHCL_GOOD) { R.value = s_empty(); R.status = st; return R; }
	if (v->kind == V_RAW) { R.value = v->info; R.status = SHCL_GOOD; }
	else { R.value = s_empty(); R.status = SHCL_BAD_TYPE; }
	return R;
}

static shcl_read_i64_arr read_int_array_in(shcl_doc *d, Arena *a, S p) {
	shcl_read_i64_arr R; Element **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, a, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	int64_t *out = (int64_t *)arena_alloc(a, (n ? n : 1) * sizeof(int64_t));
	for (size_t i = 0; i < n; i++) { int64_t v; if (els[i] && parse_int_text(&d->scratch, els[i], d->strictness, &v)) out[i] = v; else { out[i] = 0; if (els[i]) sts[i] = SHCL_BAD_TYPE; } }
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}
shcl_read_i64_arr shcl_read_int_array(shcl_doc *d, const char *path, size_t plen) {
	S p; p.p = path; p.n = plen;
	return read_int_array_in(d, &d->arena, p);
}
shcl_read_f64_arr shcl_read_float_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_f64_arr R; S p; p.p = path; p.n = plen; Element **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, &d->arena, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	double *out = (double *)arena_alloc(&d->arena, (n ? n : 1) * sizeof(double));
	for (size_t i = 0; i < n; i++) { double v; if (els[i] && parse_float_text(&d->scratch, els[i], d->strictness, &v)) out[i] = v; else { out[i] = 0; if (els[i]) sts[i] = SHCL_BAD_TYPE; } }
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}
shcl_read_bool_arr shcl_read_bool_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_bool_arr R; S p; p.p = path; p.n = plen; Element **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, &d->arena, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	int *out = (int *)arena_alloc(&d->arena, (n ? n : 1) * sizeof(int));
	for (size_t i = 0; i < n; i++) { int v; if (els[i] && parse_bool_text(&d->scratch, els[i]->text, d->strictness, &v)) out[i] = v; else { out[i] = 0; if (els[i]) sts[i] = SHCL_BAD_TYPE; } }
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}
shcl_read_dt_arr shcl_read_datetime_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_dt_arr R; S p; p.p = path; p.n = plen; Element **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, &d->arena, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	shcl_datetime *out = (shcl_datetime *)arena_alloc(&d->arena, (n ? n : 1) * sizeof(shcl_datetime));
	for (size_t i = 0; i < n; i++) {
		memset(&out[i], 0, sizeof out[i]); out[i].zone = SHCL_ZONE_NONE;
		if (els[i]) { if (!parse_datetime(&d->scratch, els[i]->text, &out[i])) { memset(&out[i], 0, sizeof out[i]); out[i].zone = SHCL_ZONE_NONE; sts[i] = SHCL_BAD_TYPE; } }
	}
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}
shcl_read_str_arr shcl_read_string_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_str_arr R; S p; p.p = path; p.n = plen; Element **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, &d->arena, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	shcl_str *out = (shcl_str *)arena_alloc(&d->arena, (n ? n : 1) * sizeof(shcl_str));
	for (size_t i = 0; i < n; i++) out[i] = els[i] ? apply_escapes(&d->arena, els[i]->text) : s_empty();
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}

// --- formatter (canonical output) -------------------------------------------

static void bare_quote_counts(S t, size_t *dq, size_t *sq) {
	*dq = 0; *sq = 0; size_t i = 0;
	while (i < t.n) {
		uint32_t c; size_t l = utf8_decode(t.p, t.n, i, &c); i += l;
		if (c == '\\') { if (i < t.n) { uint32_t e; i += utf8_decode(t.p, t.n, i, &e); } continue; }
		if (c == '"') (*dq)++; else if (c == '\'') (*sq)++;
	}
}
static S quote_text(Arena *a, S t) {
	/* A dangling trailing backslash would turn the closing quote into an
	   escape pair - the scanner reads the path back wrong, or not at all.
	   Store the doubled spelling (identical on string read), the same rule
	   the element parser applies to bare text. */
	if (t.n && t.p[t.n - 1] == '\\') t = norm_dangling(a, t);
	size_t dq, sq; bare_quote_counts(t, &dq, &sq);
	SB s = {0};
	if (dq == 0) { sb_putc(a, &s, '"'); sb_putS(a, &s, t); sb_putc(a, &s, '"'); }
	else if (sq == 0) { sb_putc(a, &s, '\''); sb_putS(a, &s, t); sb_putc(a, &s, '\''); }
	else {
		sb_putc(a, &s, '"'); size_t i = 0;
		while (i < t.n) {
			uint32_t c; size_t l = utf8_decode(t.p, t.n, i, &c); i += l;
			if (c == '\\') { sb_putc(a, &s, '\\'); if (i < t.n) { uint32_t e; size_t l2 = utf8_decode(t.p, t.n, i, &e); i += l2; sb_put_cp(a, &s, e); } }
			else if (c == '"') sb_puts(a, &s, "\\\"");
			else sb_put_cp(a, &s, c);
		}
		sb_putc(a, &s, '"');
	}
	return sb_S(&s);
}
// is_data_format: true when the text reads as an int, float, bool, or datetime
// at standard strictness - fixed there deliberately, so canonical form cannot
// vary with the load strictness.
static int is_data_format(Arena *a, const Element *e) {
	int64_t iv; double fv; int bv; shcl_datetime dv;
	if (parse_int_text(a, e, SHCL_STANDARD, &iv)) return 1;
	if (parse_float_text(a, e, SHCL_STANDARD, &fv)) return 1;
	if (parse_bool_text(a, e->text, SHCL_STANDARD, &bv)) return 1;
	if (parse_datetime(a, e->text, &dv)) return 1;
	return 0;
}
// Minimal quoting: bare unless a reserved character (or lookalike hazard) forces it.
// One addition: an author-quoted element keeps its quotes unless the text reads as
// one of SHCL's own data formats - quoting those is just spelling (readers type the
// value either way), but quoting a plain string is the escape and must survive
// canonicalization. This clause only ever adds quoting, so a bare emit stays safe.
static S emit_element(Arena *a, const Element *e) {
	S t = e->text;
	int needs = (t.n == 0);
	if (!needs) {
		size_t i = 0;
		while (i < t.n) { uint32_t c; size_t l = utf8_decode(t.p, t.n, i, &c); i += l;
			if (c == ' ' || c == '\t' || c == ',' || c == ':' || c == '#' || c == '"' || c == '\'' || c == '[' || c == ']') { needs = 1; break; } }
	}
	if (!needs) { Fence f = fence_open(a, t); if (f.ok) needs = 1; }
	if (!needs && e->quoted && !is_data_format(a, e)) needs = 1;
	return needs ? quote_text(a, t) : t;
}
static S emit_name(Arena *a, S name) {
	if (name.n > 0) {
		int allbare = 1; size_t i = 0;
		while (i < name.n) { uint32_t c; size_t l = utf8_decode(name.p, name.n, i, &c); i += l; if (!is_bare_name_char(c)) { allbare = 0; break; } }
		if (allbare) return name;
	}
	return quote_text(a, name);
}
/* Inline comment, canonically two spaces before the `#`. */
static void emit_trailing(Arena *a, SB *out, S trailing) {
	if (trailing.n) { sb_puts(a, out, "  "); sb_putS(a, out, trailing); }
}
// Emit a sibling run. The parent walk already knows whether an earlier
// same-name sibling is empty (the raw same-line-fence hazard), so one
// seen-empties set here replaces a per-child rescan of the whole run.
static void emit_node(shcl_doc *d, size_t idx, size_t depth, int would_merge, SB *out);
static void emit_children(shcl_doc *d, const VecSize *kids, size_t depth, SB *out) {
	CMap empties; memset(&empties, 0, sizeof empties);
	for (size_t i = 0; i < kids->len; i++) {
		size_t c = kids->data[i];
		Node *n = &NODE(d, c);
		uint64_t h = cmap_hash(n->name, s_empty());
		int wm = n->value.kind == V_RAW && cmap_get(&empties, h, n->name, s_empty()) != (size_t)-1;
		if (v_is_empty(&n->value) && cmap_get(&empties, h, n->name, s_empty()) == (size_t)-1)
			cmap_put(&d->scratch, &empties, h, n->name, s_empty(), 1);
		emit_node(d, c, depth, wm, out);
	}
}

static void emit_node(shcl_doc *d, size_t idx, size_t depth, int would_merge, SB *out) {
	Arena *a = &d->arena;
	Node *node = &NODE(d, idx);
	Value *v = &node->value;
	/* Same-line fence spelling can't carry an inline comment (an unbalanced
	   quote in the info-string could hide the `#` on reparse), so its trailing
	   comment joins the leading lines instead; the flag comes from the
	   parent's walk. Each blank rides its own comment (or the binding line),
	   never as the first output line. */
	for (size_t k = 0; k < node->leading.len; k++) {
		if (node->leading.data[k].blank_before && out->len) sb_putc(a, out, '\n');
		for (size_t z = 0; z < depth; z++) sb_putc(a, out, '\t');
		sb_putS(a, out, node->leading.data[k].text); sb_putc(a, out, '\n');
	}
	if (node->blank_before && out->len) sb_putc(a, out, '\n');
	if (would_merge && node->trailing.n) {
		for (size_t z = 0; z < depth; z++) sb_putc(a, out, '\t');
		sb_putS(a, out, node->trailing); sb_putc(a, out, '\n');
	}
	for (size_t k = 0; k < depth; k++) sb_putc(a, out, '\t');
	sb_putS(a, out, emit_name(a, node->name));
	sb_putc(a, out, ':');
	if (v->kind == V_EMPTY) { emit_trailing(a, out, node->trailing); sb_putc(a, out, '\n'); }
	else if (v->kind == V_CELL) {
		sb_putc(a, out, ' ');
		for (size_t i = 0; i < v->nels; i++) { if (i) sb_puts(a, out, ", "); sb_putS(a, out, emit_element(a, &v->els[i])); }
		emit_trailing(a, out, node->trailing);
		sb_putc(a, out, '\n');
	} else {
		if (would_merge) sb_putc(a, out, ' ');
		else { emit_trailing(a, out, node->trailing); sb_putc(a, out, '\n'); }
		if (!would_merge) for (size_t k = 0; k < depth + 1; k++) sb_putc(a, out, '\t');
		for (size_t k = 0; k < v->fence_len; k++) sb_putc(a, out, (char)v->fence_char);
		if (v->info.n > 0) { if ((unsigned char)v->info.p[0] == v->fence_char) sb_putc(a, out, ' '); sb_putS(a, out, v->info); }
		sb_putc(a, out, '\n');
		if (v->content.n > 0) {
			size_t start = 0;
			for (size_t i = 0; i <= v->content.n; i++) if (i == v->content.n || v->content.p[i] == '\n') {
				S l = s_slice(v->content, start, i);
				if (l.n > 0) for (size_t z = 0; z < depth + 1; z++) sb_putc(a, out, '\t');
				sb_putS(a, out, l); sb_putc(a, out, '\n');
				start = i + 1;
			}
		}
		for (size_t k = 0; k < depth + 1; k++) sb_putc(a, out, '\t');
		for (size_t k = 0; k < v->fence_len; k++) sb_putc(a, out, (char)v->fence_char);
		sb_putc(a, out, '\n');
	}
	VecSize ch = NODE(d, idx).children;
	emit_children(d, &ch, depth + 1, out);
	/* Comments this block owns with no child to carry them, one deeper. */
	for (size_t k = 0; k < NODE(d, idx).inside.len; k++) {
		Lead *c = &NODE(d, idx).inside.data[k];
		if (c->blank_before && out->len) sb_putc(a, out, '\n');
		for (size_t z = 0; z < depth + 1; z++) sb_putc(a, out, '\t');
		sb_putS(a, out, c->text); sb_putc(a, out, '\n');
	}
	/* Comments that hung on this block after its last child. */
	for (size_t k = 0; k < NODE(d, idx).after.len; k++) {
		Lead *c = &NODE(d, idx).after.data[k];
		if (c->blank_before && out->len) sb_putc(a, out, '\n');
		for (size_t z = 0; z < depth; z++) sb_putc(a, out, '\t');
		sb_putS(a, out, c->text); sb_putc(a, out, '\n');
	}
}
shcl_str shcl_to_canonical(shcl_doc *d) {
	SB out = {0};
	VecSize rc = NODE(d, ROOT).children;
	emit_children(d, &rc, 0, &out);
	/* Comments that never found a following line re-emit at the end. */
	for (size_t k = 0; k < d->orphans.len; k++) {
		if (d->orphans.data[k].blank_before && out.len) sb_putc(&d->arena, &out, '\n');
		sb_putS(&d->arena, &out, d->orphans.data[k].text); sb_putc(&d->arena, &out, '\n');
	}
	return sb_S(&out);
}

// --- format helpers + remaining public API ----------------------------------

size_t shcl_format_f64(double v, char *out) {
	if (isnan(v)) { memcpy(out, "NaN", 3); return 3; }
	if (isinf(v)) { if (v < 0) { memcpy(out, "-inf", 4); return 4; } memcpy(out, "inf", 3); return 3; }
	if (v == 0.0) { if (signbit(v)) { memcpy(out, "-0", 2); return 2; } out[0] = '0'; return 1; }
	char tmp[64]; int prec;
	for (prec = 1; prec <= 17; prec++) { snprintf(tmp, sizeof tmp, "%.*e", prec - 1, v); if (strtod(tmp, NULL) == v) break; }
	if (prec > 17) prec = 17;
	// The round-trip above needed tmp in the host locale; the scan below wants '.'.
	{
		const char *dp = dec_point(); size_t dn = strlen(dp);
		if (dn != 1 || *dp != '.') {
			char *q = strstr(tmp, dp);
			if (q) { *q = '.'; memmove(q + 1, q + dn, strlen(q + dn) + 1); }
		}
	}
	const char *s = tmp; int neg = 0;
	if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
	char digits[24]; int nd = 0;
	digits[nd++] = *s++;
	if (*s == '.') { s++; while (*s >= '0' && *s <= '9') digits[nd++] = *s++; }
	int E = 0;
	if (*s == 'e' || *s == 'E') { s++; int es = 1; if (*s == '-') { es = -1; s++; } else if (*s == '+') s++; while (*s >= '0' && *s <= '9') { E = E * 10 + (*s - '0'); s++; } E *= es; }
	int pointPos = E + 1;
	char *o = out; if (neg) *o++ = '-';
	if (pointPos <= 0) { *o++ = '0'; *o++ = '.'; for (int z = 0; z < -pointPos; z++) *o++ = '0'; for (int k = 0; k < nd; k++) *o++ = digits[k]; }
	else if (pointPos >= nd) { for (int k = 0; k < nd; k++) *o++ = digits[k]; for (int z = 0; z < pointPos - nd; z++) *o++ = '0'; }
	else { for (int k = 0; k < pointPos; k++) *o++ = digits[k]; *o++ = '.'; for (int k = pointPos; k < nd; k++) *o++ = digits[k]; }
	return (size_t)(o - out);
}
size_t shcl_datetime_str(const shcl_datetime *dt, char *out) {
	// Every field is public, so a hand-built struct can carry values parsing
	// never yields (a -1 sentinel, epoch seconds in sec): render whole into a
	// worst-case local buffer (~109 bytes), then clamp the copy to the
	// documented 64. Parsed values stay under the clamp (<= 56 with the frac
	// cap), so their output is unchanged.
	char b[128];
	char *o = b;
	if (dt->has_date) { o += sprintf(o, "%04d-%02u-%02u", dt->year, dt->month, dt->day); if (dt->has_time) *o++ = 'T'; }
	if (dt->has_time) {
		o += sprintf(o, "%02u:%02u", dt->hour, dt->minute);
		if (dt->has_sec) o += sprintf(o, ":%02u", dt->sec);
		if (dt->has_frac) {
			// frac keeps its own cap: the fixed parts plus 30 digits stay
			// inside the clamp for every parsed value.
			size_t fn = dt->frac.n > 30 ? 30 : dt->frac.n;
			*o++ = '.'; memcpy(o, dt->frac.p, fn); o += fn;
		}
	}
	if (dt->zone == SHCL_ZONE_UTC) *o++ = 'Z';
	else if (dt->zone == SHCL_ZONE_OFFSET) {
		// widen before negating: INT32_MIN has no 32-bit negation
		long long off = dt->off_min; char sign = off < 0 ? '-' : '+'; long long ao = off < 0 ? -off : off;
		o += sprintf(o, "%c%02lld:%02lld", sign, ao / 60, ao % 60);
	}
	size_t n = (size_t)(o - b);
	if (n > 64) n = 64;
	memcpy(out, b, n);
	return n;
}
int shcl_status_code(shcl_status s) {
	switch (s) { case SHCL_GOOD: return 0; case SHCL_EMPTY: return 2; case SHCL_NOT_FOUND: return 3; case SHCL_BAD_TYPE: return 4; case SHCL_MULTIPLE: return 5; }
	return 1;
}
const char *shcl_status_name(shcl_status s) {
	switch (s) { case SHCL_GOOD: return "Good"; case SHCL_EMPTY: return "Empty"; case SHCL_NOT_FOUND: return "NotFound"; case SHCL_BAD_TYPE: return "BadType"; case SHCL_MULTIPLE: return "Multiple"; }
	return "Good";
}
int shcl_strictness_from_arg(const char *s, size_t n, shcl_strictness *out) {
	char buf[16]; if (n >= sizeof buf) return 0;
	for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)s[i]; buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c; }
	buf[n] = '\0';
	if (!strcmp(buf, "loose") || !strcmp(buf, "1")) { *out = SHCL_LOOSE; return 1; }
	if (!strcmp(buf, "standard") || !strcmp(buf, "2")) { *out = SHCL_STANDARD; return 1; }
	if (!strcmp(buf, "strict") || !strcmp(buf, "3")) { *out = SHCL_STRICT; return 1; }
	return 0;
}

shcl_doc *shcl_parse(const char *text, size_t len) { return do_parse(text, len, SHCL_STANDARD); }
shcl_doc *shcl_parse_with(const char *text, size_t len, shcl_strictness s) { return do_parse(text, len, s); }
void shcl_free(shcl_doc *d) { if (!d) return; arena_free(&d->arena); arena_free(&d->scratch); free(d); }
int shcl_strict_failed(const shcl_doc *d) {
	if (d->strictness != SHCL_STRICT) return 0;
	for (size_t i = 0; i < d->diags.len; i++) if (d->diags.data[i].sev == SHCL_SEV_ERROR) return 1;
	return 0;
}
shcl_strictness shcl_strictness_of(const shcl_doc *d) { return d->strictness; }
size_t shcl_diag_count(const shcl_doc *d) { return d->diags.len; }
size_t shcl_diag_line(const shcl_doc *d, size_t i) { return d->diags.data[i].line; }
shcl_severity shcl_diag_severity(const shcl_doc *d, size_t i) { return d->diags.data[i].sev; }
shcl_str shcl_diag_message(const shcl_doc *d, size_t i) { return d->diags.data[i].message; }
const char *shcl_diag_code(const shcl_doc *d, size_t i) { return d->diags.data[i].code; }

// ===========================================================================
// Validator: schema-as-SHCL
// The schema is an ordinary parsed document: a flat list of `field: <path>`
// instances whose children are the constraints (closed vocabulary - see
// spec.md "Schema validation"). Validation reuses the accessor's path scan and
// the typed coercions, so document strictness composes for free. Schema faults
// (V09x) come first and the surviving constraints still check the document;
// the unknown-field sweep skips only when a fault cost a path spelling. One
// line-number space per result.
// Everything (scratch and results) lives in the validation's own arena.

struct shcl_validation { Arena arena; VecDiag diags; };

static const char *v_schema_types[] = {
	"int", "float", "bool", "string", "datetime", "raw",
	"int-array", "float-array", "bool-array", "string-array", "datetime-array",
};

typedef enum { ALLOW_INTS, ALLOW_FLOATS, ALLOW_BOOLS, ALLOW_DATES, ALLOW_STRINGS } allowkind;

typedef struct {
	S path; // as written in the schema; message text only
	VecSeg segs;
	const char *ty; // member of v_schema_types; NULL = untyped
	int required;
	int has_allowed; allowkind akind; size_t a_n;
	int64_t *a_ints; double *a_floats; int *a_bools; shcl_datetime *a_dates; S *a_strs;
	int has_min_i, has_max_i, has_min_f, has_max_f;
	int64_t min_i, max_i; double min_f, max_f;
	int has_repeat; uint64_t rep_lo, rep_hi;
	S inherits;           // fragment mounted at this path (subtree shape); .n == 0 = none
	size_t inherits_line; // schema line of the `inherits` key, for V095
	// Generator-only (`shcl init`): validation ignores both. has_* gates them.
	int has_desc; S desc;
	int has_default; S default_text;
} VCons;
DEFINE_VEC(VecVCons, VCons)

// An interpreted schema: the top-level constraints plus the named fragments
// their `inherits` keys can mount.
typedef struct { S name; VecVCons fields; } VFrag;
DEFINE_VEC(VecVFrag, VFrag)
// paths_complete: 0 when a fault cost the schema a path spelling (unreadable
// `field:` path, or a mount naming no declared fragment). Key-level faults
// keep their entry's chain, so only these two classes can turn declared
// fields into false unknowns - the sweep runs unless one of them happened.
typedef struct { VecVCons cons; VecVFrag frags; int paths_complete; } VSchemaDef;

static const VecVCons *v_frag_get(const VSchemaDef *def, S name) {
	for (size_t i = 0; i < def->frags.len; i++)
		if (s_eq(def->frags.data[i].name, name)) return &def->frags.data[i].fields;
	return NULL;
}

static void v_diag(Arena *a, VecDiag *out, size_t line, S msg) {
	Diag dg; dg.line = line; dg.sev = SHCL_SEV_ERROR; dg.message = msg; dg.code = diag_code(SHCL_SEV_ERROR, msg);
	VecDiag_push(a, out, dg);
}
static S v_msgz(Arena *a, const char *z) { S s; s.p = z; s.n = strlen(z); return s_dup(a, s); }
static S v_msg3(Arena *a, const char *pre, S mid, const char *post) {
	SB s = {0, 0, 0};
	sb_puts(a, &s, pre); sb_putS(a, &s, mid); sb_puts(a, &s, post);
	return sb_S(&s);
}
static S v_msg_key(Arena *a, const char *key) {
	SB s = {0, 0, 0};
	sb_puts(a, &s, "bad schema constraint '"); sb_puts(a, &s, key); sb_puts(a, &s, "'");
	return sb_S(&s);
}

// One scalar constraint value (escapes applied), or 0.
static int v_single_text(Arena *a, const Value *v, S *out) {
	if (v->kind != V_CELL || v->nels != 1) return 0;
	*out = apply_escapes(a, v->els[0].text);
	return 1;
}

// Field-wise datetime equality (struct compare would read unset fields).
static int v_dt_equal(const shcl_datetime *x, const shcl_datetime *y) {
	if (x->has_date != y->has_date || x->has_time != y->has_time) return 0;
	if (x->has_date && (x->year != y->year || x->month != y->month || x->day != y->day)) return 0;
	if (x->has_time) {
		if (x->hour != y->hour || x->minute != y->minute || x->has_sec != y->has_sec) return 0;
		if (x->has_sec && x->sec != y->sec) return 0;
	}
	if (x->has_frac != y->has_frac) return 0;
	if (x->has_frac) {
		S a; a.p = x->frac.p; a.n = x->frac.n;
		S b; b.p = y->frac.p; b.n = y->frac.n;
		if (!s_eq(a, b)) return 0;
	}
	if (x->zone != y->zone) return 0;
	if (x->zone == SHCL_ZONE_OFFSET && x->off_min != y->off_min) return 0;
	return 1;
}

// One `field:` instance (top-level or inside a fragment) -> a constraint into
// *out. Zero return = faults were reported and the constraint is dropped.
static int v_parse_field(Arena *a, shcl_doc *schema, size_t f, VecDiag *faults, VCons *out) {
	Node *node = &NODE(schema, f);
	S path;
	if (!v_single_text(a, &node->value, &path)) {
		v_diag(a, faults, node->line, v_msgz(a, "bad schema path"));
		return 0;
	}
	PathScan ps = scan_lookup(a, path);
	if (!ps.ok || ps.has_value) {
		v_diag(a, faults, node->line, v_msg3(a, "bad schema path: ", path, ""));
		return 0;
	}
	VCons c; memset(&c, 0, sizeof c);
	c.path = path; c.segs = ps.segs;
	// Deferred so `min: 1` may precede `type: int` in the file.
	int required = -1;
	int reopen_seen = 0;
	size_t allowed_at = (size_t)-1, min_at = (size_t)-1, max_at = (size_t)-1;
	VecSize kids = NODE(schema, f).children;
	for (size_t ki = 0; ki < kids.len; ki++) {
		Node *kid = &NODE(schema, kids.data[ki]);
		if (v_is_empty(&kid->value)) continue; // dangling key: treated as absent
		if (s_eq(kid->name, s_lit("type"))) {
			S t;
			int ok = v_single_text(a, &kid->value, &t);
			const char *canon = NULL;
			if (ok) {
				S low = ascii_lower(a, t);
				for (size_t x = 0; x < sizeof v_schema_types / sizeof v_schema_types[0]; x++)
					if (s_eq(low, s_lit(v_schema_types[x]))) { canon = v_schema_types[x]; break; }
				if (canon) {
					if (c.ty) v_diag(a, faults, kid->line, v_msg_key(a, "type"));
					else c.ty = canon;
				} else {
					v_diag(a, faults, kid->line, v_msg3(a, "unknown schema type '", low, "'"));
				}
			} else {
				v_diag(a, faults, kid->line, v_msg_key(a, "type"));
			}
		} else if (s_eq(kid->name, s_lit("required"))) {
			S t; int b = 0;
			int ok = v_single_text(a, &kid->value, &t) && parse_bool_text(a, t, SHCL_STANDARD, &b);
			if (ok && required < 0) required = b;
			else v_diag(a, faults, kid->line, v_msg_key(a, "required"));
		} else if (s_eq(kid->name, s_lit("reopen"))) {
			/* Consumed by the H002 suppressor (which reads the schema document
			   directly); validation itself ignores it, but a bad value still
			   faults so a typo cannot silently disavow nothing. */
			S t; int b = 0;
			int ok = v_single_text(a, &kid->value, &t) && parse_bool_text(a, t, SHCL_STANDARD, &b);
			if (ok && !reopen_seen) reopen_seen = 1;
			else v_diag(a, faults, kid->line, v_msg_key(a, "reopen"));
		} else if (s_eq(kid->name, s_lit("allowed"))) {
			if (kid->value.kind == V_CELL && allowed_at == (size_t)-1) allowed_at = kids.data[ki];
			else v_diag(a, faults, kid->line, v_msg_key(a, "allowed"));
		} else if (s_eq(kid->name, s_lit("min"))) {
			if (kid->value.kind == V_CELL && kid->value.nels == 1 && min_at == (size_t)-1) min_at = kids.data[ki];
			else v_diag(a, faults, kid->line, v_msg_key(a, "min"));
		} else if (s_eq(kid->name, s_lit("max"))) {
			if (kid->value.kind == V_CELL && kid->value.nels == 1 && max_at == (size_t)-1) max_at = kids.data[ki];
			else v_diag(a, faults, kid->line, v_msg_key(a, "max"));
		} else if (s_eq(kid->name, s_lit("repeat"))) {
			if (kid->value.kind == V_CELL && !c.has_repeat && (kid->value.nels == 1 || kid->value.nels == 2)) {
				uint64_t lo, hi;
				if (parse_u64(kid->value.els[0].text, &lo) && parse_u64(kid->value.els[kid->value.nels - 1].text, &hi) && lo <= hi) {
					c.has_repeat = 1; c.rep_lo = lo; c.rep_hi = hi;
				} else {
					v_diag(a, faults, kid->line, v_msg_key(a, "repeat"));
				}
			} else {
				v_diag(a, faults, kid->line, v_msg_key(a, "repeat"));
			}
		} else if (s_eq(kid->name, s_lit("inherits"))) {
			S t;
			if (v_single_text(a, &kid->value, &t) && t.n && c.inherits.n == 0) {
				c.inherits = t; c.inherits_line = kid->line;
			} else {
				v_diag(a, faults, kid->line, v_msg_key(a, "inherits"));
			}
		} else if (s_eq(kid->name, s_lit("desc"))) {
			// Generator-only (`shcl init`); validation ignores it. First wins.
			S t;
			if (!c.has_desc && v_single_text(a, &kid->value, &t)) { c.has_desc = 1; c.desc = t; }
		} else if (s_eq(kid->name, s_lit("default"))) {
			if (!c.has_default && kid->value.kind == V_CELL) {
				SB s = {0, 0, 0};
				for (size_t x = 0; x < kid->value.nels; x++) { if (x) sb_puts(a, &s, ", "); sb_putS(a, &s, emit_element(a, &kid->value.els[x])); }
				c.has_default = 1; c.default_text = sb_S(&s);
			}
		} else {
			v_diag(a, faults, kid->line, v_msg3(a, "unknown schema key '", kid->name, "'"));
		}
	}
	c.required = required > 0;
	const char *base = c.ty ? c.ty : "string";
	size_t blen = strlen(base);
	if (blen > 6 && memcmp(base + blen - 6, "-array", 6) == 0) {
		// Base kind name, without the -array suffix (still a literal member).
		for (size_t x = 0; x < sizeof v_schema_types / sizeof v_schema_types[0]; x++)
			if (strlen(v_schema_types[x]) == blen - 6 && memcmp(v_schema_types[x], base, blen - 6) == 0) { base = v_schema_types[x]; break; }
	}
	if (allowed_at != (size_t)-1) {
		Node *kid = &NODE(schema, allowed_at);
		Element *els = kid->value.els; size_t n = kid->value.nels;
		// Schema values are read at Standard; only the document's values
		// coerce at the document's strictness.
		int ok = 1;
		c.a_n = n;
		if (strcmp(base, "int") == 0) {
			c.akind = ALLOW_INTS;
			c.a_ints = (int64_t *)arena_alloc(a, (n ? n : 1) * sizeof(int64_t));
			for (size_t x = 0; x < n && ok; x++) ok = parse_int_text(a, &els[x], SHCL_STANDARD, &c.a_ints[x]);
		} else if (strcmp(base, "float") == 0) {
			c.akind = ALLOW_FLOATS;
			c.a_floats = (double *)arena_alloc(a, (n ? n : 1) * sizeof(double));
			for (size_t x = 0; x < n && ok; x++) ok = parse_float_text(a, &els[x], SHCL_STANDARD, &c.a_floats[x]);
		} else if (strcmp(base, "bool") == 0) {
			c.akind = ALLOW_BOOLS;
			c.a_bools = (int *)arena_alloc(a, (n ? n : 1) * sizeof(int));
			for (size_t x = 0; x < n && ok; x++) ok = parse_bool_text(a, els[x].text, SHCL_STANDARD, &c.a_bools[x]);
		} else if (strcmp(base, "datetime") == 0) {
			c.akind = ALLOW_DATES;
			c.a_dates = (shcl_datetime *)arena_alloc(a, (n ? n : 1) * sizeof(shcl_datetime));
			for (size_t x = 0; x < n && ok; x++) ok = parse_datetime(a, els[x].text, &c.a_dates[x]);
		} else if (strcmp(base, "raw") == 0) {
			ok = 0; // a raw body has no element space to enumerate
		} else {
			c.akind = ALLOW_STRINGS;
			c.a_strs = (S *)arena_alloc(a, (n ? n : 1) * sizeof(S));
			for (size_t x = 0; x < n; x++) c.a_strs[x] = apply_escapes(a, els[x].text);
		}
		if (ok) c.has_allowed = 1;
		else v_diag(a, faults, kid->line, v_msg_key(a, "allowed"));
	}
	for (int mm = 0; mm < 2; mm++) {
		int is_min = mm == 0;
		size_t at = is_min ? min_at : max_at;
		if (at == (size_t)-1) continue;
		Node *kid = &NODE(schema, at);
		Element *el = &kid->value.els[0];
		const char *key = is_min ? "min" : "max";
		if (strcmp(base, "int") == 0) {
			int64_t v;
			if (parse_int_text(a, el, SHCL_STANDARD, &v)) {
				if (is_min) { c.has_min_i = 1; c.min_i = v; }
				else { c.has_max_i = 1; c.max_i = v; }
			} else v_diag(a, faults, kid->line, v_msg_key(a, key));
		} else if (strcmp(base, "float") == 0) {
			double v;
			if (parse_float_text(a, el, SHCL_STANDARD, &v)) {
				if (is_min) { c.has_min_f = 1; c.min_f = v; }
				else { c.has_max_f = 1; c.max_f = v; }
			} else v_diag(a, faults, kid->line, v_msg_key(a, key));
		} else {
			v_diag(a, faults, kid->line, v_msg_key(a, key));
		}
	}
	*out = c;
	return 1;
}

// Interpret a parsed schema document into constraints and fragments, plus any
// schema faults (V09x, schema-file lines). Whatever parsed cleanly is kept
// even when faults are present - a broken key drops that key, a broken field
// drops that field - so a caller can still check the document against the
// surviving constraints.
static void v_build_schema(Arena *a, shcl_doc *schema, VSchemaDef *def, VecDiag *faults) {
	def->paths_complete = 1;
	VecSize top = NODE(schema, ROOT).children;
	for (size_t fi = 0; fi < top.len; fi++) {
		Node *node = &NODE(schema, top.data[fi]);
		if (s_eq(node->name, s_lit("field"))) {
			VCons c;
			if (v_parse_field(a, schema, top.data[fi], faults, &c)) VecVCons_push(a, &def->cons, c);
			else def->paths_complete = 0;
		} else if (s_eq(node->name, s_lit("fragment"))) {
			S name;
			if (!v_single_text(a, &node->value, &name) || name.n == 0) {
				v_diag(a, faults, node->line, v_msgz(a, "bad schema fragment"));
				continue;
			}
			if (v_frag_get(def, name)) {
				v_diag(a, faults, node->line, v_msg3(a, "bad schema fragment '", name, "': duplicate"));
				continue;
			}
			VFrag fr; fr.name = name; memset(&fr.fields, 0, sizeof fr.fields);
			VecSize kids = NODE(schema, top.data[fi]).children;
			for (size_t ki = 0; ki < kids.len; ki++) {
				Node *kid = &NODE(schema, kids.data[ki]);
				if (s_eq(kid->name, s_lit("field"))) {
					VCons c;
					if (v_parse_field(a, schema, kids.data[ki], faults, &c)) VecVCons_push(a, &fr.fields, c);
					else def->paths_complete = 0;
				} else {
					SB s = {0, 0, 0};
					sb_puts(a, &s, "bad schema fragment '"); sb_putS(a, &s, name);
					sb_puts(a, &s, "': unknown key '"); sb_putS(a, &s, kid->name); sb_puts(a, &s, "'");
					v_diag(a, faults, kid->line, sb_S(&s));
				}
			}
			VecVFrag_push(a, &def->frags, fr);
		} else {
			v_diag(a, faults, node->line, v_msg3(a, "unknown schema key '", node->name, "'"));
		}
	}
	// Every mount must name a declared fragment; cycles (self or mutual) are
	// legal - expansion is demand-driven against a finite document.
	for (size_t g = 0; g <= def->frags.len; g++) {
		const VecVCons *list = g == 0 ? &def->cons : &def->frags.data[g - 1].fields;
		for (size_t i = 0; i < list->len; i++) {
			const VCons *c = &list->data[i];
			if (c->inherits.n && !v_frag_get(def, c->inherits)) {
				v_diag(a, faults, c->inherits_line, v_msg3(a, "unknown schema fragment '", c->inherits, "'"));
				def->paths_complete = 0;
			}
		}
	}
	// One constraint per line in practice, so line order = file order. Insertion
	// sort keeps equal lines stable (qsort is not stable).
	for (size_t i = 1; i < faults->len; i++) {
		Diag key = faults->data[i];
		size_t j = i;
		while (j > 0 && faults->data[j - 1].line > key.line) { faults->data[j] = faults->data[j - 1]; j--; }
		faults->data[j] = key;
	}
}

// Two-row Levenshtein over codepoints; powers the "did you mean" prose (never
// the code).
static size_t v_edit_distance(Arena *a, S sa, S sb) {
	CPs ca = decode_cps(a, sa);
	CPs cb = decode_cps(a, sb);
	size_t *prev = (size_t *)arena_alloc(a, (cb.n + 1) * sizeof(size_t));
	size_t *cur = (size_t *)arena_alloc(a, (cb.n + 1) * sizeof(size_t));
	for (size_t j = 0; j <= cb.n; j++) prev[j] = j;
	for (size_t i = 1; i <= ca.n; i++) {
		cur[0] = i;
		for (size_t j = 1; j <= cb.n; j++) {
			size_t cost = ca.cp[i - 1] == cb.cp[j - 1] ? 0 : 1;
			size_t m = prev[j] + 1;
			if (cur[j - 1] + 1 < m) m = cur[j - 1] + 1;
			if (prev[j - 1] + cost < m) m = prev[j - 1] + cost;
			cur[j] = m;
		}
		size_t *t = prev; prev = cur; cur = t;
	}
	return prev[cb.n];
}

// Closest legal sibling name (same parent chain, schema order, edit distance
// <= 2) appended as "; did you mean 'x'?" - or nothing. Prose only.
static void v_suggest(Arena *a, Arena *tmp, const VecS *names, S name, SB *msg) {
	/* tmp holds the DP rows and codepoint decodes - dead after this call.
	   Resetting per unknown field keeps a wholesale unmatched document (the
	   case this feature exists for) at one sweep's peak. The sibling lists
	   are prebuilt once per validate by v_unknown. */
	arena_reset(tmp);
	if (!names) return;
	int have = 0; size_t best_dist = 0; S best_name = s_empty();
	for (size_t i = 0; i < names->len; i++) {
		size_t dist = v_edit_distance(tmp, name, names->data[i]);
		if (dist <= 2 && (!have || dist < best_dist)) { have = 1; best_dist = dist; best_name = names->data[i]; }
	}
	if (have) {
		sb_puts(a, msg, "; did you mean '");
		sb_putS(a, msg, best_name);
		sb_puts(a, msg, "'?");
	}
}

// Resolution contexts: the whole document for a plain path; each enclosing
// instance for the part of a path after a wildcard. required/repeat evaluate
// per context (anchor line 0 = document scope), so `server[*].port` + required
// means a port under EACH server - vacuously true with no servers.
typedef struct { size_t anchor; VecSize found; } VCtx;
DEFINE_VEC(VecVCtx, VCtx)

static void v_contexts(Arena *a, shcl_doc *d, const size_t *start, size_t nstart, Segment *segs, size_t nsegs, size_t anchor, VecVCtx *out) {
	VecSize cur = {0};
	for (size_t i = 0; i < nstart; i++) VecSize_push(a, &cur, start[i]);
	for (size_t si = 0; si < nsegs; si++) {
		Segment *seg = &segs[si];
		VecSize next = {0};
		for (size_t k = 0; k < cur.len; k++) {
			VecSize ch = NODE(d, cur.data[k]).children;
			if (seg->star) {
				for (size_t j = 0; j < ch.len; j++) VecSize_push(a, &next, ch.data[j]);
			} else {
				for (size_t j = 0; j < ch.len; j++) { size_t c = ch.data[j]; if (s_eq(NODE(d, c).name, seg->name)) VecSize_push(a, &next, c); }
			}
		}
		if (seg->star) {
			// Name wildcard: same per-instance split as `[*]`, any child name.
			Segment *rest = segs + si + 1; size_t nrest = nsegs - si - 1;
			if (nrest == 0) {
				VCtx ctx; ctx.anchor = anchor; ctx.found = next; VecVCtx_push(a, out, ctx);
			} else {
				for (size_t k = 0; k < next.len; k++) {
					size_t inst = next.data[k];
					v_contexts(a, d, &inst, 1, rest, nrest, NODE(d, inst).line, out);
				}
			}
			return;
		}
		switch (seg->sel.tag) {
		case SEL_NONE: cur = next; break;
		case SEL_VALUE: {
			VecSize f = {0};
			S want = apply_escapes(a, seg->sel.value);
			for (size_t k = 0; k < next.len; k++) if (s_eq(disp_key(a, &NODE(d, next.data[k]).value), want) && (!seg->sel.quoted || single_scalar(&NODE(d, next.data[k]).value))) VecSize_push(a, &f, next.data[k]);
			cur = f; break;
		}
		case SEL_INDEX: {
			VecSize f = {0};
			if (seg->sel.index < next.len) VecSize_push(a, &f, next.data[seg->sel.index]);
			cur = f; break;
		}
		case SEL_WILDCARD: {
			Segment *rest = segs + si + 1; size_t nrest = nsegs - si - 1;
			if (nrest == 0) {
				VCtx ctx; ctx.anchor = anchor; ctx.found = next; VecVCtx_push(a, out, ctx);
			} else {
				for (size_t k = 0; k < next.len; k++) {
					size_t inst = next.data[k];
					v_contexts(a, d, &inst, 1, rest, nrest, NODE(d, inst).line, out);
				}
			}
			return;
		}
		}
	}
	VCtx ctx; ctx.anchor = anchor; ctx.found = cur; VecVCtx_push(a, out, ctx);
}

static void v_wrong_type(Arena *a, VecDiag *out, size_t line, const VCons *c) {
	SB s = {0, 0, 0};
	sb_puts(a, &s, "wrong type at '"); sb_putS(a, &s, c->path);
	sb_puts(a, &s, "': value is not a valid "); sb_puts(a, &s, c->ty ? c->ty : "string");
	v_diag(a, out, line, sb_S(&s));
}
static void v_not_allowed(Arena *a, VecDiag *out, size_t line, const VCons *c, S text) {
	SB s = {0, 0, 0};
	sb_puts(a, &s, "value not allowed at '"); sb_putS(a, &s, c->path);
	sb_puts(a, &s, "': "); sb_putS(a, &s, text);
	v_diag(a, out, line, sb_S(&s));
}

// Diagnostic messages go to a (they outlive the walk); coercion temporaries
// and compare strings go to lv, the walk level's scratch.
static void v_node(Arena *a, Arena *lv, shcl_doc *d, const VCons *c, size_t n, VecDiag *out) {
	Node *node = &NODE(d, n);
	size_t line = node->line;
	const char *ty = c->ty;
	size_t tlen = ty ? strlen(ty) : 0;
	int is_array = ty && tlen > 6 && memcmp(ty + tlen - 6, "-array", 6) == 0;
	size_t blen = is_array ? tlen - 6 : tlen;
	// base kind compare helper against a literal
	#define V_BASE_IS(z) (ty && blen == strlen(z) && memcmp(ty, z, blen) == 0)
	if (node->value.kind == V_EMPTY) {
		// Empty passes everything; required already counted it as present.
		return;
	}
	if (node->value.kind == V_RAW) {
		// A raw block satisfies `raw` and scalar `string` (any value reads as a
		// string); every other kind is a type miss.
		if (ty && ((!V_BASE_IS("raw") && !V_BASE_IS("string")) || is_array)) { v_wrong_type(a, out, line, c); return; }
		if (c->has_allowed && c->akind == ALLOW_STRINGS) {
			int found = 0;
			for (size_t x = 0; x < c->a_n; x++) if (s_eq(c->a_strs[x], node->value.content)) { found = 1; break; }
			if (!found) v_not_allowed(a, out, line, c, node->value.content);
		}
		return;
	}
	Element *els = node->value.els; size_t nels = node->value.nels;
	if (V_BASE_IS("raw")) { v_wrong_type(a, out, line, c); return; }
	// A scalar kind on a multi-element value is the array-where-one-scalar-
	// expected miss - except string, which reads arrays.
	if (ty && !is_array && !V_BASE_IS("string") && nels > 1) { v_wrong_type(a, out, line, c); return; }
	if (V_BASE_IS("int")) {
		int64_t *vals = (int64_t *)arena_alloc(lv, (nels ? nels : 1) * sizeof(int64_t));
		for (size_t x = 0; x < nels; x++)
			if (!parse_int_text(lv, &els[x], d->strictness, &vals[x])) { v_wrong_type(a, out, line, c); return; }
		if (c->has_allowed && c->akind == ALLOW_INTS) {
			for (size_t x = 0; x < nels; x++) {
				int found = 0;
				for (size_t y = 0; y < c->a_n; y++) if (c->a_ints[y] == vals[x]) { found = 1; break; }
				if (!found) { v_not_allowed(a, out, line, c, els[x].text); break; }
			}
		}
		if (c->has_min_i) { for (size_t x = 0; x < nels; x++) if (vals[x] < c->min_i) { v_diag(a, out, line, v_msg3(a, "value below min at '", c->path, "'")); break; } }
		if (c->has_max_i) { for (size_t x = 0; x < nels; x++) if (vals[x] > c->max_i) { v_diag(a, out, line, v_msg3(a, "value above max at '", c->path, "'")); break; } }
	} else if (V_BASE_IS("float")) {
		double *vals = (double *)arena_alloc(lv, (nels ? nels : 1) * sizeof(double));
		for (size_t x = 0; x < nels; x++)
			if (!parse_float_text(lv, &els[x], d->strictness, &vals[x])) { v_wrong_type(a, out, line, c); return; }
		if (c->has_allowed && c->akind == ALLOW_FLOATS) {
			for (size_t x = 0; x < nels; x++) {
				int found = 0;
				for (size_t y = 0; y < c->a_n; y++) if (c->a_floats[y] == vals[x]) { found = 1; break; }
				if (!found) { v_not_allowed(a, out, line, c, els[x].text); break; }
			}
		}
		if (c->has_min_f) { for (size_t x = 0; x < nels; x++) if (vals[x] < c->min_f) { v_diag(a, out, line, v_msg3(a, "value below min at '", c->path, "'")); break; } }
		if (c->has_max_f) { for (size_t x = 0; x < nels; x++) if (vals[x] > c->max_f) { v_diag(a, out, line, v_msg3(a, "value above max at '", c->path, "'")); break; } }
	} else if (V_BASE_IS("bool")) {
		int *vals = (int *)arena_alloc(lv, (nels ? nels : 1) * sizeof(int));
		for (size_t x = 0; x < nels; x++)
			if (!parse_bool_text(lv, els[x].text, d->strictness, &vals[x])) { v_wrong_type(a, out, line, c); return; }
		if (c->has_allowed && c->akind == ALLOW_BOOLS) {
			for (size_t x = 0; x < nels; x++) {
				int found = 0;
				for (size_t y = 0; y < c->a_n; y++) if ((c->a_bools[y] != 0) == (vals[x] != 0)) { found = 1; break; }
				if (!found) { v_not_allowed(a, out, line, c, els[x].text); break; }
			}
		}
	} else if (V_BASE_IS("datetime")) {
		shcl_datetime *vals = (shcl_datetime *)arena_alloc(lv, (nels ? nels : 1) * sizeof(shcl_datetime));
		for (size_t x = 0; x < nels; x++)
			if (!parse_datetime(lv, els[x].text, &vals[x])) { v_wrong_type(a, out, line, c); return; }
		if (c->has_allowed && c->akind == ALLOW_DATES) {
			for (size_t x = 0; x < nels; x++) {
				int found = 0;
				for (size_t y = 0; y < c->a_n; y++) if (v_dt_equal(&c->a_dates[y], &vals[x])) { found = 1; break; }
				if (!found) { v_not_allowed(a, out, line, c, els[x].text); break; }
			}
		}
	} else {
		// string kind or untyped: every element coerces; only the allowed set
		// can fail, in logical-string space.
		if (c->has_allowed && c->akind == ALLOW_STRINGS) {
			for (size_t x = 0; x < nels; x++) {
				S s = apply_escapes(lv, els[x].text);
				int found = 0;
				for (size_t y = 0; y < c->a_n; y++) if (s_eq(c->a_strs[y], s)) { found = 1; break; }
				if (!found) { v_not_allowed(a, out, line, c, s); break; }
			}
		}
	}
	#undef V_BASE_IS
}

// A mounted fragment's fields run per resolved node, right after that node's
// own checks, in fragment order - depth-first, so diagnostic order stays
// derivable. Termination is structural: every mount descends at least one
// document level, and the document is finite (depth capped at 512, so this C
// stack recursion is safe - same rationale as v_contexts' own).
// lv is this level's scratch arena (next slot in the caller's per-level pool);
// resetting it at entry reuses the previous sibling call's block instead of
// retaining every level's temporaries in the validation arena until it is
// freed. Level L's contexts stay live in lv while deeper levels run in lv+1.
static void v_check_from(Arena *a, Arena *lv, shcl_doc *d, const VCons *c, const VSchemaDef *def, size_t start, size_t anchor0, VecDiag *out, CMap *mounted) {
	arena_reset(lv);
	VecVCtx ctxs = {0};
	v_contexts(lv, d, &start, 1, c->segs.data, c->segs.len, anchor0, &ctxs);
	for (size_t i = 0; i < ctxs.len; i++) {
		VCtx *ctx = &ctxs.data[i];
		if (c->required && ctx->found.len == 0)
			v_diag(a, out, ctx->anchor, v_msg3(a, "required path missing: ", c->path, ""));
		if (c->has_repeat) {
			uint64_t n = (uint64_t)ctx->found.len;
			if (n < c->rep_lo || n > c->rep_hi) {
				SB s = {0, 0, 0};
				sb_puts(a, &s, "instance count out of bounds at '"); sb_putS(a, &s, c->path);
				sb_puts(a, &s, "': "); sb_put_u64(a, &s, n);
				sb_puts(a, &s, " not in "); sb_put_u64(a, &s, c->rep_lo);
				sb_puts(a, &s, ".."); sb_put_u64(a, &s, c->rep_hi);
				v_diag(a, out, ctx->anchor, sb_S(&s));
			}
		}
		for (size_t k = 0; k < ctx->found.len; k++) {
			size_t n = ctx->found.data[k];
			v_node(a, lv, d, c, n, out);
			if (c->inherits.n) {
				const VecVCons *fcs = v_frag_get(def, c->inherits);
				if (fcs) {
					// Two constraints can resolve to the same node and mount the
					// same fragment there. The second mount would repeat the
					// first's work and its diagnostics, and repeating it per
					// level is what makes a recursive schema cost double per
					// document level, so each pair is done once.
					char kb[sizeof n]; memcpy(kb, &n, sizeof n);
					S nkey; nkey.p = kb; nkey.n = sizeof n;
					uint64_t h = cmap_hash(c->inherits, nkey);
					if (cmap_get(mounted, h, c->inherits, nkey) == (size_t)-1) {
						cmap_put(a, mounted, h, c->inherits, s_dup(a, nkey), 1);
						for (size_t fi = 0; fi < fcs->len; fi++)
							v_check_from(a, lv + 1, d, &fcs->data[fi], def, n, NODE(d, n).line, out, mounted);
					}
				}
			}
		}
	}
}

static void v_check(Arena *a, Arena *lvls, shcl_doc *d, const VCons *c, const VSchemaDef *def, VecDiag *out) {
	// (fragment, node) pairs already mounted during this constraint's walk;
	// entries live in the validation arena, so the set needs no own teardown.
	CMap mounted; memset(&mounted, 0, sizeof mounted);
	v_check_from(a, lvls, d, c, def, ROOT, 0, out, &mounted);
}

// Append a segment to a chain key. Chain keys join segments length-prefixed
// (`<len>:<name>`), not with a bare NUL: NUL is legal in a quoted name, so a
// single field named "x\0y" would impersonate the two-segment path x.y. Same
// injectivity reasoning as the merge key's cell encoding - and like it, the
// length unit is each binding's native one (bytes here), because only
// injectivity matters.
static void chain_push(Arena *a, SB *chain, S name) {
	char buf[32];
	snprintf(buf, sizeof buf, "%zu:", name.n);
	sb_puts(a, chain, buf);
	sb_putS(a, chain, name);
}

// Decode the next length-prefixed segment of a chain key at *i. Total: bails
// at the first shape the encoder can't have produced.
static int chain_next(S chain, size_t *i, S *nm) {
	size_t k = *i, n = 0;
	if (k >= chain.n) return 0;
	while (k < chain.n && chain.p[k] >= '0' && chain.p[k] <= '9') { n = n * 10 + (size_t)(chain.p[k] - '0'); k++; }
	if (k >= chain.n || chain.p[k] != ':' || k + 1 + n > chain.n) return 0;
	k++;
	*nm = s_slice(chain, k, k + n);
	*i = k + n;
	return 1;
}

// Element-wise chain match against the star-bearing schema paths: a `*`
// segment matches any one name, and every prefix of a path is legal.
static int star_legal(const VecSeg *pats, size_t npats, S chain) {
	if (npats == 0) return 0;
	for (size_t pi = 0; pi < npats; pi++) {
		const VecSeg *p = &pats[pi];
		size_t part = 0, i = 0; int match = 1;
		S nm;
		while (match && chain_next(chain, &i, &nm)) {
			if (part >= p->len || (!p->data[part].star && !s_eq(p->data[part].name, nm))) match = 0;
			part++;
		}
		if (match) return 1;
	}
	return 0;
}

// Chain legality through fragment mounts: the general matcher - element-wise
// like star_legal (stars wild, prefixes legal), and when a mount's whole path
// matched with chain left over, the remainder is retried against the mounted
// fragment's fields. Terminates: every descent consumes >= 1 part.
static int chain_parts_legal(const VecVCons *cons, const VSchemaDef *def, S chain, size_t from) {
	for (size_t ci = 0; ci < cons->len; ci++) {
		const VCons *c = &cons->data[ci];
		size_t n = c->segs.len;
		size_t part = 0, i = from, rem = from;
		int match = 1;
		S nm;
		while (match && chain_next(chain, &i, &nm)) {
			if (part < n) {
				if (!c->segs.data[part].star && !s_eq(c->segs.data[part].name, nm)) match = 0;
				if (part + 1 == n) rem = i; // remainder starts past the matched prefix
			}
			part++;
		}
		if (!match) continue;
		if (part <= n) return 1; // a prefix of a legal path
		if (c->inherits.n) {
			const VecVCons *fcs = v_frag_get(def, c->inherits);
			if (fcs && chain_parts_legal(fcs, def, chain, rem)) return 1;
		}
	}
	return 0;
}
static int chain_legal(const VSchemaDef *def, S chain) {
	return chain_parts_legal(&def->cons, def, chain, 0);
}

// Unknown-field sweep: a schema path legalizes its name chain and every prefix
// (selectors ignored). Only the topmost unknown node is reported; its subtree
// is implied unknown and skipped.
static void v_unknown(Arena *a, shcl_doc *d, const VSchemaDef *def, VecDiag *out) {
	const VecVCons *cons = &def->cons;
	// Chains below a fragment mount only match by descending the mounts.
	int has_mounts = 0;
	for (size_t i = 0; i < cons->len; i++) if (cons->data[i].inherits.n) { has_mounts = 1; break; }
	Arena tmp; tmp.head = NULL; // v_suggest scratch, reset per unknown field
	// Legal chains in a hash set (the linear scan compounded the quadratic),
	// and sibling names bucketed per parent chain, built once: v_suggest used
	// to rebuild every chain per unknown field.
	CMap legal; memset(&legal, 0, sizeof legal);
	CMap sib_of; memset(&sib_of, 0, sizeof sib_of);
	VecS *sibs = NULL; size_t nsib = 0, csib = 0;
	// Paths with a `*` segment can't live in the exact-chain hash; they
	// match element-wise (a star matches any one name, prefixes included).
	VecSeg *star_pats = NULL; size_t nstar = 0, cstar = 0;
	for (size_t i = 0; i < cons->len; i++) {
		int has_star = 0;
		for (size_t si = 0; si < cons->data[i].segs.len; si++) if (cons->data[i].segs.data[si].star) { has_star = 1; break; }
		if (has_star) {
			if (nstar == cstar) { size_t nc = cstar ? cstar * 2 : 8; star_pats = (VecSeg *)arena_grow(a, star_pats, cstar, nc, sizeof(VecSeg)); cstar = nc; }
			star_pats[nstar++] = cons->data[i].segs;
		}
		SB chain = {0, 0, 0};
		for (size_t si = 0; si < cons->data[i].segs.len; si++) {
			if (cons->data[i].segs.data[si].star) break; // no sibling entry for '*'; deeper chains are pattern-only
			S nm = cons->data[i].segs.data[si].name;
			S pc = s_dup(a, sb_S(&chain));
			uint64_t hp = cmap_hash(pc, s_empty());
			size_t g = cmap_get(&sib_of, hp, pc, s_empty());
			if (g == (size_t)-1) {
				if (nsib == csib) { size_t nc = csib ? csib * 2 : 8; sibs = (VecS *)arena_grow(a, sibs, csib, nc, sizeof(VecS)); csib = nc; }
				memset(&sibs[nsib], 0, sizeof sibs[nsib]);
				g = nsib++;
				cmap_put(a, &sib_of, hp, pc, s_empty(), g);
			}
			VecS_push(a, &sibs[g], nm);
			chain_push(a, &chain, nm);
			S full = s_dup(a, sb_S(&chain));
			uint64_t hf = cmap_hash(full, s_empty());
			if (cmap_get(&legal, hf, full, s_empty()) == (size_t)-1) cmap_put(a, &legal, hf, full, s_empty(), 1);
		}
	}
	VecSize snode = {0}; VecS schain = {0}; VecS sshown = {0};
	VecSize top = NODE(d, ROOT).children;
	for (size_t i = top.len; i > 0; i--) {
		VecSize_push(a, &snode, top.data[i - 1]);
		VecS_push(a, &schain, s_empty());
		VecS_push(a, &sshown, s_empty());
	}
	while (snode.len) {
		size_t n = snode.data[snode.len - 1];
		S pchain = schain.data[snode.len - 1];
		S pshown = sshown.data[snode.len - 1];
		snode.len--; schain.len--; sshown.len--;
		Node *node = &NODE(d, n);
		SB cb = {0, 0, 0};
		sb_putS(a, &cb, pchain);
		chain_push(a, &cb, node->name);
		S chain = sb_S(&cb);
		SB sb2 = {0, 0, 0};
		if (pshown.n) { sb_putS(a, &sb2, pshown); sb_putc(a, &sb2, '.'); }
		sb_putS(a, &sb2, node->name);
		S shown = sb_S(&sb2);
		int found = cmap_get(&legal, cmap_hash(chain, s_empty()), chain, s_empty()) != (size_t)-1;
		if (!found && !star_legal(star_pats, nstar, chain) && !(has_mounts && chain_legal(def, chain))) {
			SB msg = {0, 0, 0};
			sb_puts(a, &msg, "unknown field '"); sb_putS(a, &msg, shown); sb_puts(a, &msg, "'");
			size_t sg = cmap_get(&sib_of, cmap_hash(pchain, s_empty()), pchain, s_empty());
			v_suggest(a, &tmp, sg == (size_t)-1 ? NULL : &sibs[sg], node->name, &msg);
			v_diag(a, out, node->line, sb_S(&msg));
			continue;
		}
		VecSize ch = node->children;
		for (size_t i = ch.len; i > 0; i--) {
			VecSize_push(a, &snode, ch.data[i - 1]);
			VecS_push(a, &schain, chain);
			VecS_push(a, &sshown, shown);
		}
	}
	arena_free(&tmp);
}

shcl_validation *shcl_validate(shcl_doc *d, shcl_doc *schema) {
	shcl_validation *v = (shcl_validation *)malloc(sizeof *v);
	if (!v) { fprintf(stderr, "shcl: out of memory\n"); exit(70); }
	memset(v, 0, sizeof *v);
	Arena *a = &v->arena;
	VSchemaDef def; memset(&def, 0, sizeof def);
	VecDiag faults = {0};
	v_build_schema(a, schema, &def, &faults);
	v->diags = faults;
	// One scratch arena per mount-recursion level, reset and reused across
	// sibling calls: peak retention is one block per active document level,
	// not every level of every walk. The parser caps depth at SHCL_MAX_DEPTH
	// and every mount starts at least one level deeper, so the pool cannot be
	// outrun; untouched slots never allocate.
	Arena lvls[SHCL_MAX_DEPTH + 1];
	memset(lvls, 0, sizeof lvls);
	for (size_t i = 0; i < def.cons.len; i++) v_check(a, lvls, d, &def.cons.data[i], &def, &v->diags);
	for (size_t i = 0; i <= SHCL_MAX_DEPTH; i++) arena_free(&lvls[i]);
	if (def.paths_complete) v_unknown(a, d, &def, &v->diags);
	return v;
}
size_t shcl_validation_count(const shcl_validation *v) { return v->diags.len; }
size_t shcl_validation_line(const shcl_validation *v, size_t i) { return v->diags.data[i].line; }
shcl_severity shcl_validation_severity(const shcl_validation *v, size_t i) { return v->diags.data[i].sev; }
shcl_str shcl_validation_message(const shcl_validation *v, size_t i) {
	shcl_str s; s.p = v->diags.data[i].message.p; s.n = v->diags.data[i].message.n; return s;
}
const char *shcl_validation_code(const shcl_validation *v, size_t i) { return v->diags.data[i].code; }
void shcl_validation_free(shcl_validation *v) { if (!v) return; arena_free(&v->arena); free(v); }

void shcl_suppress_declared_repeats(shcl_doc *schema, shcl_doc *doc) {
	/* Everything this probe builds - instance/repeat query results as well as
	   the collected names (which must survive the per-read scratch resets) -
	   goes into its own arena, freed on exit: the function owns neither doc,
	   so it must not leave allocations behind in either. */
	Arena tmp; tmp.head = NULL;
	VecS names = {0};
	/* Top-level fields plus every fragment's fields: a repeat declared inside
	   a mounted shape disavows the hint the same way. */
	size_t nfrag = shcl_count(schema, "fragment", 8);
	for (size_t g = 0; g <= nfrag; g++) {
		char base[48];
		int bn = g == 0 ? snprintf(base, sizeof base, "field")
		                : snprintf(base, sizeof base, "fragment[#%zu].field", g - 1);
		shcl_str *paths;
		S bp; bp.p = base; bp.n = (size_t)bn;
		size_t np = instances_in(schema, &tmp, bp, &paths);
		for (size_t i = 0; i < np; i++) {
			char q[80];
			int qn = snprintf(q, sizeof q, "%s[#%zu].repeat", base, i);
			/* repeat is a 1-2 element array (`repeat: lo[, hi]`); the bound
			   that matters here is the last one. */
			S qp; qp.p = q; qp.n = (size_t)qn;
			shcl_read_i64_arr rep = read_int_array_in(schema, &tmp, qp);
			if (rep.status != SHCL_GOOD || rep.n == 0 || rep.values[rep.n - 1] <= 1) continue;
			S p; p.p = paths[i].p; p.n = paths[i].n;
			/* Leaf name from the parsed path, not a re-split of its text: a
			   quoted last segment may contain dots (`a."b.c"`). The scanner
			   folds the name; the doc side stores names folded too. */
			PathScan ps = scan_lookup(&tmp, p);
			if (!ps.ok || ps.segs.len == 0) continue;
			Segment *last = &ps.segs.data[ps.segs.len - 1];
			if (last->star) continue; /* name wildcard: no single leaf name to disavow */
			if (last->name.n) VecS_push(&tmp, &names, last->name);
		}
	}
	if (!names.len) { arena_free(&tmp); return; }
	VecS heads = {0};
	for (size_t k = 0; k < names.len; k++) VecS_push(&tmp, &heads, h001_head(&tmp, names.data[k]));
	size_t w = 0;
	for (size_t i = 0; i < doc->diags.len; i++) {
		Diag dg = doc->diags.data[i];
		int drop = 0;
		if (strcmp(dg.code, "H001") == 0) {
			S m = dg.message;
			for (size_t k = 0; k < heads.len; k++) {
				S h = heads.data[k];
				if (m.n >= h.n && memcmp(m.p, h.p, h.n) == 0) { drop = 1; break; }
			}
		}
		if (!drop) doc->diags.data[w++] = dg;
	}
	doc->diags.len = w;
	arena_free(&tmp);
}

void shcl_suppress_declared_reopens(shcl_doc *schema, shcl_doc *doc) {
	/* Same arena discipline as the H001 suppressor above. */
	Arena tmp; tmp.head = NULL;
	VecS names = {0};
	size_t nfrag = shcl_count(schema, "fragment", 8);
	for (size_t g = 0; g <= nfrag; g++) {
		char base[48];
		int bn = g == 0 ? snprintf(base, sizeof base, "field")
		                : snprintf(base, sizeof base, "fragment[#%zu].field", g - 1);
		shcl_str *paths;
		S bp; bp.p = base; bp.n = (size_t)bn;
		size_t np = instances_in(schema, &tmp, bp, &paths);
		for (size_t i = 0; i < np; i++) {
			char q[80];
			int qn = snprintf(q, sizeof q, "%s[#%zu].reopen", base, i);
			shcl_read_bool re = shcl_read_bool_(schema, q, (size_t)qn);
			if (re.status != SHCL_GOOD || !re.value) continue;
			S p; p.p = paths[i].p; p.n = paths[i].n;
			PathScan ps = scan_lookup(&tmp, p);
			if (!ps.ok || ps.segs.len == 0) continue;
			Segment *last = &ps.segs.data[ps.segs.len - 1];
			if (last->star) continue; /* name wildcard: no single leaf name to disavow */
			if (last->name.n) VecS_push(&tmp, &names, last->name);
		}
	}
	if (!names.len) { arena_free(&tmp); return; }
	VecS heads = {0};
	for (size_t k = 0; k < names.len; k++) VecS_push(&tmp, &heads, h002_head(&tmp, names.data[k]));
	size_t w = 0;
	for (size_t i = 0; i < doc->diags.len; i++) {
		Diag dg = doc->diags.data[i];
		int drop = 0;
		if (strcmp(dg.code, "H002") == 0) {
			S m = dg.message;
			for (size_t k = 0; k < heads.len; k++) {
				S h = heads.data[k];
				if (m.n >= h.n && memcmp(m.p, h.p, h.n) == 0) { drop = 1; break; }
			}
		}
		if (!drop) doc->diags.data[w++] = dg;
	}
	doc->diags.len = w;
	arena_free(&tmp);
}

// How many lines or values parsing dropped that canonical output cannot
// re-emit - bad indentation, an unusable selector, a line past the depth cap.
// Content-malformed lines do NOT count: those are retained as trivia and
// survive a save. Nonzero means a save would delete hand-written content, so
// shcl_save_file refuses then (shcl_save_file_lossy overrides).
size_t shcl_lost_count(const shcl_doc *d) { return d->lost; }

size_t shcl_error_count(const shcl_doc *d) {
	size_t n = 0;
	for (size_t i = 0; i < d->diags.len; i++) if (d->diags.data[i].sev == SHCL_SEV_ERROR) n++;
	return n;
}

shcl_doc *shcl_load_and_validate(const char *text, size_t len, const char *schema, size_t slen, shcl_strictness s) {
	shcl_doc *d = shcl_parse_with(text, len, s);
	S st; st.p = schema ? schema : ""; st.n = schema ? slen : 0;
	if (s_trim(st).n != 0) {
		shcl_doc *sd = shcl_parse(schema, slen);
		// A schema that did not load would silently drop the constraints on
		// its broken lines, or report every field as unknown - either way
		// blaming the document for the schema. Say so instead, as `check`
		// does, and validate nothing.
		int sbad = 0;
		for (size_t i = 0; i < sd->diags.len; i++) if (sd->diags.data[i].sev == SHCL_SEV_ERROR) { sbad = 1; break; }
		if (sbad) {
			push_diag(d, 0, SHCL_SEV_ERROR, s_lit("schema failed to load"));
			shcl_free(sd);
			return d;
		}
		shcl_validation *v = shcl_validate(d, sd);
		for (size_t i = 0; i < v->diags.len; i++) {
			Diag dg = v->diags.data[i];
			/* the validation arena dies below; codes are static strings */
			dg.message = s_dup(&d->arena, dg.message);
			VecDiag_push(&d->arena, &d->diags, dg);
		}
		shcl_suppress_declared_repeats(sd, d);
		shcl_suppress_declared_reopens(sd, d);
		shcl_validation_free(v);
		shcl_free(sd);
	}
	return d;
}

// --- File tier (optional; compile out with -DSHCL_NO_FILE_IO) ----------------

#ifndef SHCL_NO_FILE_IO
#include <errno.h>
#include <fcntl.h>
#ifdef _WIN32
	#include <windows.h>
	#include <io.h>
	#include <process.h>
	#include <sys/stat.h>
#else
	#include <unistd.h>
	#include <sys/stat.h>
	// realpath is XSI; declared here so the single-header build works under a
	// plain -std=c11 -D_POSIX_C_SOURCE consumer too (the symbol is always in
	// libc even when the prototype is feature-gated away).
	extern char *realpath(const char *, char *);
#endif

// The file tier's write mechanism (also what the CLI's --write uses): a temp
// file in the same dir, then a rename over the target, so an interrupted write
// can never truncate the config it rewrites. The data is synced before the
// rename so a crash cannot publish an empty file. The target is resolved
// through symlinks first and the original's mode is copied onto the temp file;
// other hard links to the old inode keep the old content (inherent to rename).
// Returns 1 on success, 0 on failure with errno left describing it.
int shcl_write_file_atomic(const char *path, const char *data, size_t n) {
	const char *target = path;
#ifndef _WIN32
	// realpath returns NULL when the target does not exist yet; that is a
	// plain create, so the path as given is already the right one.
	char *real = realpath(path, NULL);
	if (real) target = real;
	#define SHCL_FILE_CLEANUP() do { free(real); } while (0)
#else
	#define SHCL_FILE_CLEANUP() do { } while (0)
#endif
	const char *slash = strrchr(target, '/');
	char *tmp = (char *)malloc(strlen(target) + 48);
	if (!tmp) { SHCL_FILE_CLEANUP(); return 0; }
	// Exclusive create: anything already sitting at the predictable name -
	// including a planted symlink - must fail rather than be written through.
	// Born 0600; the real mode goes on below, before any data.
	int fd = -1;
	for (int attempt = 0; attempt < 8; attempt++) {
		if (slash) sprintf(tmp, "%.*s.%s.tmp%ld.%d", (int)(slash - target + 1), target, slash + 1, (long)getpid(), attempt);
		else sprintf(tmp, ".%s.tmp%ld.%d", target, (long)getpid(), attempt);
#ifdef _WIN32
		fd = _open(tmp, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
		fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
		if (fd >= 0) break;
	}
	if (fd < 0) { free(tmp); SHCL_FILE_CLEANUP(); return 0; }
	FILE *f = fdopen(fd, "wb");
	if (!f) { close(fd); remove(tmp); free(tmp); SHCL_FILE_CLEANUP(); return 0; }
#ifndef _WIN32
	// On the descriptor before any data, so umask cannot narrow it. Best
	// effort: a filesystem that cannot carry the mode is not a failure.
	struct stat st;
	if (stat(target, &st) == 0) (void)fchmod(fileno(f), st.st_mode & 07777);
#endif
	int ok = fwrite(data, 1, n, f) == n && fflush(f) == 0;
#ifdef _WIN32
	ok = ok && _commit(_fileno(f)) == 0;
#else
	ok = ok && fsync(fileno(f)) == 0;
#endif
	ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
	// C rename() will not replace an existing file on Windows.
	ok = ok && MoveFileExA(tmp, target, MOVEFILE_REPLACE_EXISTING);
#else
	ok = ok && rename(tmp, target) == 0;
#endif
	if (!ok) remove(tmp);
	free(tmp);
	SHCL_FILE_CLEANUP();
#undef SHCL_FILE_CLEANUP
	return ok ? 1 : 0;
}

// File tier, load half: read and parse PATH. Never fails - the document
// always comes back usable (empty when the file could not be read), and the
// status out-param separates the four cases consumers otherwise confuse:
// absent, present-but-unreadable, parsed with errors, clean.
shcl_doc *shcl_load_file_with(const char *path, shcl_strictness s, shcl_file_status *status) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		if (status) *status = (errno == ENOENT) ? SHCL_FILE_NOT_FOUND : SHCL_FILE_UNREADABLE;
		return shcl_parse_with("", 0, s);
	}
	size_t cap = 1 << 16, len = 0;
	char *buf = (char *)malloc(cap);
	int rerr = buf == NULL;
	while (!rerr) {
		if (len == cap) {
			char *nb = (char *)realloc(buf, cap *= 2);
			if (!nb) { rerr = 1; break; }
			buf = nb;
		}
		size_t got = fread(buf + len, 1, cap - len, f);
		len += got;
		if (got < cap - len + got) {
			if (ferror(f)) rerr = 1; // a directory reads this way on POSIX
			break;
		}
	}
	fclose(f);
	if (rerr) {
		free(buf);
		if (status) *status = SHCL_FILE_UNREADABLE;
		return shcl_parse_with("", 0, s);
	}
	shcl_doc *d = shcl_parse_with(buf, len, s);
	free(buf);
	if (status) {
		*status = SHCL_FILE_CLEAN;
		for (size_t i = 0; i < d->diags.len; i++)
			if (d->diags.data[i].sev == SHCL_SEV_ERROR) { *status = SHCL_FILE_HAD_ERRORS; break; }
	}
	return d;
}

shcl_doc *shcl_load_file(const char *path, shcl_file_status *status) {
	return shcl_load_file_with(path, SHCL_STANDARD, status);
}

// File tier, save half: write the document's canonical text to PATH through
// shcl_write_file_atomic. Returns 1 on success, 0 on failure. Refuses when
// parsing lost content that a save would silently delete (shcl_lost_count);
// shcl_save_file_lossy writes anyway.
int shcl_save_file(shcl_doc *d, const char *path) {
	if (d->lost > 0) return 0;
	shcl_str c = shcl_to_canonical(d);
	return shcl_write_file_atomic(path, c.p, c.n);
}

// shcl_save_file without the lost-content gate: writes even when parsing
// dropped lines this save deletes. The caller owns that choice.
int shcl_save_file_lossy(shcl_doc *d, const char *path) {
	shcl_str c = shcl_to_canonical(d);
	return shcl_write_file_atomic(path, c.p, c.n);
}
#endif /* SHCL_NO_FILE_IO */

// --- Schema-driven generation (`shcl init --schema`) ------------------------

static S v_allowed_join(Arena *a, const VCons *c) {
	SB s = {0, 0, 0};
	char nb[64];
	for (size_t i = 0; i < c->a_n; i++) {
		if (i) sb_puts(a, &s, ", ");
		switch (c->akind) {
			case ALLOW_INTS: { snprintf(nb, sizeof nb, "%" PRId64, c->a_ints[i]); sb_puts(a, &s, nb); break; }
			case ALLOW_FLOATS: { char fb[SHCL_F64_BUF]; S f; f.p = fb; f.n = shcl_format_f64(c->a_floats[i], fb); sb_putS(a, &s, f); break; }
			case ALLOW_BOOLS: sb_puts(a, &s, c->a_bools[i] ? "true" : "false"); break;
			case ALLOW_DATES: { char db[64]; S d; d.p = db; d.n = shcl_datetime_str(&c->a_dates[i], db); sb_putS(a, &s, d); break; }
			case ALLOW_STRINGS: sb_putS(a, &s, c->a_strs[i]); break;
		}
	}
	return sb_S(&s);
}

// The `# type, ...` annotation line summarizing a constraint, ASCII only.
static S v_gen_annotation(Arena *a, const VCons *c, S tyname) {
	SB s = {0, 0, 0};
	char nb[80];
	sb_putS(a, &s, tyname);
	if (c->has_allowed) {
		sb_puts(a, &s, ", one of: "); sb_putS(a, &s, v_allowed_join(a, c));
	} else if (c->has_min_i || c->has_max_i) {
		if (c->has_min_i && c->has_max_i) snprintf(nb, sizeof nb, ", %" PRId64 "-%" PRId64, c->min_i, c->max_i);
		else if (c->has_min_i) snprintf(nb, sizeof nb, ", >= %" PRId64, c->min_i);
		else snprintf(nb, sizeof nb, ", <= %" PRId64, c->max_i);
		sb_puts(a, &s, nb);
	} else if (c->has_min_f || c->has_max_f) {
		char fb[SHCL_F64_BUF];
		sb_puts(a, &s, ", ");
		if (c->has_min_f && c->has_max_f) {
			S f; f.p = fb; f.n = shcl_format_f64(c->min_f, fb); sb_putS(a, &s, f);
			sb_putc(a, &s, '-');
			S g; g.p = fb; g.n = shcl_format_f64(c->max_f, fb); sb_putS(a, &s, g);
		} else if (c->has_min_f) {
			sb_puts(a, &s, ">= "); S f; f.p = fb; f.n = shcl_format_f64(c->min_f, fb); sb_putS(a, &s, f);
		} else {
			sb_puts(a, &s, "<= "); S f; f.p = fb; f.n = shcl_format_f64(c->max_f, fb); sb_putS(a, &s, f);
		}
	}
	if (c->has_repeat) {
		if (c->rep_lo == c->rep_hi) snprintf(nb, sizeof nb, ", repeat %" PRIu64, c->rep_lo);
		else snprintf(nb, sizeof nb, ", repeat %" PRIu64 "-%" PRIu64, c->rep_lo, c->rep_hi);
		sb_puts(a, &s, nb);
	}
	if (c->required) sb_puts(a, &s, ", required");
	return sb_S(&s);
}

// A field must exist when required or its repeat lower bound is 1+; a
// commented-out line for either would fail the very schema that produced it.
static int g_must_exist(const VCons *c) { return c->required || (c->has_repeat && c->rep_lo >= 1); }
static int g_has_wild(const VCons *c) {
	for (size_t si = 0; si < c->segs.len; si++) if (c->segs.data[si].sel.tag == SEL_WILDCARD) return 1;
	return 0;
}
// `[#N]` needs a pre-existing instance and its `#` would start a comment on a
// binding line; a path with a literal newline cannot be written at all. A path
// deeper than a document may nest cannot be generated either: the line would
// draw E016 on the way back in.
static int g_unwritable(const VCons *c) {
	if (c->segs.len > SHCL_MAX_DEPTH) return 1;
	for (size_t si = 0; si < c->segs.len; si++) if (c->segs.data[si].sel.tag == SEL_INDEX || c->segs.data[si].star) return 1;
	for (size_t k = 0; k < c->path.n; k++) if (c->path.p[k] == '\n') return 1;
	return 0;
}
// s with every '\n' escaped to backslash-n (comments and annotations must stay
// one line no matter what an allowed value smuggles in).
static S g_escape_nl(Arena *a, S s) {
	int has = 0;
	for (size_t k = 0; k < s.n; k++) if (s.p[k] == '\n') { has = 1; break; }
	if (!has) return s;
	SB b = {0, 0, 0};
	for (size_t k = 0; k < s.n; k++) {
		if (s.p[k] == '\n') sb_puts(a, &b, "\\n");
		else sb_putc(a, &b, s.p[k]);
	}
	return sb_S(&b);
}
// A default carrying a literal newline cannot sit on a value line; the quoted
// escaped spelling reads back to the same string.
static S g_default_text(Arena *a, S v) {
	int has = 0;
	for (size_t k = 0; k < v.n; k++) if (v.p[k] == '\n') { has = 1; break; }
	if (!has) return v;
	SB b = {0, 0, 0};
	sb_putc(a, &b, '"');
	for (size_t k = 0; k < v.n; k++) {
		char ch = v.p[k];
		if (ch == '\\') sb_puts(a, &b, "\\\\");
		else if (ch == '"') sb_puts(a, &b, "\\\"");
		else if (ch == '\n') sb_puts(a, &b, "\\n");
		else if (ch == '\t') sb_puts(a, &b, "\\t");
		else sb_putc(a, &b, ch);
	}
	sb_putc(a, &b, '"');
	return sb_S(&b);
}

// Ceiling on how many fields one schema may expand to. Fragments that mount
// each other at more than one path multiply, so a short schema can otherwise
// ask for more output than the machine can hold; past this the generator
// reports a schema fault rather than running until something breaks.
#define GEN_MAX_FIELDS ((size_t)10000)

// Footer telling whoever opens the generated file what the format is and where
// its spec lives. It is output, so every binding emits these bytes exactly; the
// Legal line names SHCL as its subject so it cannot be read as a claim over the
// config it sits in.
#define GEN_BANNER \
	"#\n" \
	"# This config file format is SHCL.\n" \
	"# \"Simple Hierarchical Config Language\"\n" \
	"#    Home     https://github.com/jim-collier/shcl\n" \
	"#    Syntax   https://github.com/jim-collier/shcl/blob/main/project/spec.md\n" \
	"#    Legal    SHCL is Copyright © 2026 Jim Collier. License: MIT. No warranty.\n" \
	"#\n"

// Render parsed segments back as a dotted path, dropping wildcard selectors
// (a generated line targets the one instance it materializes) and quoting a
// name that needs it, so the result is a path the scanner reads back the same.
static S gen_path_text(Arena *a, const VecSeg *segs) {
	SB out = {0, 0, 0};
	char nb[32];
	for (size_t i = 0; i < segs->len; i++) {
		const Segment *s = &segs->data[i];
		if (i > 0) sb_putc(a, &out, '.');
		if (s->star) sb_putc(a, &out, '*');
		else sb_putS(a, &out, emit_name(a, s->name));
		switch (s->sel.tag) {
		case SEL_VALUE:
			sb_putc(a, &out, '[');
			if (s->sel.quoted) sb_putS(a, &out, quote_text(a, s->sel.value));
			else sb_putS(a, &out, s->sel.value);
			sb_putc(a, &out, ']'); break;
		case SEL_INDEX: { int nn = snprintf(nb, sizeof nb, "[#%" PRIu64 "]", s->sel.index); sb_put(a, &out, nb, (size_t)nn); break; }
		case SEL_WILDCARD: case SEL_NONE: break;
		}
	}
	return sb_S(&out);
}

// Inline every fragment mount into a flat constraint list, depth-first in
// schema order, each field's path and segments prefixed by its mount's. A
// mount whose fragment is already expanding (a cycle) stops there and is
// recorded as (path, fragment name) for the trailing not-generated block.
static void g_expand_go(Arena *a, const VecVCons *list, const VSchemaDef *def, const S *at_path, const VecSeg *at_segs, VecS *stack, VecVCons *out, VecS *cut_path, VecS *cut_frag) {
	for (size_t i = 0; i < list->len; i++) {
		const VCons *c = &list->data[i];
		VCons cc = *c;
		if (at_path) {
			SB p = {0, 0, 0};
			sb_putS(a, &p, *at_path); sb_putc(a, &p, '.'); sb_putS(a, &p, c->path);
			cc.path = sb_S(&p);
			VecSeg segs = {0, 0, 0};
			for (size_t k = 0; k < at_segs->len; k++) VecSeg_push(a, &segs, at_segs->data[k]);
			for (size_t k = 0; k < c->segs.len; k++) VecSeg_push(a, &segs, c->segs.data[k]);
			cc.segs = segs;
		}
		S path = cc.path; VecSeg segs = cc.segs;
		if (out->len >= GEN_MAX_FIELDS) return;
		VecVCons_push(a, out, cc);
		if (c->inherits.n) {
			int cycling = 0;
			for (size_t k = 0; k < stack->len; k++) if (s_eq(stack->data[k], c->inherits)) { cycling = 1; break; }
			// A chain long enough to outrun the stack, or a mount that
			// re-enters, stops here and is noted instead of expanded.
			if (cycling || stack->len >= SHCL_MAX_DEPTH) {
				VecS_push(a, cut_path, g_escape_nl(a, path));
				VecS_push(a, cut_frag, c->inherits);
			} else {
				const VecVCons *fcs = v_frag_get(def, c->inherits);
				if (fcs) {
					VecS_push(a, stack, c->inherits);
					g_expand_go(a, fcs, def, &path, &segs, stack, out, cut_path, cut_frag);
					stack->len--;
				}
			}
		}
	}
}
static void g_expand_mounts(Arena *a, const VSchemaDef *def, VecVCons *out, VecS *cut_path, VecS *cut_frag) {
	VecS stack = {0, 0, 0};
	g_expand_go(a, &def->cons, def, NULL, NULL, &stack, out, cut_path, cut_frag);
}

shcl_str shcl_generate(shcl_doc *schema, int no_banner, int *ok) {
	// Only the returned bytes are contracted to live in the schema's arena;
	// the constraint/fault lists, expansion copies, and the output builder are
	// ~60x that, so they build in a private arena (shcl_validate's discipline)
	// and die here - the caller may generate from one schema repeatedly.
	Arena tmp; tmp.head = NULL;
	Arena *a = &tmp;
	VSchemaDef def; memset(&def, 0, sizeof def);
	VecDiag faults = {0, 0, 0};
	// Generation lays the whole schema out, so unlike validation it has no
	// safe partial mode: any fault fails it.
	v_build_schema(a, schema, &def, &faults);
	shcl_str r;
	if (faults.len) { if (ok) *ok = 0; S e = s_empty(); r.p = e.p; r.n = e.n; arena_free(&tmp); return r; }
	if (ok) *ok = 1;
	VecVCons cons = {0, 0, 0};
	VecS cut_path = {0, 0, 0}, cut_frag = {0, 0, 0};
	g_expand_mounts(a, &def, &cons, &cut_path, &cut_frag);
	if (cons.len >= GEN_MAX_FIELDS) {
		// Generation-only fault: recorded on the schema document (this
		// signature has no fault list of its own to return).
		SB m = {0, 0, 0};
		sb_puts(a, &m, "schema expands past "); sb_put_u64(a, &m, GEN_MAX_FIELDS);
		sb_puts(a, &m, " fields; fragments mounted at more than one path multiply");
		// the diag outlives this call: its text must leave the private arena
		push_diag(schema, 0, SHCL_SEV_ERROR, s_dup(&schema->arena, sb_S(&m)));
		if (ok) *ok = 0;
		S e = s_empty(); r.p = e.p; r.n = e.n; arena_free(&tmp); return r;
	}
	// Live concrete paths materialize instances; decide which must-exist
	// wildcards get filled (their first-wildcard parent chain is a prefix of
	// some live path's name list). Fixpoint: a fill can materialize another's
	// parent. Live paths are stored as their segment-name lists.
	size_t nlive = 0, clive = 0;
	VecSeg *live = NULL;
	int *fill = (int *)arena_alloc(a, (cons.len ? cons.len : 1) * sizeof *fill);
	for (size_t i = 0; i < cons.len; i++) fill[i] = 0;
	#define LIVE_PUSH(SEGS) do { if (nlive == clive) { clive = clive ? clive * 2 : 8; VecSeg *nl = (VecSeg *)arena_alloc(a, clive * sizeof *nl); for (size_t t = 0; t < nlive; t++) nl[t] = live[t]; live = nl; } live[nlive++] = (SEGS); } while (0)
	for (size_t i = 0; i < cons.len; i++) {
		VCons *c = &cons.data[i];
		if (!g_has_wild(c) && !g_unwritable(c) && g_must_exist(c)) LIVE_PUSH(c->segs);
	}
	for (;;) {
		int changed = 0;
		for (size_t i = 0; i < cons.len; i++) {
			VCons *c = &cons.data[i];
			if (fill[i] || !g_has_wild(c) || g_unwritable(c) || !g_must_exist(c)) continue;
			size_t k = 0;
			while (c->segs.data[k].sel.tag != SEL_WILDCARD) k++;
			size_t plen = k + 1; // parent chain: names up to and including the wildcard segment
			int hit = 0;
			for (size_t li = 0; li < nlive && !hit; li++) {
				if (live[li].len < plen) continue;
				int eq = 1;
				for (size_t s2 = 0; s2 < plen; s2++) if (!s_eq(live[li].data[s2].name, c->segs.data[s2].name)) { eq = 0; break; }
				hit = eq;
			}
			if (hit) { fill[i] = 1; LIVE_PUSH(c->segs); changed = 1; }
		}
		if (!changed) break;
	}
	#undef LIVE_PUSH
	SB out = {0, 0, 0};
	VecS wild_path = {0, 0, 0}, wild_type = {0, 0, 0};
	int first = 1;
	for (size_t i = 0; i < cons.len; i++) {
		VCons *c = &cons.data[i];
		S tyname;
		if (c->ty) { tyname.p = c->ty; tyname.n = strlen(c->ty); } else tyname = s_lit("any");
		if (g_unwritable(c) || (g_has_wild(c) && !fill[i])) {
			VecS_push(a, &wild_path, g_escape_nl(a, c->path)); VecS_push(a, &wild_type, tyname);
			continue;
		}
		if (!first) sb_putc(a, &out, '\n');
		first = 0;
		if (c->has_desc) {
			size_t start = 0;
			for (size_t k = 0; k <= c->desc.n; k++) {
				if (k == c->desc.n || c->desc.p[k] == '\n') {
					sb_puts(a, &out, "# ");
					S ln; ln.p = c->desc.p + start; ln.n = k - start; sb_putS(a, &out, ln);
					sb_putc(a, &out, '\n');
					start = k + 1;
				}
			}
		}
		sb_puts(a, &out, "# "); sb_putS(a, &out, g_escape_nl(a, v_gen_annotation(a, c, tyname))); sb_putc(a, &out, '\n');
		if (!g_must_exist(c)) sb_putc(a, &out, '#');
		if (fill[i]) {
			// A filled wildcard emits in dotted form, targeting the first (the
			// materialized) instance. Rebuilt from the parsed segments, not by
			// cutting text out of the path: the same path can be written several
			// ways, and only the segments say what it means.
			sb_putS(a, &out, gen_path_text(a, &c->segs));
		} else {
			sb_putS(a, &out, c->path);
		}
		if (c->has_default) { sb_puts(a, &out, ": "); sb_putS(a, &out, g_default_text(a, c->default_text)); }
		else sb_putc(a, &out, ':');
		sb_putc(a, &out, '\n');
	}
	// Cycle-cut mounts last: their "type" column names the fragment that
	// belongs at the path.
	for (size_t i = 0; i < cut_path.len; i++) {
		VecS_push(a, &wild_path, cut_path.data[i]);
		VecS_push(a, &wild_type, cut_frag.data[i]);
	}
	if (wild_path.len) {
		if (!first) sb_putc(a, &out, '\n');
		sb_puts(a, &out, "# Paths needing an instance name (not generated):\n");
		for (size_t i = 0; i < wild_path.len; i++) {
			sb_puts(a, &out, "#   "); sb_putS(a, &out, wild_path.data[i]);
			sb_puts(a, &out, "   "); sb_putS(a, &out, wild_type.data[i]); sb_putc(a, &out, '\n');
		}
	}
	if (!no_banner) {
		if (out.len) sb_putc(a, &out, '\n');
		sb_puts(a, &out, GEN_BANNER);
	}
	S s = s_dup(&schema->arena, sb_S(&out)); r.p = s.p; r.n = s.n;
	arena_free(&tmp);
	return r;
}

#ifdef __cplusplus
#pragma GCC diagnostic pop
#endif

// The implementation's short internal macros would otherwise outlive the header
// in the consumer's own translation unit, where these names are common.
#undef DEFINE_VEC
#undef ROOT
#undef NODE
#undef GEN_MAX_FIELDS
#undef GEN_BANNER

#endif // SHCL_IMPLEMENTATION
#endif // SHCL_H
