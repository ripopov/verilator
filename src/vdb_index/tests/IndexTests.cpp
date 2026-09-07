// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: verilator_vdb_index tests
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2026 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "SourceIndex.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace vdb_index;

namespace {

int failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            ++failures; \
            std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #cond "\n"; \
        } \
    } while (false)

#define CHECK_EQ(a, b) \
    do { \
        const auto va = (a); \
        const auto vb = (b); \
        if (!(va == vb)) { \
            ++failures; \
            std::cerr << __FILE__ << ':' << __LINE__ << ": " #a " != " #b " (" << va << " vs " \
                      << vb << ")\n"; \
        } \
    } while (false)

std::string readFile(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

const FileIndex* file(const SourceIndex& index, const std::string& path) {
    for (const auto& f : index.files) {
        if (f.path == path) return &f;
    }
    return nullptr;
}

// The token starting at `line:column`, or a null-length token when absent.
Token at(const FileIndex& f, uint32_t line, uint32_t column) {
    for (const Token& token : f.tokens) {
        if (token.line == line && token.column == column) return token;
    }
    std::cerr << "no token at " << f.path << ':' << line << ':' << column << '\n';
    return Token{0, 0, 0, TokenClass::Count, 0, {}};
}

uint32_t mod(Modifier m) { return static_cast<uint32_t>(m); }

void testIndex(const std::string& fixtures) {
    const std::string document = readFile(fixtures + "/design.vdb.json");
    const IndexRequest request = requestFromVdb(parseJson(document), fixtures);
    CHECK_EQ(request.top, std::string{"top"});
    CHECK_EQ(request.elaboration.includeDirs.size(), size_t{1});
    const SourceIndex index = buildIndex(request);
    CHECK_EQ(index.errors, size_t{0});
    CHECK_EQ(index.files.size(), size_t{2});
    const FileIndex* design = file(index, "design.sv");
    const FileIndex* defs = file(index, "include/defs.svh");
    CHECK(design && defs);
    if (!design || !defs) return;
    const int32_t designIdx = design == &index.files[0] ? 0 : 1;

    // Declarations: module, ports with directions, the clock heuristic.
    Token t = at(*design, 28, 8);  // module top
    CHECK(t.cls == TokenClass::Module);
    CHECK_EQ(t.modifiers, mod(Modifier::Declaration));
    t = at(*design, 17, 15);  // lane's clk port
    CHECK(t.cls == TokenClass::Variable);
    CHECK_EQ(t.modifiers,
             mod(Modifier::Declaration) | mod(Modifier::Input) | mod(Modifier::Clock));
    t = at(*design, 17, 32);  // lane's rst_n: sampled by the edge and read as data
    CHECK_EQ(t.modifiers, mod(Modifier::Declaration) | mod(Modifier::Input));
    t = at(*design, 17, 83);  // lane's q
    CHECK_EQ(t.modifiers, mod(Modifier::Declaration) | mod(Modifier::Output));
    CHECK_EQ(t.length, 1u);

    // References carry the declaration they denote.
    t = at(*design, 24, 38);  // sat(d)
    CHECK(t.cls == TokenClass::Function);
    CHECK(t.declaration == (Location{designIdx, 5, 34}));
    t = at(*design, 29, 3);  // pkg::pkt_t
    CHECK(t.cls == TokenClass::Package);
    CHECK(t.declaration == (Location{designIdx, 2, 9}));
    t = at(*design, 29, 8);
    CHECK(t.cls == TokenClass::Type);
    CHECK(t.declaration == (Location{designIdx, 4, 60}));
    t = at(*design, 33, 11);  // .W(8)
    CHECK(t.cls == TokenClass::Parameter);
    CHECK(t.declaration == (Location{designIdx, 16, 29}));
    t = at(*design, 33, 61);  // .d(in)
    CHECK(t.cls == TokenClass::Variable);
    CHECK_EQ(t.modifiers, mod(Modifier::Input));
    CHECK(t.declaration == (Location{designIdx, 17, 59}));
    t = at(*design, 36, 27);  // u_fast.q[0]: the instance
    CHECK(t.cls == TokenClass::Instance);
    CHECK(t.declaration == (Location{designIdx, 33, 27}));
    t = at(*design, 36, 34);  // and its port
    CHECK(t.cls == TokenClass::Variable);
    CHECK_EQ(t.modifiers, mod(Modifier::Output));
    CHECK(t.declaration == (Location{designIdx, 17, 83}));
    t = at(*design, 36, 46);  // pkt.data: struct field
    CHECK(t.cls == TokenClass::Property);
    CHECK(t.declaration == (Location{designIdx, 4, 52}));
    t = at(*design, 33, 3);  // lane instantiation names the module
    CHECK(t.cls == TokenClass::Module);
    CHECK(t.declaration == (Location{designIdx, 16, 8}));
    t = at(*design, 30, 3);  // interface instantiation
    CHECK(t.cls == TokenClass::Interface);
    t = at(*design, 19, 3);  // state_t through the wildcard import
    CHECK(t.cls == TokenClass::Type);
    CHECK(t.declaration == (Location{designIdx, 3, 40}));
    t = at(*design, 18, 10);  // import pkg::*
    CHECK(t.cls == TokenClass::Package);

    // Lexical classes: keywords, comments, numbers, macros, include files.
    CHECK(at(*design, 28, 1).cls == TokenClass::Keyword);
    t = at(*design, 29, 20);
    CHECK(t.cls == TokenClass::Comment);
    CHECK_EQ(t.length, 15u);
    CHECK(at(*design, 31, 3).cls == TokenClass::Comment);
    t = at(*design, 32, 1);
    CHECK(t.cls == TokenClass::Comment);
    CHECK_EQ(t.length, 15u);
    CHECK(at(*design, 33, 13).cls == TokenClass::Number);
    t = at(*design, 36, 54);  // `LIMIT usage
    CHECK(t.cls == TokenClass::Macro);
    CHECK_EQ(t.length, 6u);
    CHECK(at(*design, 6, 16).cls == TokenClass::Macro);
    CHECK(at(*design, 1, 1).cls == TokenClass::Macro);
    CHECK(at(*design, 1, 10).cls == TokenClass::String);
    CHECK(at(*defs, 1, 1).cls == TokenClass::Comment);
    CHECK(at(*defs, 2, 1).cls == TokenClass::Macro);
    CHECK(at(*defs, 2, 9).cls == TokenClass::Macro);
    CHECK(at(*defs, 2, 15).cls == TokenClass::Number);

    // Per-instance uninstantiated generate blocks.
    CHECK_EQ(index.inactive.size(), size_t{2});
    const auto fast = index.inactive.find("top.u_fast");
    const auto slow = index.inactive.find("top.u_slow");
    CHECK(fast != index.inactive.end() && slow != index.inactive.end());
    if (fast != index.inactive.end() && slow != index.inactive.end()) {
        CHECK_EQ(fast->second.size(), size_t{1});
        CHECK(fast->second[0].range == (Range{22, 12, 25, 6}));
        CHECK_EQ(slow->second.size(), size_t{1});
        CHECK(slow->second[0].range == (Range{20, 13, 22, 6}));
    }
    CHECK(index.definitions.at("lane") == (Location{designIdx, 16, 8}));
    CHECK(index.definitions.at("bus_if") == (Location{designIdx, 10, 11}));
    CHECK(index.definitions.at("pkg") == (Location{designIdx, 2, 9}));

    // Serialization and in-place splicing keep the original bytes intact.
    const std::string json = toJson(index);
    const Json roundTrip = parseJson(json);
    CHECK_EQ(roundTrip.at("files").items.size(), size_t{2});
    CHECK_EQ(roundTrip.at("classes").items.size(), static_cast<size_t>(TokenClass::Count));
    const std::string once = withSourceIndex(document, json);
    CHECK_EQ(once.substr(0, document.find_last_of('}')),
             document.substr(0, document.find_last_of('}')));
    const std::string twice = withSourceIndex(once, json);
    CHECK_EQ(twice, once);
    const Json spliced = parseJson(twice);
    CHECK(spliced.at("source_index").at("files").items.size() == 2);
    CHECK_EQ(spliced.at("design_id").str(), std::string_view{"fixture"});
}

void testJson() {
    const Json value = parseJson(R"({"a":[1,2.5,-3],"b":"x\"yé","c":{"d":true,"e":null}})");
    CHECK_EQ(value.at("a").items.size(), size_t{3});
    CHECK_EQ(value.at("a").items[1].number, 2.5);
    CHECK_EQ(value.at("b").string, std::string{"x\"y\xc3\xa9"});
    CHECK(value.at("c").at("d").boolean);
    CHECK(value.at("c").at("e").kind == Json::Kind::Null);
    CHECK(value.get("missing") == nullptr);
    CHECK_EQ(quoteJson("a\"\n\\"), std::string{"\"a\\\"\\n\\\\\""});
    bool threw = false;
    try {
        parseJson("{\"a\":}");
    } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    std::vector<MemberSpan> spans;
    parseJson(R"({"x":1,"source_index":{"k":[]},"y":2})", &spans);
    CHECK_EQ(spans.size(), size_t{3});
    CHECK_EQ(spans[1].key, std::string{"source_index"});
    CHECK_EQ(withSourceIndex(R"({"x":1,"source_index":{"k":[]},"y":2})", "{}"),
             std::string{R"({"x":1,"y":2,"source_index":{}})"});
}

}  // namespace

int main() {
    testJson();
    testIndex(VDB_INDEX_FIXTURES);
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
