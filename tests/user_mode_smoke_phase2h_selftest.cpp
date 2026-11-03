#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../kernel/src/process/user_probe_payload.hpp"

static unsigned passed = 0;
static unsigned failed = 0;
#define CHECK(condition, label) do { \
    if (condition) { ++passed; std::printf("[PASS] %s\n", label); } \
    else { ++failed; std::printf("[FAIL] %s\n", label); } \
} while (0)

static uint64_t read64(const unsigned char* p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(p[i]) << (8u * i);
    return value;
}
static uint32_t read32(const unsigned char* p) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(p[i]) << (8u * i);
    return value;
}

int main() {
    using namespace starlix::process::user_probe;
    constexpr uint64_t user0 = 0x0000008000101000ULL;
    constexpr uint64_t user1 = user0 + 8ULL;
    unsigned char code[4096]{};
    const bool built = build_payload(code, sizeof(code), user0, user1);
    CHECK(built, "builds a complete position-independent Ring-3 probe payload");
    CHECK(PAYLOAD_BYTES == 57u, "payload length matches the audited instruction sequence");
    CHECK(code[0] == 0x48 && code[1] == 0xBB, "payload loads its own counter address into RBX");
    CHECK(read64(code + OWN_COUNTER_IMMEDIATE_OFFSET) == user0,
          "own counter immediate is patched little-endian");
    CHECK(code[10] == 0x48 && code[11] == 0xB9,
          "payload loads the peer counter address into RCX");
    CHECK(read64(code + PEER_COUNTER_IMMEDIATE_OFFSET) == user1,
          "peer counter immediate is patched little-endian");
    CHECK(read32(code + OWN_THRESHOLD_IMMEDIATE_OFFSET) == COUNTER_THRESHOLD,
          "own-counter threshold is encoded correctly");
    CHECK(read32(code + PEER_THRESHOLD_IMMEDIATE_OFFSET) == COUNTER_THRESHOLD,
          "peer-counter threshold is encoded correctly");
    CHECK(code[32] == 0x72 && int(code[33]) - 256 + 34 == 20,
          "own-counter conditional branch targets the increment loop");
    CHECK(code[41] == 0x72 && int(code[42]) - 256 + 43 == 20,
          "peer-counter conditional branch targets the increment loop");
    CHECK(code[43] == 0x48 && code[44] == 0xB8,
          "payload loads the dedicated test-exit magic into RAX");
    CHECK(read64(code + EXIT_MAGIC_IMMEDIATE_OFFSET) == EXIT_MAGIC,
          "exit magic immediate is encoded correctly");
    CHECK(code[53] == 0xCD && code[54] == 0x80,
          "completion uses only the test-only INT 0x80 gate");
    CHECK(EXIT_RIP_OFFSET == 55u && code[55] == 0xEB && code[56] == 0xFE,
          "saved user RIP after INT 0x80 matches the exit validator");

    unsigned char reverse[4096]{};
    CHECK(build_payload(reverse, sizeof(reverse), user1, user0),
          "builds the second process payload with reversed counter ownership");
    CHECK(read64(reverse + OWN_COUNTER_IMMEDIATE_OFFSET) == user1 &&
          read64(reverse + PEER_COUNTER_IMMEDIATE_OFFSET) == user0,
          "two processes have distinct own-counter roles");
    CHECK(std::memcmp(code, reverse, PAYLOAD_BYTES) != 0,
          "process probe images are distinct while sharing only counter memory");

    unsigned char too_small[PAYLOAD_BYTES - 1u];
    std::memset(too_small, 0xA5, sizeof(too_small));
    CHECK(!build_payload(too_small, sizeof(too_small), user0, user1),
          "rejects an undersized output buffer");
    bool unchanged = true;
    for (unsigned char value : too_small) unchanged &= value == 0xA5;
    CHECK(unchanged, "undersized buffer remains untouched on rejection");
    CHECK(!build_payload(nullptr, PAYLOAD_BYTES, user0, user1),
          "rejects a null output buffer");
    CHECK(!build_payload(code, sizeof(code), 0ULL, user1),
          "rejects a zero own-counter address");
    CHECK(!build_payload(code, sizeof(code), user0, 0ULL),
          "rejects a zero peer-counter address");
    CHECK(!build_payload(code, sizeof(code), 0x1000ULL, user1),
          "rejects a kernel/low-half own-counter address");
    CHECK(!build_payload(code, sizeof(code), user0, 0x1000ULL),
          "rejects a kernel/low-half peer-counter address");
    CHECK(!build_payload(code, sizeof(code), user0 + 1ULL, user1),
          "rejects an unaligned own-counter address");
    CHECK(!build_payload(code, sizeof(code), user0, user1 + 1ULL),
          "rejects an unaligned peer-counter address");
    CHECK(!build_payload(code, sizeof(code), 0x0000010000000000ULL, user1),
          "rejects an address at the exclusive end of the user region");

    std::printf("USER_MODE_SMOKE_PHASE2H_SELFTEST: %u passed, %u failed\n",
                passed, failed);
    return failed == 0u ? 0 : 1;
}
