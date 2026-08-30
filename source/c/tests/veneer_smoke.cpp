// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

// Compile + behavior smoke for the C++ veneer. Full behavior is pinned by the C
// core's conformance runner; this just proves the typed surface builds and
// delegates correctly, so the header can't silently rot. Exit nonzero on a miss.

#define SHCL_IMPLEMENTATION
#include "shcl.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#ifndef SHCL_NO_FILE_IO
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>   // _mkdir - windows' mkdir takes no mode argument
#endif
#endif

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

	CHECK(doc.quote_segment("port") == "port");
	CHECK(doc.quote_segment("q n") == "\"q n\"");
	auto cities = doc.instances("city");
	CHECK(cities.size() == 2 && cities[0] == "Chicago" && cities[1] == "Boston");

	CHECK(doc.line("port") == 2 && doc.line("nope") == 0);
	auto cityLines = doc.lines("city");
	CHECK(cityLines.size() == 2 && cityLines[0] == 6 && cityLines[1] == 7);
	CHECK(doc.lines("nope").empty());
	auto kids = doc.children("");
	CHECK(kids.size() == 7 && kids[0] == "name" && kids[5] == "city" && kids[6] == "city");
	CHECK(doc.children("nope").empty());
	CHECK(doc.write_reason("port") == shcl::WriteReason::Writable);
	CHECK(doc.write_reason("city[*]") == shcl::WriteReason::Wildcard);
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

	// Convenience tier: value on Good, call-site fallback otherwise.
	CHECK(doc.get_or<int64_t>("port", 9) == 8080);
	CHECK(doc.get_or<int64_t>("nope", 9) == 9);
	CHECK(doc.get_or<std::string>("nope", std::string("fb")) == "fb");

	// The rest of the read surface the veneer used to be missing: the quoted
	// flag, existence, per-slot array statuses, and the datetime array.
	auto qd = shcl::Document::parse("a: @null\nb: \"@null\"\nnums: 1, x, 3\nwhen: 2026-08-02, nope\n");
	CHECK(!qd.quoted("a") && qd.quoted("b") && !qd.quoted("nope"));
	CHECK(qd.exists("a") && !qd.exists("nope"));
	auto nums = qd.read_int_array("nums");
	CHECK(nums.slots.size() == 3 && nums.slots[0] == shcl::Status::Good && nums.slots[1] == shcl::Status::BadType);
	CHECK(qd.read_int("a").slots.empty()); // scalar reads carry no slots
	auto when = qd.read_datetime_array("when");
	CHECK(when.value.size() == 2 && when.value[0] == "2026-08-02" && when.slots[1] == shcl::Status::BadType);
	CHECK(std::string(shcl::to_string(shcl::Status::BadType)) == "BadType");

	// The rest of the surface, item by item: strictness, strict_failed, paths,
	// authored_name, the raw pair, the float/bool arrays, and the typed get
	// over double and bool.
	CHECK(doc.strictness() == shcl::Strictness::Standard);
	CHECK(!doc.strict_failed());
	auto strictDoc = shcl::Document::parse_with("x 1\n", shcl::Strictness::Strict);
	CHECK(strictDoc.strict_failed());
	auto allPaths = doc.paths();
	CHECK(allPaths.size() == 6 && allPaths[0] == "name" && allPaths[5] == "city");
	auto spelled = shcl::Document::parse("SYMBOLS: 3\n");
	CHECK(spelled.authored_name("symbols") == "SYMBOLS");
	CHECK(spelled.authored_name("missing").empty());
	auto rawDoc = shcl::Document::parse("blk:\n\t```sql\n\tselect 1\n\t```\n");
	auto rb = rawDoc.read_raw("blk");
	CHECK(rb.ok() && rb.value == "select 1");
	CHECK(rawDoc.read_raw_info("blk").value == "sql");
	CHECK(rawDoc.read_raw("missing").status == shcl::Status::NotFound);
	auto fa = qd.read_float_array("nums");
	CHECK(fa.slots.size() == 3 && fa.slots[1] == shcl::Status::BadType && fa.value[2] == 3.0);
	auto ba = shcl::Document::parse("flags: true, false, x\n").read_bool_array("flags");
	CHECK(ba.value.size() == 3 && ba.value[0] && !ba.value[1] && ba.slots[2] == shcl::Status::BadType);
	CHECK(doc.get<double>("ratio").value == 3.5);
	CHECK(doc.get<bool>("on").value == true);
	CHECK(doc.get_or<double>("ratio", 0.0) == 3.5);
	CHECK(doc.get_or<double>("nope", 1.25) == 1.25);
	CHECK(doc.get_or<bool>("on", false) == true);
	CHECK(doc.get_or<bool>("nope", true) == true);

	// Schema validation rides through the veneer: a conforming doc is clean, a
	// violation carries its stable V-code, and a key-level schema fault still
	// lets the unknown-field sweep run (the faulted entry keeps its path).
	auto schema = shcl::Document::parse("field: port\n\ttype: int\n\tmin: 1\nfield: city\nfield: ratio\nfield: name\nfield: on\nfield: tags\n");
	CHECK(doc.validate(schema).empty());
	auto badschema = shcl::Document::parse("field: port\n\ttype: int\n\tmin: 90000\n");
	auto vd = doc.validate(badschema);
	CHECK(vd.size() >= 1 && vd[0].code == "V005");
	auto broken = shcl::Document::parse("field: port\n\tfrobnicate: 1\n");
	auto fd = doc.validate(broken);
	CHECK(fd.size() >= 2 && fd[0].code == "V090" && fd.back().code == "V001");

	// Layered loading: overlay a higher-priority doc; leaf override, container merge.
	auto base = shcl::Document::parse("port: 8080\nserver: web1\n\tport: 80\n");
	auto over = shcl::Document::parse("port: 9090\nserver: web1\n\thost: h1\n");
	base.merge(over);
	CHECK(base.get_or<int64_t>("port", 0) == 9090);
	CHECK(base.get_or<int64_t>("server[web1].port", 0) == 80);
	CHECK(base.get_or<std::string>("server[web1].host", std::string()) == "h1");

	// Schema-driven generation: a starter config with a required live field.
	// The default run appends the format footer; --no-banner is the same bytes
	// without it.
	auto gschema = shcl::Document::parse("field: port\n\ttype: int\n\trequired: yes\n\tdefault: 8080\n");
	auto [bare, bareOk] = gschema.generate(true);
	CHECK(bareOk && bare == "# int, required\nport: 8080\n");
	auto [starter, starterOk] = gschema.generate();
	CHECK(starterOk && starter.rfind(bare, 0) == 0);
	CHECK(starter.find("This config file format is SHCL.", bare.size()) != std::string::npos);
	auto faulty = shcl::Document::parse("field: port\n\tfrobnicate: 1\n").generate();
	CHECK(!faulty.second && faulty.first.empty());
	// A default-constructed Document is an empty one, not a null handle.
	shcl::Document blank;
	CHECK(blank.to_canonical().empty() && blank.count("x") == 0 && blank.diagnostics().empty());

	// One-shot: one combined list (parse then validation), error predicate.
	auto combined = shcl::Document::load_and_validate(": nope\nport: x\n", "field: port\n\ttype: int\n", shcl::Strictness::Standard);
	auto cdiags = combined.diagnostics();
	CHECK(cdiags.size() == 2 && cdiags[0].code == "E014" && cdiags[1].code == "V003");
	CHECK(combined.error_count() == 2);
	CHECK(combined.get_or<std::string>("port", std::string()) == "x"); // doc still usable
	auto clean = shcl::Document::load_and_validate("a: 1\n", "", shcl::Strictness::Standard);
	CHECK(clean.error_count() == 0 && clean.diagnostics().empty());

	// A read must stay usable after the document it came from is gone; the
	// datetime one used to hand back a pointer into the freed arena.
	shcl::Read<shcl::Datetime> outlives;
	{
		auto tmp = shcl::Document::parse("t: 2026-08-02T10:20:30.123456789Z\n");
		outlives = tmp.read_datetime("t");
		// read_datetime is the structured read, matching every other binding;
		// read_datetime_str is the textual one. The old backwards pair is gone.
		CHECK(tmp.read_datetime_str("t").value == outlives.value.str());
	}
	CHECK(outlives.status == shcl::Status::Good);
	CHECK(outlives.value.str() == "2026-08-02T10:20:30.123456789Z");
	auto copied = outlives;
	CHECK(copied.value.str() == outlives.value.str());
	auto moved = std::move(copied);
	CHECK(moved.value.str() == outlives.value.str());
	// The moved-from half is the half that used to be wrong: it kept a view into
	// the digits it had just handed away, so it formatted a fraction it no longer
	// held. Reading it is legal - unspecified value, not undefined behavior.
	CHECK(copied.value.str() == "2026-08-02T10:20:30Z");
	shcl::Datetime target;
	target = std::move(moved.value);
	CHECK(target.str() == outlives.value.str());
	CHECK(moved.value.str() == "2026-08-02T10:20:30Z");

#ifndef SHCL_NO_FILE_IO
	// The file tier through the veneer: the load status, the strictness form,
	// the lost count, and a save gate whose refusal is a value the caller can
	// act on - which is the whole reason save_file is not a bool.
	{
		// TMPDIR is the POSIX spelling; windows sets TEMP/TMP and has no /tmp.
		const char *tmp = std::getenv("TMPDIR");
		if (!tmp || !*tmp) tmp = std::getenv("TEMP");
		if (!tmp || !*tmp) tmp = std::getenv("TMP");
		std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/shcl-veneer-" + std::to_string((long)getpid());
#ifdef _WIN32
		CHECK(_mkdir(dir.c_str()) == 0);
#else
		CHECK(mkdir(dir.c_str(), 0700) == 0);
#endif
		std::string f = dir + "/t.shcl";
		auto st = shcl::Document::FileStatus::Clean;
		auto absent = shcl::Document::load_file(f, &st);
		CHECK(st == shcl::Document::FileStatus::NotFound);
		CHECK(std::string(shcl::Document::to_string(st)) == "NotFound");
		{ FILE *fh = std::fopen(f.c_str(), "wb"); CHECK(fh && std::fputs("pct: 50%\n", fh) != EOF && std::fclose(fh) == 0); }
		auto strict = shcl::Document::load_file_with(f, shcl::Strictness::Loose, &st);
		CHECK(st == shcl::Document::FileStatus::Clean && strict.read_float("pct").value == 0.5);
		CHECK(shcl::Document::read_file(f, 0, &st) == std::string("pct: 50%\n") && st == shcl::Document::FileStatus::Clean);
		CHECK(!shcl::Document::read_file(f, 8, &st) && st == shcl::Document::FileStatus::Unreadable);
		CHECK(strict.lost_count() == 0);
		CHECK(strict.save_file(f) == shcl::Document::SaveResult::Ok);
		auto lost = shcl::Document::parse("a:\n\tb: 1\n  c: 2\n"); // indent matches no level
		CHECK(lost.lost_count() == 1);
		CHECK(lost.save_file(f) == shcl::Document::SaveResult::Refused);
		CHECK(lost.save_file_lossy(f) == shcl::Document::SaveResult::Ok);
		CHECK(strict.save_file(dir + "/nope/t.shcl") == shcl::Document::SaveResult::Failed);
		std::remove(f.c_str());
		rmdir(dir.c_str());
	}
#endif

	// The veneer hands the core's read memory back on the next read, so a read
	// loop on one long-lived Document stays flat instead of climbing until the
	// Document is destroyed.
	{
		const std::string t = "ports: 80, 443, 8080\n";
		shcl_doc *raw = shcl_parse(t.data(), t.size());
		shcl::Document held(raw);   // takes ownership; raw stays valid alongside
		for (int i = 0; i < 20000; i++) {
			auto r = held.read_int_array("ports");
			CHECK(r.status == shcl::Status::Good && r.value.size() == 3);
			if (r.status != shcl::Status::Good) break;
		}
		std::size_t held_bytes = 0;
		for (const ShclBlock *b = raw->reads.head; b; b = b->next) held_bytes += b->used;
		CHECK(held_bytes <= 4096);
	}

	if (fails) { std::fprintf(stderr, "veneer: %d failure(s)\n", fails); return 1; }
	std::printf("veneer: ok\n");
	return 0;
}
