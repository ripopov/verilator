// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: verilator_vdb_index command line
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

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int usage(std::ostream& out) {
    out << "usage: verilator_vdb_index [--dump] [--verbose] <design.vdb.json>\n"
           "Elaborates the design described by the VDB's elaboration record with slang\n"
           "and adds its `source_index` member to the file in place.\n"
           "  --dump     write the source_index JSON to stdout instead of the file\n"
           "  --verbose  report the number of files, tokens and tolerated errors\n";
    return 2;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) throw std::runtime_error{"cannot read " + path.string()};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

int main(int argc, char** argv) {
    bool dump = false;
    bool verbose = false;
    std::string file;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump") {
            dump = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "verilator_vdb_index: unknown option " << arg << '\n';
            return usage(std::cerr);
        } else if (file.empty()) {
            file = arg;
        } else {
            return usage(std::cerr);
        }
    }
    if (file.empty()) return usage(std::cerr);
    try {
        const std::filesystem::path path = std::filesystem::absolute(file);
        const std::string document = readFile(path);
        const vdb_index::Json parsed = vdb_index::parseJson(document);
        const vdb_index::IndexRequest request
            = vdb_index::requestFromVdb(parsed, path.parent_path().string());
        const vdb_index::SourceIndex index = vdb_index::buildIndex(request);
        const std::string json = vdb_index::toJson(index);
        if (verbose) {
            size_t tokens = 0;
            for (const auto& f : index.files) tokens += f.tokens.size();
            std::cerr << "verilator_vdb_index: " << index.files.size() << " files, " << tokens
                      << " tokens, " << index.inactive.size()
                      << " instances with uninstantiated generate blocks, " << index.errors
                      << " slang errors tolerated (" << index.producer << ")\n";
        }
        if (dump) {
            std::cout << json << '\n';
            return 0;
        }
        const std::string updated = vdb_index::withSourceIndex(document, json);
        const std::filesystem::path temp = path.string() + ".tmp";
        {
            std::ofstream out{temp, std::ios::binary};
            if (!out) throw std::runtime_error{"cannot write " + temp.string()};
            out << updated << '\n';
        }
        std::filesystem::rename(temp, path);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "verilator_vdb_index: " << e.what() << '\n';
        return 1;
    }
}
