// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Minimal JSON reader for VDB companion documents
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

#ifndef VERILATOR_VDB_INDEX_JSON_H_
#define VERILATOR_VDB_INDEX_JSON_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vdb_index {

// A parsed JSON value. Only what the exporter needs: member lookup, strings,
// numbers and arrays. Documents are small compared to the design itself.
struct Member;

struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Json> items;  // Array
    std::vector<Member> members;  // Object, in document order

    const Json* get(std::string_view key) const;
    // Member value or an empty Null value when absent.
    const Json& at(std::string_view key) const;
    std::string_view str() const { return kind == Kind::String ? string : std::string_view{}; }
    bool isObject() const { return kind == Kind::Object; }
    bool isArray() const { return kind == Kind::Array; }
    bool isString() const { return kind == Kind::String; }
};

struct Member {
    std::string key;
    Json value;
};

// Byte span [start, end) of one top-level member, key included.
struct MemberSpan {
    std::string key;
    size_t start;
    size_t end;
};

// Parses a complete document. Throws std::runtime_error on malformed input.
// When spans is given, it receives the byte spans of the top-level members.
Json parseJson(std::string_view text, std::vector<MemberSpan>* spans = nullptr);

// Quotes a string with JSON escapes.
std::string quoteJson(std::string_view text);

}  // namespace vdb_index

#endif
