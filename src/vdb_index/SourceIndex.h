// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Static source index of an elaborated design
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
//
// The source index is the section of a VDB companion that lets a waveform
// viewer render design sources without a language frontend of its own: every
// token of every elaborated file with a class, modifiers and, for identifiers,
// the declaration it denotes, plus the generate blocks each instance leaves
// uninstantiated. It is produced once, at verilation time, by elaborating the
// same file set with slang.
//
//*************************************************************************

#ifndef VERILATOR_VDB_INDEX_SOURCEINDEX_H_
#define VERILATOR_VDB_INDEX_SOURCEINDEX_H_

#include "Json.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vdb_index {

// Token classes, serialized by name in the `classes` legend.
enum class TokenClass : uint32_t {
    Keyword,
    Comment,
    Number,
    String,
    Operator,
    Macro,
    Variable,
    Parameter,
    EnumMember,
    Type,
    Module,
    Interface,
    Package,
    Instance,
    Function,
    Property,
    Namespace,
    Count
};

// Modifier bits, serialized by name in the `modifiers` legend, in bit order.
enum class Modifier : uint32_t {
    Declaration = 1u << 0,
    Input = 1u << 1,
    Output = 1u << 2,
    InOut = 1u << 3,
    Ref = 1u << 4,
    Clock = 1u << 5,
    Readonly = 1u << 6,
    DefaultLibrary = 1u << 7,
    Argument = 1u << 8,
};

std::string_view toString(TokenClass cls);
const std::vector<std::string_view>& classNames();
const std::vector<std::string_view>& modifierNames();

// A position in an indexed file: index into SourceIndex::files, 1-based line
// and 1-based byte column. file < 0 means "not in an indexed file".
struct Location {
    int32_t file = -1;
    uint32_t line = 0;
    uint32_t column = 0;
    bool valid() const { return file >= 0; }
    bool operator==(const Location&) const = default;
};

struct Token {
    uint32_t line;  // 1-based
    uint32_t column;  // 1-based byte offset in the line
    uint32_t length;  // bytes; tokens never span lines
    TokenClass cls;
    uint32_t modifiers;
    Location declaration;  // for identifiers that resolved to a symbol
    bool operator==(const Token&) const = default;
};

// [start, end) in 1-based line/column coordinates.
struct Range {
    uint32_t startLine;
    uint32_t startColumn;
    uint32_t endLine;
    uint32_t endColumn;
    bool operator==(const Range&) const = default;
};

struct InactiveRange {
    int32_t file;
    Range range;
};

struct FileIndex {
    std::string path;  // as listed in the VDB `sources` when known, else as loaded
    std::vector<Token> tokens;  // sorted by line then column
};

// The elaboration record of a VDB: the same inputs the simulator elaborated.
struct Elaboration {
    std::string workDir;
    std::string language;
    std::vector<std::string> files;
    std::vector<std::string> libraryFiles;
    std::vector<std::string> includeDirs;
    std::vector<std::string> libraryExts;
    std::vector<std::pair<std::string, std::string>> defines;
};

struct IndexRequest {
    Elaboration elaboration;
    std::string top;
    std::vector<std::string> sources;  // VDB `sources[].path`, relative to workDir
    std::string baseDir;  // resolves a relative workDir (directory of the VDB)
};

struct SourceIndex {
    std::string producer;
    std::vector<FileIndex> files;
    // Instance path -> generate blocks that instance leaves uninstantiated.
    std::map<std::string, std::vector<InactiveRange>> inactive;
    // Module, interface, program and package name -> declaration.
    std::map<std::string, Location> definitions;
    size_t errors = 0;  // slang errors tolerated during elaboration
};

// Reads the elaboration record and top of a parsed VDB document.
// Throws std::runtime_error when the document has no elaboration record.
IndexRequest requestFromVdb(const Json& document, std::string baseDir);

// Elaborates the design with slang and classifies every token.
// Throws std::runtime_error when nothing could be parsed.
SourceIndex buildIndex(const IndexRequest& request);

// Serializes the index as the compact JSON value of the `source_index` member.
std::string toJson(const SourceIndex& index);

// Returns the document with `indexJson` as its `source_index` member, replacing
// an existing one. Every other byte of the document is preserved, so the
// design identity computed over the original text still holds.
std::string withSourceIndex(std::string_view document, std::string_view indexJson);

}  // namespace vdb_index

#endif
