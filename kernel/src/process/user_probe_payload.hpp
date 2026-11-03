// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

namespace starlix::process::user_probe {

using ProbeByte = unsigned char;
using ProbeU32 = unsigned int;
using ProbeU64 = unsigned long long;
using ProbeSize = unsigned long long;


constexpr ProbeU64 USER_REGION_BASE = 0x0000008000000000ULL;
constexpr ProbeU64 USER_REGION_END = 0x0000010000000000ULL; // exclusive
constexpr ProbeU64 EXIT_MAGIC = 0x53544C5850524F42ULL; // "STLXPROB"
constexpr ProbeU32 COUNTER_THRESHOLD = 0x00002000u;
constexpr ProbeSize PAYLOAD_BYTES = 57u;
constexpr ProbeSize OWN_COUNTER_IMMEDIATE_OFFSET = 2u;
constexpr ProbeSize PEER_COUNTER_IMMEDIATE_OFFSET = 12u;
constexpr ProbeSize OWN_THRESHOLD_IMMEDIATE_OFFSET = 28u;
constexpr ProbeSize PEER_THRESHOLD_IMMEDIATE_OFFSET = 37u;
constexpr ProbeSize EXIT_MAGIC_IMMEDIATE_OFFSET = 45u;
constexpr ProbeSize EXIT_RIP_OFFSET = 55u;

/*
 * Emits a tiny GPR-only program:
 *   own_counter++ until own and peer counters reach THRESHOLD;
 *   RAX=EXIT_MAGIC; INT 0x80.
 * All memory references are to explicitly mapped user pages. No syscall,
 * privileged instruction, kernel address, stack call, or external relocation.
 */
inline bool build_payload(ProbeByte* output, ProbeSize capacity,
                          ProbeU64 own_counter, ProbeU64 peer_counter) {
    if (!output || capacity < PAYLOAD_BYTES || own_counter == 0ULL ||
        peer_counter == 0ULL || (own_counter & 7ULL) != 0ULL ||
        (peer_counter & 7ULL) != 0ULL ||
        own_counter < USER_REGION_BASE || own_counter >= USER_REGION_END ||
        peer_counter < USER_REGION_BASE || peer_counter >= USER_REGION_END)
        return false;

    static constexpr ProbeByte code[PAYLOAD_BYTES] = {
        0x48, 0xBB, 0,0,0,0,0,0,0,0,       // movabs rbx, own
        0x48, 0xB9, 0,0,0,0,0,0,0,0,       // movabs rcx, peer
        0x48, 0xFF, 0x03,                   // loop: inc qword [rbx]
        0xF3, 0x90,                         // pause
        0x48, 0x81, 0x3B, 0,0,0,0,         // cmp qword [rbx], threshold
        0x72, 0xF2,                         // jb loop
        0x48, 0x81, 0x39, 0,0,0,0,         // cmp qword [rcx], threshold
        0x72, 0xE9,                         // jb loop
        0x48, 0xB8, 0,0,0,0,0,0,0,0,       // movabs rax, EXIT_MAGIC
        0xCD, 0x80,                         // int 0x80 (test-only DPL3 gate)
        0xEB, 0xFE                          // fail-safe if gate returns
    };

    for (ProbeSize i = 0; i < PAYLOAD_BYTES; ++i) output[i] = code[i];
    const auto patch64 = [&](ProbeSize offset, ProbeU64 value) {
        for (ProbeU32 i = 0; i < 8u; ++i)
            output[offset + i] = static_cast<ProbeByte>(value >> (i * 8u));
    };
    const auto patch32 = [&](ProbeSize offset, ProbeU32 value) {
        for (ProbeU32 i = 0; i < 4u; ++i)
            output[offset + i] = static_cast<ProbeByte>(value >> (i * 8u));
    };
    patch64(OWN_COUNTER_IMMEDIATE_OFFSET, own_counter);
    patch64(PEER_COUNTER_IMMEDIATE_OFFSET, peer_counter);
    patch32(OWN_THRESHOLD_IMMEDIATE_OFFSET, COUNTER_THRESHOLD);
    patch32(PEER_THRESHOLD_IMMEDIATE_OFFSET, COUNTER_THRESHOLD);
    patch64(EXIT_MAGIC_IMMEDIATE_OFFSET, EXIT_MAGIC);
    return true;
}

} // namespace starlix::process::user_probe
