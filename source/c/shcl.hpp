// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

// C++ typed veneer over the C core (shcl.h). This is NOT a second parser: it
// wraps the same shcl_* functions and adds a compile-time-typed surface
// (Read<T>, get<T>()), so it inherits the core's conformance. Drop shcl.h and
// shcl.hpp into your tree; in one TU, #define SHCL_IMPLEMENTATION before either.

#ifndef SHCL_HPP
#define SHCL_HPP

#include "shcl.h"

#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace shcl {

enum class Strictness { Loose = SHCL_LOOSE, Standard = SHCL_STANDARD, Strict = SHCL_STRICT };
enum class Status { Good = SHCL_GOOD, Empty = SHCL_EMPTY, NotFound = SHCL_NOT_FOUND, BadType = SHCL_BAD_TYPE, Multiple = SHCL_MULTIPLE };
enum class WriteReason { Writable = SHCL_W_WRITABLE, BadPath = SHCL_W_BAD_PATH, ValueInPath = SHCL_W_VALUE_IN_PATH, Wildcard = SHCL_W_WILDCARD, NoSuchIndex = SHCL_W_NO_SUCH_INDEX, TooDeep = SHCL_W_TOO_DEEP };

template <class T> struct Read {
	T value{};
	Status status{};
	bool ok() const { return status == Status::Good || status == Status::Empty; }
};

struct Diagnostic { std::size_t line; bool is_error; std::string message; std::string code; };

// Owning structured datetime. The core's shcl_datetime borrows its frac digits
// from the document's arena; this copies them so the value keeps the veneer's
// RAII promise and may outlive the Document. Copies/moves re-point the C view
// at their own storage.
class Datetime {
	shcl_datetime v_{};
	std::string frac_;
	void rebind() { v_.frac.p = frac_.data(); v_.frac.n = frac_.size(); }
public:
	Datetime() { v_.zone = SHCL_ZONE_NONE; }
	explicit Datetime(const shcl_datetime &v) : v_(v), frac_(v.frac.p ? std::string(v.frac.p, v.frac.n) : std::string()) { rebind(); }
	Datetime(const Datetime &o) : v_(o.v_), frac_(o.frac_) { rebind(); }
	Datetime &operator=(const Datetime &o) { v_ = o.v_; frac_ = o.frac_; rebind(); return *this; }
	Datetime(Datetime &&o) noexcept : v_(o.v_), frac_(std::move(o.frac_)) { rebind(); }
	Datetime &operator=(Datetime &&o) noexcept { v_ = o.v_; frac_ = std::move(o.frac_); rebind(); return *this; }
	// The C view, for shcl_datetime_str and friends: valid as long as *this.
	const shcl_datetime &c() const noexcept { return v_; }
	// The reference's textual form.
	std::string str() const { char b[64]; return std::string(b, shcl_datetime_str(&v_, b)); }
};

inline std::string to_str(shcl_str s) { return std::string(s.p, s.n); }

class Document {
	shcl_doc *d_ = nullptr;
	static Status st(shcl_status s) { return static_cast<Status>(s); }
public:
	Document() = default;
	explicit Document(shcl_doc *d) : d_(d) {}
	Document(const Document &) = delete;
	Document &operator=(const Document &) = delete;
	Document(Document &&o) noexcept : d_(o.d_) { o.d_ = nullptr; }
	Document &operator=(Document &&o) noexcept { if (this != &o) { if (d_) shcl_free(d_); d_ = o.d_; o.d_ = nullptr; } return *this; }
	~Document() { if (d_) shcl_free(d_); }

	static Document parse(std::string_view t) { return Document(shcl_parse(t.data(), t.size())); }
	static Document parse_with(std::string_view t, Strictness s) { return Document(shcl_parse_with(t.data(), t.size(), static_cast<shcl_strictness>(s))); }

	// One-shot load-and-validate: parse at a strictness, validate against a
	// schema, and hand back a document whose diagnostics() serve ONE combined
	// list (parse first, then validation). Never fails: error_count() answers
	// "did it fail". An empty schema text skips validation entirely; H001
	// hints the schema disavows (declared repeat upper bound above 1) are
	// dropped.
	static Document load_and_validate(std::string_view text, std::string_view schema, Strictness s) {
		return Document(shcl_load_and_validate(text.data(), text.size(), schema.data(), schema.size(), static_cast<shcl_strictness>(s)));
	}

	bool strict_failed() const { return shcl_strict_failed(d_) != 0; }
	Strictness strictness() const { return static_cast<Strictness>(shcl_strictness_of(d_)); }
	std::string to_canonical() const { return to_str(shcl_to_canonical(d_)); }

	std::vector<Diagnostic> diagnostics() const {
		std::vector<Diagnostic> v; std::size_t n = shcl_diag_count(d_);
		for (std::size_t i = 0; i < n; i++)
			v.push_back({shcl_diag_line(d_, i), shcl_diag_severity(d_, i) == SHCL_SEV_ERROR, to_str(shcl_diag_message(d_, i)), shcl_diag_code(d_, i)});
		return v;
	}

	// How many error-severity diagnostics the document carries - the "did
	// this file have errors?" predicate. After load_and_validate, that
	// includes validation errors.
	std::size_t error_count() const { return shcl_error_count(d_); }

	// Schema validation (spec.md "Schema validation"): empty result = conforms.
	// Schema faults (V09x, schema-file lines) come first; the surviving
	// constraints still check the document, and only the unknown-field sweep
	// needs a fault-free schema.
	std::vector<Diagnostic> validate(const Document &schema) const {
		std::vector<Diagnostic> v;
		shcl_validation *r = shcl_validate(d_, schema.d_);
		std::size_t n = shcl_validation_count(r);
		for (std::size_t i = 0; i < n; i++)
			v.push_back({shcl_validation_line(r, i), shcl_validation_severity(r, i) == SHCL_SEV_ERROR, to_str(shcl_validation_message(r, i)), shcl_validation_code(r, i)});
		shcl_validation_free(r);
		return v;
	}

	// Layered loading: overlay `over` (a higher-priority layer) onto this doc.
	// Leaf names in `over` override; container instances merge by (name, value).
	void merge(const Document &over) { shcl_merge(d_, over.d_); }

	// Schema-driven generation (`shcl init`): a commented, typed starter config
	// from this document read as a schema. ok is set false on schema faults;
	// for the fault list, validate() an empty document against this schema -
	// it reproduces the same V09x diagnostics. A footer naming the format and
	// pointing at the spec is written last unless no_banner.
	std::string generate(bool &ok, bool no_banner = false) const {
		int iok = 0;
		std::string s = to_str(shcl_generate(d_, no_banner ? 1 : 0, &iok));
		ok = iok != 0;
		return s;
	}

	std::size_t count(std::string_view p) const { return shcl_count(d_, p.data(), p.size()); }
	// Every field path, file order, deduplicated (bare-name-safe segments only).
	// Quote one path segment for splicing into a lookup path (injection-safe).
	std::string quote_segment(std::string_view name) const { return to_str(shcl_quote_segment(d_, name.data(), name.size())); }

	std::vector<std::string> paths() const {
		shcl_str *v; std::size_t n = shcl_paths(d_, &v);
		std::vector<std::string> r; r.reserve(n);
		for (std::size_t i = 0; i < n; i++) r.push_back(to_str(v[i]));
		return r;
	}

	std::vector<std::string> instances(std::string_view p) const {
		shcl_str *a; std::size_t n = shcl_instances(d_, p.data(), p.size(), &a);
		std::vector<std::string> v; v.reserve(n);
		for (std::size_t i = 0; i < n; i++) v.push_back(to_str(a[i]));
		return v;
	}

	// 1-based source line of the binding at a path; 0 when it does not resolve
	// to exactly one node or the node was writer-built.
	std::size_t line(std::string_view p) const { return shcl_line(d_, p.data(), p.size()); }

	// The plural line(): 1-based source lines at a path, in file order, so a
	// repeated field - the case that most wants a citable line - yields every
	// binding's.
	std::vector<std::size_t> lines(std::string_view p) const {
		std::size_t *a; std::size_t n = shcl_lines(d_, p.data(), p.size(), &a);
		std::vector<std::size_t> v; v.reserve(n);
		for (std::size_t i = 0; i < n; i++) v.push_back(a[i]);
		return v;
	}

	// Why a write at a path would fail - the reason behind a setter's bare
	// failure. Probes only; never creates.
	WriteReason write_reason(std::string_view p) const { return static_cast<WriteReason>(shcl_write_reason_(d_, p.data(), p.size())); }

	// Child field names under a path, file order, duplicates included; "" is
	// the top level. Names as stored - quote_segment() splices one into a path.
	std::vector<std::string> children(std::string_view p) const {
		shcl_str *a; std::size_t n = shcl_children(d_, p.data(), p.size(), &a);
		std::vector<std::string> v; v.reserve(n);
		for (std::size_t i = 0; i < n; i++) v.push_back(to_str(a[i]));
		return v;
	}

	Read<int64_t> read_int(std::string_view p) const { auto r = shcl_read_int(d_, p.data(), p.size()); return {r.value, st(r.status)}; }
	Read<double> read_float(std::string_view p) const { auto r = shcl_read_float(d_, p.data(), p.size()); return {r.value, st(r.status)}; }
	Read<bool> read_bool(std::string_view p) const { auto r = shcl_read_bool_(d_, p.data(), p.size()); return {r.value != 0, st(r.status)}; }
	Read<std::string> read_string(std::string_view p) const { auto r = shcl_read_string(d_, p.data(), p.size()); return {to_str(r.value), st(r.status)}; }
	Read<std::string> read_raw(std::string_view p) const { auto r = shcl_read_raw(d_, p.data(), p.size()); return {to_str(r.value), st(r.status)}; }
	Read<std::string> read_raw_info(std::string_view p) const { auto r = shcl_read_raw_info(d_, p.data(), p.size()); return {to_str(r.value), st(r.status)}; }

	// Datetime as the reference's textual form (the common need).
	Read<std::string> read_datetime(std::string_view p) const {
		auto r = shcl_read_datetime(d_, p.data(), p.size());
		char buf[64]; std::size_t k = shcl_datetime_str(&r.value, buf);
		return {std::string(buf, k), st(r.status)};
	}
	// Structured datetime, if the caller wants the parsed fields. Owning
	// (unlike the core's shcl_read_datetime), so it may outlive the Document.
	Read<Datetime> read_datetime_raw(std::string_view p) const { auto r = shcl_read_datetime(d_, p.data(), p.size()); return {Datetime(r.value), st(r.status)}; }

	Read<std::vector<int64_t>> read_int_array(std::string_view p) const { auto r = shcl_read_int_array(d_, p.data(), p.size()); return {std::vector<int64_t>(r.values, r.values + r.n), st(r.status)}; }
	Read<std::vector<double>> read_float_array(std::string_view p) const { auto r = shcl_read_float_array(d_, p.data(), p.size()); return {std::vector<double>(r.values, r.values + r.n), st(r.status)}; }
	Read<std::vector<bool>> read_bool_array(std::string_view p) const {
		auto r = shcl_read_bool_array(d_, p.data(), p.size());
		std::vector<bool> v; v.reserve(r.n); for (std::size_t i = 0; i < r.n; i++) v.push_back(r.values[i] != 0);
		return {std::move(v), st(r.status)};
	}
	Read<std::vector<std::string>> read_string_array(std::string_view p) const {
		auto r = shcl_read_string_array(d_, p.data(), p.size());
		std::vector<std::string> v; v.reserve(r.n); for (std::size_t i = 0; i < r.n; i++) v.push_back(to_str(r.values[i]));
		return {std::move(v), st(r.status)};
	}

	// Compile-time-typed read: get<int64_t>/get<double>/get<bool>/get<std::string>.
	// Any other T - a bare `int` included - fails right here with this message,
	// not as a bare undefined-symbol link error.
	template <class T> Read<T> get(std::string_view) const {
		static_assert(sizeof(T) == 0,
			"shcl::Document::get<T>: T must be exactly int64_t, double, bool, or std::string");
		return {};
	}

	// Convenience tier: the value, or the call-site fallback unless Good - so a
	// missing/empty/bad/ambiguous read cannot masquerade as a real zero.
	template <class T> T get_or(std::string_view p, T def) const {
		auto r = get<T>(p);
		return r.status == Status::Good ? r.value : def;
	}
};

template <> inline Read<int64_t> Document::get<int64_t>(std::string_view p) const { return read_int(p); }
template <> inline Read<double> Document::get<double>(std::string_view p) const { return read_float(p); }
template <> inline Read<bool> Document::get<bool>(std::string_view p) const { return read_bool(p); }
template <> inline Read<std::string> Document::get<std::string>(std::string_view p) const { return read_string(p); }

} // namespace shcl

#endif // SHCL_HPP
