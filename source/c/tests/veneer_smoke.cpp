// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

// Compile + behavior smoke for the C++ veneer. Full behavior is pinned by the C
// core's conformance runner; this just proves the typed surface builds and
// delegates correctly, so the header can't silently rot. Exit nonzero on a miss.

#define SHCL_IMPLEMENTATION
#include "shcl.hpp"

#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "veneer FAIL: %s (line %d)\n", #cond, __LINE__); fails++; } } while (0)

int main() {
	const std::string src =
		"name: demo\n"
		"port: 8080\n"
		"ratio: 3.5\n"
		"on: yes\n"
		"tags: red, green, blue\n"
		"city: Chicago\n"
		"city: Boston\n";

	auto doc = shcl::Document::parse(src);

	auto port = doc.get<int64_t>("port");
	CHECK(port.ok() && port.status == shcl::Status::Good && port.value == 8080);

	auto ratio = doc.read_float("ratio");
	CHECK(ratio.ok() && ratio.value == 3.5);

	auto name = doc.get<std::string>("name");
	CHECK(name.value == "demo");

	// yes is a Standard boolean.
	auto on = doc.read_bool("on");
	CHECK(on.ok() && on.value == true);

	auto tags = doc.read_string_array("tags");
	CHECK(tags.ok() && tags.value.size() == 3 && tags.value[1] == "green");

	// Two same-name leaves are instances, not one scalar.
	CHECK(doc.count("city") == 2);
	auto cities = doc.instances("city");
	CHECK(cities.size() == 2 && cities[0] == "Chicago" && cities[1] == "Boston");
	auto multi = doc.read_string("city");
	CHECK(multi.status == shcl::Status::Multiple);

	// Loose strictness widens coercions.
	auto loose = shcl::Document::parse_with("pct: 50%\n", shcl::Strictness::Loose);
	auto pct = loose.read_float("pct");
	CHECK(pct.ok() && pct.value == 0.5);

	// Canonical form is stable and re-parseable.
	std::string canon = doc.to_canonical();
	auto again = shcl::Document::parse(canon);
	CHECK(again.to_canonical() == canon);

	auto missing = doc.read_int("nope");
	CHECK(missing.status == shcl::Status::NotFound);

	if (fails) { std::fprintf(stderr, "veneer: %d failure(s)\n", fails); return 1; }
	std::printf("veneer: ok\n");
	return 0;
}
