// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include "verilated.h"
#include "verilated_vtr_c.h"

#include <fstream>
#include <iterator>
#include <string>
#include <zlib.h>

struct Document final {
    std::string bytes;
    uint64_t decodedBytes;
};

void initialize(void* opaque, VerilatedVtr* tracep, uint32_t) {
    const Document& document = *static_cast<Document*>(opaque);
    tracep->declVdb("0123456789abcdef", document.bytes.data(), document.bytes.size(),
                    document.decodedBytes, "TOP");
}

int main() {
    VerilatedContext context;
    context.threads(1);
    for (const auto mode : {vtr::ExecutionMode::inline_, vtr::ExecutionMode::threaded}) {
        vtr::WriterOptions options;
        options.mode = mode;
        Document document;
        VerilatedVtrC trace{options};
        trace.spTrace()->addInitCb(initialize, &document, "TOP", false, 0);
        for (const size_t size : {65535U, 65536U, 65537U, 131072U}) {
            const std::string original = "{\"payload\":\"" + std::string(size - 14, 'x') + "\"}";
            if (original.size() != size) vl_fatal(__FILE__, __LINE__, "", "Fixture size mismatch");
            uLongf compressed = compressBound(original.size());
            document.bytes.resize(compressed);
            if (compress2(reinterpret_cast<Bytef*>(&document.bytes[0]), &compressed,
                          reinterpret_cast<const Bytef*>(original.data()), original.size(),
                          Z_DEFAULT_COMPRESSION) != Z_OK)
                vl_fatal(__FILE__, __LINE__, "", "Fixture compression failed");
            document.bytes.resize(compressed);
            document.decodedBytes = size;
            const std::string path = VL_STRINGIFY(TEST_OBJ_DIR) "/boundary.vtr";
            trace.open(path.c_str());
            trace.close();
            std::ifstream file{VL_STRINGIFY(TEST_OBJ_DIR) "/boundary.vdb.json", std::ios::binary};
            const std::string actual{std::istreambuf_iterator<char>{file},
                                     std::istreambuf_iterator<char>{}};
            const std::string expected = original.substr(0, original.size() - 1)
                                         + ",\"trace_binding\":{\"prefix\":\"TOP\",\"signals\":{}}}\n";
            if (actual != expected)
                vl_fatal(__FILE__, __LINE__, "", "VDB streaming extraction changed document");
        }
    }
    std::puts("*-* All Finished *-*");
}
