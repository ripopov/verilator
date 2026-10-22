// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include "verilated.h"
#include "verilated_vtr_c.h"
#include "vtr_reader.h"

#include VM_PREFIX_INCLUDE

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>

static void check(bool value, const char* message) {
    if (!value) vl_fatal(__FILE__, __LINE__, "", message);
}

struct CallbackProbe;
struct CallbackLane final {
    CallbackProbe* ownerp;
    uint32_t index;
    uint32_t code = 0;
};

struct CallbackProbe final {
    std::array<CallbackLane, 2> lanes{{{this, 0}, {this, 1}}};
    std::atomic<uint32_t> arrivals{0};
    uint64_t time = 0;
    bool parallel = false;
};

static void initializeProbe(void* opaque, VerilatedVtr* tracep, uint32_t baseCode) {
    auto& probe = *static_cast<CallbackProbe*>(opaque);
    tracep->pushPrefix("probe", VerilatedTracePrefixType::SCOPE_MODULE);
    for (auto& lane : probe.lanes) {
        lane.code = baseCode + 2 * lane.index;
        const std::string name = "callback_probe_" + std::to_string(lane.index);
        tracep->declQuad(lane.code, name.c_str(), -1, VerilatedTraceSigDirection::NONE,
                        VerilatedTraceSigKind::VAR, VerilatedTraceSigType::LOGIC, 63, 0);
    }
    tracep->popPrefix();
}

static void dumpProbe(void* opaque, VerilatedVtr::Buffer* bufferp) {
    auto& lane = *static_cast<CallbackLane*>(opaque);
    auto& probe = *lane.ownerp;
    probe.arrivals.fetch_or(1U << lane.index);
    if (probe.parallel) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (probe.arrivals.load() != 3) {
            check(std::chrono::steady_clock::now() < deadline,
                  "Trace callbacks did not execute concurrently");
            std::this_thread::yield();
        }
    }
    bufferp->fullQData(bufferp->oldp(lane.code), (probe.time << 32) | (lane.index + 1), 64);
}

static void run(vtr::ExecutionMode mode) {
    VerilatedContext context;
    context.threads(2);
    context.traceEverOn(true);
    VM_PREFIX model{&context, "TOP"};
    VerilatedVtrC trace{{.mode = mode}};
    model.trace(&trace, 99);
    CallbackProbe probe;
    trace.spTrace()->addInitCb(initializeProbe, &probe, "probe", false, 4);
    for (auto& lane : probe.lanes) {
        trace.spTrace()->addFullCb(dumpProbe, lane.index, &lane);
        trace.spTrace()->addChgCb(dumpProbe, lane.index, &lane);
    }
    probe.parallel = mode == vtr::ExecutionMode::threaded && model.threads() > 1;
    const std::string path = VL_STRINGIFY(TEST_OBJ_DIR) "/dpi-"
        + std::string{mode == vtr::ExecutionMode::inline_ ? "inline" : "threaded"} + ".vtr";
    trace.open(path.c_str());
    for (unsigned time = 0; time < 128; ++time) {
        probe.time = time;
        probe.arrivals.store(0);
        model.clk = time & 1;
        model.eval();
        trace.dump(context.time());
        context.timeInc(1);
    }
    model.final();
    trace.close();
    const auto stats = trace.stats();
    check(stats.backend_cpu_nanoseconds.has_value(), "Backend CPU counter unavailable");
    if (mode == vtr::ExecutionMode::inline_) {
        check(!stats.transport.submitted && !stats.encoding.submitted && !*stats.backend_cpu_nanoseconds,
              "Inline tracing used a queue or worker");
    } else {
        check(stats.transport.submitted && stats.transport.submitted == stats.transport.completed
                  && *stats.backend_cpu_nanoseconds,
              "Threaded tracing did not drain operations or account for backend CPU");
    }
    vtr::Reader reader{path};
    check(reader.validate().missing.empty(), "DPI trace validation failed");
    for (uint32_t lane = 0; lane < 2; ++lane) {
        const auto signal = reader.find_signal("probe.callback_probe_" + std::to_string(lane) + " [63:0]");
        check(signal.has_value(), "Callback waveform declaration missing");
        uint64_t count = 0;
        reader.changes(*signal, {}, [&](const auto& change) {
            check(change.time == count && change.value.as_u64() == (count << 32 | (lane + 1)),
                  "Callback waveform order, owner or value changed");
            ++count;
            return vtr::Flow::next;
        });
        check(count == 128, "Callback waveform timestamp missing");
    }
    const auto instructions = reader.find_generator("TOP.t.instructions");
    const auto requests = reader.find_generator("TOP.t.requests");
    const auto schedule = reader.find_generator("TOP.t.schedule");
    check(instructions && requests && schedule, "DPI declarations missing");
    std::map<uint64_t, vtr::RecordId> ids;
    unsigned owners = 0, children = 0, logs = 0;
    reader.transactions({}, [&](const auto& record) {
        if (record.generator() == instructions) {
            ++owners;
            uint64_t sequence = UINT64_MAX;
            for (const auto& attr : record.attributes())
                if (reader.string(attr.key) == "pc")
                    sequence = (std::get<uint64_t>(attr.data.data) - 0x80000000) / 4;
            check(sequence < 64, "DPI instruction attribute missing");
            ids.emplace(sequence, record.id());
            check(record.begin() == 2 * sequence + 1
                      && record.end() == (sequence < 62 ? 2 * sequence + 5 : 128)
                      && record.status() == (sequence < 62 ? vtr::Status::ok : vtr::Status::aborted),
                  "DPI instruction lifetime differs from RTL");
            check(record.stages().size() == (sequence < 63 ? 2U : 1U)
                      && record.events().size() == (sequence < 63 ? 1U : 0U),
                  "DPI stage or event missing");
            if (sequence < 63)
                check(record.events()[0].time == 2 * sequence + 3,
                      "DPI event timestamp differs from RTL");
        }
        return vtr::Flow::next;
    });
    reader.transactions({}, [&](const auto& record) {
        if (record.generator() == requests) {
            ++children;
            const auto sequence = (record.begin() - 1) / 2;
            check(!(sequence & 1) && record.parent() == ids.at(sequence)
                      && *record.parent() < record.id(), "DPI parent identity differs from RTL");
            unsigned relations = 0;
            reader.relations(record.id(), vtr::Direction::incoming, [&](const auto& relation) {
                ++relations;
                check(relation.from == ids.at(sequence) && reader.string(relation.kind) == "load",
                      "DPI relation differs from RTL");
                return vtr::Flow::next;
            });
            check(relations == 1, "DPI relation missing");
        }
        return vtr::Flow::next;
    });
    reader.records({.kind = vtr::RecordKind::log}, [&](const auto& record) {
        if (reader.format_log(record).find("DPI complete 64") != std::string::npos) ++logs;
        return vtr::Flow::next;
    });
    check(owners == 64 && children == 32 && logs == 1 && reader.cycle_at(*schedule, 128) == 63,
          "DPI finalization, clocks or simulator logs differ from RTL");
}

int main() {
    run(vtr::ExecutionMode::inline_);
    run(vtr::ExecutionMode::threaded);
    std::puts("*-* All Finished *-*");
}
