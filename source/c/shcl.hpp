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

template <class T> struct Read {
	T value{};
	Status status{};
	bool ok() const { return status == Status::Good || status == Status::Empty; }
};

struct Diagnostic { std::size_t line; bool is_error; std::string message; };

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

	bool strict_failed() const { return shcl_strict_failed(d_) != 0; }
	Strictness strictness() const { return static_cast<Strictness>(shcl_strictness_of(d_)); }
	std::string to_canonical() const { return to_str(shcl_to_canonical(d_)); }

	std::vector<Diagnostic> diagnostics() const {
		std::vector<Diagnostic> v; std::size_t n = shcl_diag_count(d_);
		for (std::size_t i = 0; i < n; i++)
			v.push_back({shcl_diag_line(d_, i), shcl_diag_severity(d_, i) == SHCL_SEV_ERROR, to_str(shcl_diag_message(d_, i))});
		return v;
	}

	std::size_t count(std::string_view p) const { return shcl_count(d_, p.data(), p.size()); }
	std::vector<std::string> instances(std::string_view p) const {
		shcl_str *a; std::size_t n = shcl_instances(d_, p.data(), p.size(), &a);
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
	// Structured datetime, if the caller wants the parsed fields.
	Read<shcl_datetime> read_datetime_raw(std::string_view p) const { auto r = shcl_read_datetime(d_, p.data(), p.size()); return {r.value, st(r.status)}; }

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
	template <class T> Read<T> get(std::string_view p) const;

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
