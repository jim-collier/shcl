// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

// SHCL reference implementation for C: parser, accessor, writer/formatter.
// Single-header, drop-in: copy this file into your tree and, in exactly ONE .c,
//   #define SHCL_IMPLEMENTATION
//   #include "shcl.h"
// Everything is byte-for-byte with the Rust reference (source/rust); the cicd
// cross-binding check compares CLI stdout + exit codes across every binding.
// The language spec lives in project/spec.md; project/conformance/ pins behavior.
//
// A companion C++ typed veneer (get<int64_t>() etc.) sits in shcl.hpp; it wraps
// this core, it is not a second parser.

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

// Parse never fails: bad lines are skipped and diagnosed. Text need not be NUL
// terminated. Free with shcl_free.
shcl_doc *shcl_parse(const char *text, size_t len);
shcl_doc *shcl_parse_with(const char *text, size_t len, shcl_strictness s);
void shcl_free(shcl_doc *d);

// True when a strict load would fail (strictness==strict and an error diagnostic
// exists). At loose/standard this is always false.
int shcl_strict_failed(const shcl_doc *d);
shcl_strictness shcl_strictness_of(const shcl_doc *d);

// Diagnostics, in emission order (parse errors then repeated-leaf hints).
size_t shcl_diag_count(const shcl_doc *d);
size_t shcl_diag_line(const shcl_doc *d, size_t i);
shcl_severity shcl_diag_severity(const shcl_doc *d, size_t i);
shcl_str shcl_diag_message(const shcl_doc *d, size_t i);

// Canonical form (block layout, tabs, insertion order, minimal quoting). The
// returned bytes live in the document's arena; valid until shcl_free.
shcl_str shcl_to_canonical(shcl_doc *d);

size_t shcl_count(shcl_doc *d, const char *path, size_t plen);
// Instance display values, in file order. Writes an arena-owned array to *out.
size_t shcl_instances(shcl_doc *d, const char *path, size_t plen, shcl_str **out);

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

// CLI/aliases: 1|2|3 or loose|standard|strict. Returns 1 on success.
int shcl_strictness_from_arg(const char *s, size_t n, shcl_strictness *out);

// Format helpers matching the reference's textual output.
// out must be at least SHCL_F64_BUF bytes; returns the byte length written.
#define SHCL_F64_BUF 512
size_t shcl_format_f64(double v, char *out);
// Renders a datetime into out (>= 64 bytes); returns byte length.
size_t shcl_datetime_str(const shcl_datetime *dt, char *out);
// Status <-> the CLI exit code / textual name.
int shcl_status_code(shcl_status s);
const char *shcl_status_name(shcl_status s);

#ifdef __cplusplus
}
#endif

// ===========================================================================
#ifdef SHCL_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <inttypes.h>

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
	S content; S info; unsigned char fence_char; size_t fence_len; // V_RAW
} Value;

DEFINE_VEC(VecSize, size_t)

typedef struct {
	S name;
	Value value;
	VecSize children;
	size_t parent;
	size_t line;
	int star_list;
} Node;
DEFINE_VEC(VecNode, Node)

typedef struct { size_t line; shcl_severity sev; S message; } Diag;
DEFINE_VEC(VecDiag, Diag)

struct shcl_doc {
	Arena arena;
	VecNode nodes;
	VecDiag diags;
	shcl_strictness strictness;
};
#define ROOT ((size_t)0)
#define NODE(d, i) ((d)->nodes.data[i])

static Value v_empty(void) { Value v; memset(&v, 0, sizeof v); v.kind = V_EMPTY; return v; }
static int v_is_empty(const Value *v) { return v->kind == V_EMPTY; }

// Merge key: nodes with equal (name, key) collapse into one. Built exactly like
// the reference (NUL-separated cell texts) so any pathological collision matches.
static S value_key(Arena *a, const Value *v) {
	SB s = {0};
	if (v->kind == V_EMPTY) { sb_putc(a, &s, 'e'); return sb_S(&s); }
	if (v->kind == V_CELL) {
		sb_puts(a, &s, "c:");
		for (size_t i = 0; i < v->nels; i++) { sb_putc(a, &s, '\0'); sb_putS(a, &s, v->els[i].text); }
		return sb_S(&s);
	}
	sb_puts(a, &s, "r:"); sb_putS(a, &s, v->content); return sb_S(&s);
}
static S value_display(Arena *a, const Value *v) {
	if (v->kind == V_EMPTY) return s_empty();
	if (v->kind == V_RAW) return v->content;
	SB s = {0};
	for (size_t i = 0; i < v->nels; i++) { if (i) sb_puts(a, &s, ", "); sb_putS(a, &s, v->els[i].text); }
	return sb_S(&s);
}

// --- lexical helpers ---------------------------------------------------------

// Strip an unquoted trailing comment. A backslash shields the next char.
static S strip_comment(S s) {
	uint32_t inq = 0; // 0 = none, else the open quote codepoint
	size_t i = 0;
	while (i < s.n) {
		uint32_t c; size_t l = utf8_decode(s.p, s.n, i, &c);
		if (c == '\\') { i += l; if (i < s.n) { uint32_t d; i += utf8_decode(s.p, s.n, i, &d); } continue; }
		if (inq) { if (c == inq) inq = 0; }
		else if (c == '"' || c == '\'') inq = c;
		else if (c == '#') return s_slice(s, 0, i);
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
typedef struct { seltag tag; S value; size_t index; } Selector;
typedef struct { S name; Selector sel; } Segment;
DEFINE_VEC(VecSeg, Segment)
typedef struct { int ok; VecSeg segs; int has_value; S value_text; S err; } PathScan;

// usize parse: optional single leading '+', >=1 digit, no overflow.
static int parse_usize(S s, size_t *out) {
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
	*out = (size_t)v; return 1;
}

static void skip_ws_cp(CPs c, size_t *pos) {
	while (*pos < c.n && (c.cp[*pos] == ' ' || c.cp[*pos] == '\t')) (*pos)++;
}
// Read a quoted name/value in a path (escape pairs preserved literally).
static int read_quoted_path(Arena *a, S src, CPs c, size_t *pos, S *out, S *err) {
	uint32_t q = c.cp[*pos]; (*pos)++;
	SB sb = {0};
	for (;;) {
		if (*pos >= c.n) { *err = s_lit("unterminated quote"); return 0; }
		uint32_t ch = c.cp[*pos];
		if (ch == '\\' && *pos + 1 < c.n) { sb_put_cp(a, &sb, ch); sb_put_cp(a, &sb, c.cp[*pos + 1]); *pos += 2; continue; }
		(*pos)++;
		if (ch == q) { *out = sb_S(&sb); (void)src; return 1; }
		sb_put_cp(a, &sb, ch);
	}
}

static PathScan scan_path(Arena *a, S input) {
	PathScan ps; ps.ok = 0; memset(&ps.segs, 0, sizeof ps.segs); ps.has_value = 0; ps.value_text = s_empty(); ps.err = s_empty();
	CPs c = decode_cps(a, input);
	size_t pos = 0;
	for (;;) {
		skip_ws_cp(c, &pos);
		if (pos >= c.n) { ps.err = s_lit("empty path"); return ps; }
		S name;
		if (c.cp[pos] == '"' || c.cp[pos] == '\'') {
			if (!read_quoted_path(a, input, c, &pos, &name, &ps.err)) return ps;
		} else {
			size_t start = pos;
			while (pos < c.n && is_bare_name_char(c.cp[pos])) pos++;
			if (pos == start) {
				SB e = {0}; sb_puts(a, &e, "expected field name, found '"); sb_put_cp(a, &e, c.cp[pos]); sb_putc(a, &e, '\'');
				ps.err = sb_S(&e); return ps;
			}
			name = cps_slice(input, c, start, pos);
		}
		Selector sel; sel.tag = SEL_NONE; sel.value = s_empty(); sel.index = 0;
		skip_ws_cp(c, &pos);
		int have_bracket = 0; size_t bracket_at = 0;
		if (pos < c.n && c.cp[pos] == '[') { have_bracket = 1; bracket_at = pos; }
		else if (pos < c.n && c.cp[pos] == ':') {
			size_t q = pos + 1; skip_ws_cp(c, &q);
			if (q < c.n && c.cp[q] == '[') { have_bracket = 1; bracket_at = q; }
		}
		if (have_bracket) {
			pos = bracket_at + 1;
			skip_ws_cp(c, &pos);
			if (pos < c.n && (c.cp[pos] == '"' || c.cp[pos] == '\'')) {
				S v; if (!read_quoted_path(a, input, c, &pos, &v, &ps.err)) return ps;
				sel.tag = SEL_VALUE; sel.value = v; // quotes force a value match, even numeric
			} else {
				size_t start = pos;
				while (pos < c.n && c.cp[pos] != ']') pos++;
				S body = s_trim(cps_slice(input, c, start, pos));
				size_t idx;
				if (body.n == 1 && body.p[0] == '*') {
					sel.tag = SEL_WILDCARD;
				} else if (body.n >= 1 && body.p[0] == '#' && parse_usize(s_slice(body, 1, body.n), &idx)) {
					sel.tag = SEL_INDEX; sel.index = idx;
				} else if (parse_usize(body, &idx)) {
					sel.tag = SEL_INDEX; sel.index = idx;
				} else if (body.n == 0) {
					ps.err = s_lit("empty selector"); return ps;
				} else {
					sel.tag = SEL_VALUE; sel.value = norm_dangling(a, body);
				}
			}
			skip_ws_cp(c, &pos);
			if (pos >= c.n || c.cp[pos] != ']') { ps.err = s_lit("unterminated selector"); return ps; }
			pos++;
			skip_ws_cp(c, &pos);
		}
		Segment seg; seg.name = fold_name(a, name); seg.sel = sel;
		VecSeg_push(a, &ps.segs, seg);
		if (pos >= c.n) { ps.ok = 1; ps.has_value = 0; return ps; }
		if (c.cp[pos] == '.') { pos++; continue; }
		if (c.cp[pos] == ':') {
			pos++;
			ps.ok = 1; ps.has_value = 1;
			ps.value_text = s_dup(a, s_trim(cps_slice(input, c, pos, c.n)));
			return ps;
		}
		{ SB e = {0}; sb_puts(a, &e, "unexpected '"); sb_put_cp(a, &e, c.cp[pos]); sb_puts(a, &e, "' after field"); ps.err = sb_S(&e); return ps; }
	}
}

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

DEFINE_VEC(VecS, S)
typedef struct { S indent; size_t node; } StackEnt;
DEFINE_VEC(VecStack, StackEnt)
typedef struct { int present; size_t idx; shcl_status miss; } Slot;
DEFINE_VEC(VecSlot, Slot)

// --- coercion ("intelligent but safe"; Loose re-admits a closed list) --------

static const uint32_t CURRENCY[] = {
	'$', 0xA2, 0xA3, 0xA4, 0xA5, 0x20A9, 0x20AA, 0x20AB, 0x20AC, 0x20AD,
	0x20AE, 0x20B1, 0x20B2, 0x20B4, 0x20B9, 0x20BA, 0x20BC, 0x20BD, 0x20BE, 0x20BF,
};
static S strip_currency(S t) {
	if (t.n == 0) return t;
	uint32_t c; size_t l = utf8_decode(t.p, t.n, 0, &c);
	for (size_t i = 0; i < sizeof(CURRENCY) / sizeof(CURRENCY[0]); i++)
		if (c == CURRENCY[i]) return s_slice(t, l, t.n);
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
static int parse_hex_i64(S h, int64_t *out) {
	uint64_t v = 0;
	for (size_t i = 0; i < h.n; i++) {
		unsigned char c = (unsigned char)h.p[i]; int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else return 0;
		if (v > ((uint64_t)INT64_MAX - d) / 16) return 0;
		v = v * 16 + d;
	}
	*out = (int64_t)v; return 1;
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
	char *buf = (char *)arena_alloc(a, t.n + 1);
	memcpy(buf, t.p, t.n); buf[t.n] = '\0';
	char *end; double v = strtod(buf, &end);
	if (end != buf + t.n) return 0;
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
		if (all_ahex(h)) { int64_t v; if (!parse_hex_i64(h, &v)) return 0; *out = neg ? -v : v; return 1; }
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

typedef struct { shcl_doc *d; VecStack stack; } Parser;

static void push_diag(shcl_doc *d, size_t line, shcl_severity sev, S msg) {
	Diag dg; dg.line = line; dg.sev = sev; dg.message = msg;
	VecDiag_push(&d->arena, &d->diags, dg);
}
static void p_err(Parser *P, size_t line, S msg) { push_diag(P->d, line, SHCL_SEV_ERROR, msg); }

static size_t select_or_create(Parser *P, size_t parent, S name, Value value, size_t line) {
	Arena *a = &P->d->arena;
	S key = value_key(a, &value);
	VecSize ch = NODE(P->d, parent).children;
	for (size_t k = 0; k < ch.len; k++) {
		size_t c = ch.data[k];
		if (s_eq(NODE(P->d, c).name, name)) {
			S ck = value_key(a, &NODE(P->d, c).value);
			if (s_eq(ck, key)) return c;
		}
	}
	size_t idx = P->d->nodes.len;
	Node n; memset(&n, 0, sizeof n);
	n.name = s_dup(a, name); n.value = value; n.parent = parent; n.line = line; n.star_list = 0;
	VecNode_push(a, &P->d->nodes, n);
	VecSize_push(a, &NODE(P->d, parent).children, idx);
	return idx;
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

static int attach_path(Parser *P, size_t parent, Segment *segs, size_t nsegs, Value value, size_t line, size_t *out) {
	Arena *a = &P->d->arena;
	size_t cur = parent;
	for (size_t i = 0; i < nsegs; i++) {
		Segment *seg = &segs[i];
		int is_last = (i + 1 == nsegs);
		switch (seg->sel.tag) {
		case SEL_VALUE: {
			Value disc; memset(&disc, 0, sizeof disc); disc.kind = V_CELL;
			Element *e = (Element *)arena_alloc(a, sizeof(Element));
			e->text = s_dup(a, seg->sel.value); e->quoted = 0;
			disc.els = e; disc.nels = 1;
			cur = select_or_create(P, cur, seg->name, disc, line);
			if (is_last && !v_is_empty(&value)) {
				SB m = {0}; sb_puts(a, &m, "value after selector on '"); sb_putS(a, &m, seg->name); sb_puts(a, &m, "' ignored");
				p_err(P, line, sb_S(&m));
			}
			break;
		}
		case SEL_INDEX: {
			VecSize matches = {0}; VecSize ch = NODE(P->d, cur).children;
			for (size_t k = 0; k < ch.len; k++) { size_t c = ch.data[k]; if (s_eq(NODE(P->d, c).name, seg->name)) VecSize_push(a, &matches, c); }
			if (seg->sel.index < matches.len) cur = matches.data[seg->sel.index];
			else {
				SB m = {0}; sb_puts(a, &m, "no instance "); sb_put_u64(a, &m, seg->sel.index); sb_puts(a, &m, " of '"); sb_putS(a, &m, seg->name); sb_putc(a, &m, '\'');
				p_err(P, line, sb_S(&m)); return 0;
			}
			break;
		}
		case SEL_WILDCARD:
			p_err(P, line, s_lit("wildcard selector is query-only")); return 0;
		case SEL_NONE:
			cur = select_or_create(P, cur, seg->name, is_last ? value : v_empty(), line);
			break;
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

static void bind_block(Parser *P, size_t parent, Value value, size_t line) {
	if (parent == ROOT) { p_err(P, line, s_lit("raw block with no parent field")); return; }
	if (v_is_empty(&NODE(P->d, parent).value)) NODE(P->d, parent).value = value;
	else {
		S name = NODE(P->d, parent).name; size_t gp = NODE(P->d, parent).parent;
		select_or_create(P, gp, name, value, line);
	}
}

static void add_star_element(Parser *P, size_t parent, S body, size_t line) {
	Arena *a = &P->d->arena;
	if (parent == ROOT) { p_err(P, line, s_lit("list element with no parent field")); return; }
	S trimmed = s_trim(body);
	if (trimmed.n == 0) { p_err(P, line, s_lit("empty list element")); return; }
	if (count_unquoted_pieces(trimmed) > 1) { p_err(P, line, s_lit("bare comma in list element (one element per line)")); return; }
	Element el;
	if (!parse_element(a, trimmed, &el)) { p_err(P, line, s_lit("empty list element")); return; }
	Node *node = &NODE(P->d, parent);
	if (node->value.kind == V_EMPTY) {
		Element *arr = (Element *)arena_alloc(a, sizeof(Element)); arr[0] = el;
		node->value.kind = V_CELL; node->value.els = arr; node->value.nels = 1; node->star_list = 1;
	} else if (node->value.kind == V_CELL && node->star_list) {
		Element *arr = (Element *)arena_alloc(a, (node->value.nels + 1) * sizeof(Element));
		memcpy(arr, node->value.els, node->value.nels * sizeof(Element)); arr[node->value.nels] = el;
		node->value.els = arr; node->value.nels++;
	} else {
		p_err(P, line, s_lit("field already has a value; list element ignored"));
	}
}

static void emit_repeated_leaf_hints(Parser *P) {
	Arena *a = &P->d->arena;
	for (size_t parent = 0; parent < P->d->nodes.len; parent++) {
		VecS names = {0}; VecSize *groups = NULL; size_t ngroups = 0, cgroups = 0;
		VecSize ch = NODE(P->d, parent).children;
		for (size_t k = 0; k < ch.len; k++) {
			size_t c = ch.data[k]; S nm = NODE(P->d, c).name;
			size_t g = (size_t)-1;
			for (size_t j = 0; j < names.len; j++) if (s_eq(names.data[j], nm)) { g = j; break; }
			if (g == (size_t)-1) {
				VecS_push(a, &names, nm);
				if (ngroups == cgroups) { size_t nc = cgroups ? cgroups * 2 : 8; groups = (VecSize *)arena_grow(a, groups, cgroups, nc, sizeof(VecSize)); cgroups = nc; }
				memset(&groups[ngroups], 0, sizeof(VecSize)); g = ngroups++;
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
			SB m = {0}; sb_putc(a, &m, '\''); sb_putS(a, &m, names.data[gi]); sb_puts(a, &m, "' repeats as a bare leaf - did you mean '"); sb_putS(a, &m, names.data[gi]); sb_puts(a, &m, ": "); sb_putS(a, &m, sb_S(&joined)); sb_puts(a, &m, "'?");
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
	Parser P; P.d = d; memset(&P.stack, 0, sizeof P.stack);
	StackEnt e0; e0.indent = s_empty(); e0.node = ROOT; VecStack_push(a, &P.stack, e0);

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
		if (rest.n == 0 || rest.p[0] == '#') { i++; continue; }
		Fence f = fence_open(a, rest);
		if (f.ok) {
			size_t parent;
			if (!resolve_parent(&P, indent, &parent)) { p_err(&P, lineno, s_lit("indentation matches no open level")); i++; continue; }
			size_t next; Value val = consume_raw(&P, lines.data, lines.len, i + 1, lineno, f.ch, f.len, f.info, &next);
			bind_block(&P, parent, val, lineno); i = next; continue;
		}
		if (rest.n >= 1 && rest.p[0] == '*') {
			S after = s_slice(rest, 1, rest.n);
			if (after.n >= 1 && (after.p[0] == ' ' || after.p[0] == '\t')) {
				size_t parent;
				if (!resolve_parent(&P, indent, &parent)) { p_err(&P, lineno, s_lit("indentation matches no open level")); i++; continue; }
				add_star_element(&P, parent, strip_comment(after), lineno); i++; continue;
			}
			p_err(&P, lineno, s_lit("malformed line: '*' must be followed by a space")); i++; continue;
		}
		S content = trim_end(strip_comment(rest));
		if (content.n == 0) { i++; continue; }
		size_t parent;
		if (!resolve_parent(&P, indent, &parent)) { p_err(&P, lineno, s_lit("indentation matches no open level")); i++; continue; }
		PathScan scan = scan_path(a, content);
		if (!scan.ok) { SB m = {0}; sb_puts(a, &m, "malformed line skipped: "); sb_putS(a, &m, scan.err); p_err(&P, lineno, sb_S(&m)); i++; continue; }
		size_t next = i + 1;
		Value value;
		if (!scan.has_value) { p_err(&P, lineno, s_lit("missing colon; repaired as an empty value")); value = v_empty(); }
		else if (scan.value_text.n == 0) value = v_empty();
		else {
			Fence vf = fence_open(a, scan.value_text);
			if (vf.ok) value = consume_raw(&P, lines.data, lines.len, i + 1, lineno, vf.ch, vf.len, vf.info, &next);
			else value = parse_cell(a, scan.value_text);
		}
		size_t node;
		if (attach_path(&P, parent, scan.segs.data, scan.segs.len, value, lineno, &node)) {
			StackEnt se; se.indent = s_dup(a, indent); se.node = node; VecStack_push(a, &P.stack, se);
		}
		i = next;
	}
	emit_repeated_leaf_hints(&P);
	return d;
}

// --- accessor: path resolution ----------------------------------------------

typedef enum { R_NONE, R_ONE, R_MANY, R_SLOTS } rkind;
typedef struct { rkind kind; size_t one; VecSize many; VecSlot slots; } Resolved;

static Resolved resolve_from(shcl_doc *d, size_t *start, size_t nstart, Segment *segs, size_t nsegs) {
	Arena *a = &d->arena;
	VecSize cur = {0};
	for (size_t i = 0; i < nstart; i++) VecSize_push(a, &cur, start[i]);
	for (size_t si = 0; si < nsegs; si++) {
		Segment *seg = &segs[si];
		VecSize next = {0};
		for (size_t k = 0; k < cur.len; k++) {
			VecSize ch = NODE(d, cur.data[k]).children;
			for (size_t j = 0; j < ch.len; j++) { size_t c = ch.data[j]; if (s_eq(NODE(d, c).name, seg->name)) VecSize_push(a, &next, c); }
		}
		switch (seg->sel.tag) {
		case SEL_NONE: cur = next; break;
		case SEL_VALUE: {
			VecSize f = {0};
			for (size_t k = 0; k < next.len; k++) if (s_eq(value_display(a, &NODE(d, next.data[k]).value), seg->sel.value)) VecSize_push(a, &f, next.data[k]);
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
	PathScan ps = scan_path(&d->arena, path);
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
// spec never drops one silently.
static shcl_status array_elements(shcl_doc *d, S path, Element ***els, shcl_status **sts, size_t *n) {
	Arena *a = &d->arena;
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
size_t shcl_instances(shcl_doc *d, const char *path, size_t plen, shcl_str **out) {
	// Wildcard slots that did not resolve stay in the list as "" so indices
	// keep matching shcl_count.
	Arena *a = &d->arena; S p; p.p = path; p.n = plen;
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

shcl_read_i64 shcl_read_int(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_i64 R; S p; p.p = path; p.n = plen; Element *el; shcl_status st = scalar_at(d, p, &el);
	if (st != SHCL_GOOD) { R.value = 0; R.status = st; return R; }
	int64_t v; if (parse_int_text(&d->arena, el, d->strictness, &v)) { R.value = v; R.status = SHCL_GOOD; }
	else { R.value = 0; R.status = SHCL_BAD_TYPE; }
	return R;
}
shcl_read_f64 shcl_read_float(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_f64 R; S p; p.p = path; p.n = plen; Element *el; shcl_status st = scalar_at(d, p, &el);
	if (st != SHCL_GOOD) { R.value = 0; R.status = st; return R; }
	double v; if (parse_float_text(&d->arena, el, d->strictness, &v)) { R.value = v; R.status = SHCL_GOOD; }
	else { R.value = 0; R.status = SHCL_BAD_TYPE; }
	return R;
}
shcl_read_bool shcl_read_bool_(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_bool R; S p; p.p = path; p.n = plen; Element *el; shcl_status st = scalar_at(d, p, &el);
	if (st != SHCL_GOOD) { R.value = 0; R.status = st; return R; }
	int v; if (parse_bool_text(&d->arena, el->text, d->strictness, &v)) { R.value = v; R.status = SHCL_GOOD; }
	else { R.value = 0; R.status = SHCL_BAD_TYPE; }
	return R;
}
shcl_read_dt shcl_read_datetime(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_dt R; S p; p.p = path; p.n = plen; Element *el; shcl_status st = scalar_at(d, p, &el);
	memset(&R.value, 0, sizeof R.value); R.value.zone = SHCL_ZONE_NONE;
	if (st != SHCL_GOOD) { R.status = st; return R; }
	if (parse_datetime(&d->arena, el->text, &R.value)) R.status = SHCL_GOOD;
	else { memset(&R.value, 0, sizeof R.value); R.value.zone = SHCL_ZONE_NONE; R.status = SHCL_BAD_TYPE; }
	return R;
}
shcl_read_str shcl_read_string(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_str R; S p; p.p = path; p.n = plen; Value *v; shcl_status st = value_at(d, p, &v);
	if (st != SHCL_GOOD) { R.value = s_empty(); R.status = st; return R; }
	if (v->kind == V_EMPTY) { R.value = s_empty(); R.status = SHCL_EMPTY; }
	else if (v->kind == V_RAW) { R.value = v->content; R.status = SHCL_GOOD; }
	else if (v->nels == 1) { R.value = apply_escapes(&d->arena, v->els[0].text); R.status = SHCL_GOOD; }
	else { R.value = value_display(&d->arena, v); R.status = SHCL_GOOD; }
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

shcl_read_i64_arr shcl_read_int_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_i64_arr R; S p; p.p = path; p.n = plen; Element **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	int64_t *out = (int64_t *)arena_alloc(&d->arena, (n ? n : 1) * sizeof(int64_t));
	for (size_t i = 0; i < n; i++) { int64_t v; if (els[i] && parse_int_text(&d->arena, els[i], d->strictness, &v)) out[i] = v; else { out[i] = 0; if (els[i]) sts[i] = SHCL_BAD_TYPE; } }
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}
shcl_read_f64_arr shcl_read_float_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_f64_arr R; S p; p.p = path; p.n = plen; Element **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	double *out = (double *)arena_alloc(&d->arena, (n ? n : 1) * sizeof(double));
	for (size_t i = 0; i < n; i++) { double v; if (els[i] && parse_float_text(&d->arena, els[i], d->strictness, &v)) out[i] = v; else { out[i] = 0; if (els[i]) sts[i] = SHCL_BAD_TYPE; } }
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}
shcl_read_bool_arr shcl_read_bool_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_bool_arr R; S p; p.p = path; p.n = plen; Element **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	int *out = (int *)arena_alloc(&d->arena, (n ? n : 1) * sizeof(int));
	for (size_t i = 0; i < n; i++) { int v; if (els[i] && parse_bool_text(&d->arena, els[i]->text, d->strictness, &v)) out[i] = v; else { out[i] = 0; if (els[i]) sts[i] = SHCL_BAD_TYPE; } }
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}
shcl_read_dt_arr shcl_read_datetime_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_dt_arr R; S p; p.p = path; p.n = plen; Element **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, p, &els, &sts, &n);
	if (st != SHCL_GOOD && st != SHCL_EMPTY) { R.values = NULL; R.n = 0; R.status = st; R.statuses = NULL; return R; }
	shcl_datetime *out = (shcl_datetime *)arena_alloc(&d->arena, (n ? n : 1) * sizeof(shcl_datetime));
	for (size_t i = 0; i < n; i++) {
		memset(&out[i], 0, sizeof out[i]); out[i].zone = SHCL_ZONE_NONE;
		if (els[i]) { if (!parse_datetime(&d->arena, els[i]->text, &out[i])) { memset(&out[i], 0, sizeof out[i]); out[i].zone = SHCL_ZONE_NONE; sts[i] = SHCL_BAD_TYPE; } }
	}
	R.values = out; R.n = n; R.status = worst_slot(sts, n, st); R.statuses = sts; return R;
}
shcl_read_str_arr shcl_read_string_array(shcl_doc *d, const char *path, size_t plen) {
	shcl_read_str_arr R; S p; p.p = path; p.n = plen; Element **els; shcl_status *sts; size_t n;
	shcl_status st = array_elements(d, p, &els, &sts, &n);
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
static S emit_element(Arena *a, const Element *e) {
	S t = e->text;
	int needs = (t.n == 0);
	if (!needs) {
		size_t i = 0;
		while (i < t.n) { uint32_t c; size_t l = utf8_decode(t.p, t.n, i, &c); i += l;
			if (c == ' ' || c == '\t' || c == ',' || c == ':' || c == '#' || c == '"' || c == '\'' || c == '[' || c == ']') { needs = 1; break; } }
	}
	if (!needs) { Fence f = fence_open(a, t); if (f.ok) needs = 1; }
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
static void emit_node(shcl_doc *d, size_t idx, size_t depth, SB *out) {
	Arena *a = &d->arena;
	Node *node = &NODE(d, idx);
	for (size_t k = 0; k < depth; k++) sb_putc(a, out, '\t');
	sb_putS(a, out, emit_name(a, node->name));
	sb_putc(a, out, ':');
	Value *v = &node->value;
	if (v->kind == V_EMPTY) sb_putc(a, out, '\n');
	else if (v->kind == V_CELL) {
		sb_putc(a, out, ' ');
		for (size_t i = 0; i < v->nels; i++) { if (i) sb_puts(a, out, ", "); sb_putS(a, out, emit_element(a, &v->els[i])); }
		sb_putc(a, out, '\n');
	} else {
		size_t parent = node->parent;
		VecSize pch = NODE(d, parent).children; size_t mypos = (size_t)-1;
		for (size_t k = 0; k < pch.len; k++) if (pch.data[k] == idx) { mypos = k; break; }
		int would_merge = 0;
		if (mypos != (size_t)-1)
			for (size_t k = 0; k < mypos; k++) { size_t c = pch.data[k]; if (s_eq(NODE(d, c).name, node->name) && v_is_empty(&NODE(d, c).value)) { would_merge = 1; break; } }
		if (would_merge) sb_putc(a, out, ' '); else sb_putc(a, out, '\n');
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
	for (size_t k = 0; k < ch.len; k++) emit_node(d, ch.data[k], depth + 1, out);
}
shcl_str shcl_to_canonical(shcl_doc *d) {
	SB out = {0};
	VecSize rc = NODE(d, ROOT).children;
	for (size_t k = 0; k < rc.len; k++) emit_node(d, rc.data[k], 0, &out);
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
	char *o = out;
	if (dt->has_date) { o += sprintf(o, "%04d-%02u-%02u", dt->year, dt->month, dt->day); if (dt->has_time) *o++ = 'T'; }
	if (dt->has_time) {
		o += sprintf(o, "%02u:%02u", dt->hour, dt->minute);
		if (dt->has_sec) o += sprintf(o, ":%02u", dt->sec);
		if (dt->has_frac) { *o++ = '.'; memcpy(o, dt->frac.p, dt->frac.n); o += dt->frac.n; }
	}
	if (dt->zone == SHCL_ZONE_UTC) *o++ = 'Z';
	else if (dt->zone == SHCL_ZONE_OFFSET) { int off = dt->off_min; char sign = off < 0 ? '-' : '+'; int ao = off < 0 ? -off : off; o += sprintf(o, "%c%02d:%02d", sign, ao / 60, ao % 60); }
	return (size_t)(o - out);
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
void shcl_free(shcl_doc *d) { if (!d) return; arena_free(&d->arena); free(d); }
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

#endif // SHCL_IMPLEMENTATION
#endif // SHCL_H
