#include "../kernel/src/loader/ect_format.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using starlix::loader::ect::LoadPlan;
using starlix::loader::ect::validate_image;

void put32(std::vector<unsigned char>& b, size_t off, unsigned value) {
    for (unsigned i = 0; i < 4; ++i)
        b[off + i] = static_cast<unsigned char>((value >> (i * 8)) & 0xFFu);
}
unsigned passed = 0;
unsigned failed = 0;

void check(const char* name, const std::vector<unsigned char>& image,
           bool expected) {
    LoadPlan plan{};
    const bool actual = validate_image(image.data(), image.size(), plan);
    if (actual == expected) {
        ++passed;
    } else {
        ++failed;
        std::cerr << "FAIL: " << name << " expected=" << expected
                  << " actual=" << actual << "\n";
    }
    if (!actual && (plan.image_base != 0ULL || plan.entry_point != 0ULL ||
                    plan.section_count != 0u || plan.image_end != 0ULL)) {
        ++failed;
        std::cerr << "FAIL: " << name << " left a partial load plan\n";
    }
}

std::vector<unsigned char> mutate(
    const std::vector<unsigned char>& original,
    size_t offset, unsigned char value) {
    auto copy = original;
    if (offset < copy.size()) copy[offset] = value;
    return copy;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: ect_loader_phase2j_selftest <stlcs.ect>\n";
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "cannot open ECT input\n";
        return 2;
    }
    std::vector<unsigned char> valid((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());

    check("valid stlcs ECT", valid, true);
    {
        LoadPlan plan{};
        const bool ok = validate_image(valid.data(), valid.size(), plan);
        if (ok && plan.section_count == 2u &&
            plan.image_base == 0x0000008000100000ULL &&
            plan.entry_point >= plan.sections[0].virtual_address &&
            plan.entry_point < plan.sections[0].virtual_address +
                plan.sections[0].byte_size &&
            (plan.sections[0].flags & 0x4u) != 0u &&
            (plan.sections[0].flags & 0x2u) == 0u &&
            plan.sections[0].virtual_address == 0x0000008000101000ULL) {
            ++passed;
        } else {
            ++failed;
            std::cerr << "FAIL: validated load plan values\n";
        }
    }
    check("null/short image", std::vector<unsigned char>(127u, 0u), false);
    check("bad magic", mutate(valid, 0u, 'Q'), false);
    check("bad banner", mutate(valid, 2u, 'X'), false);
    check("bad format string", mutate(valid, 34u, 'X'), false);
    check("unsupported major", mutate(valid, 74u, 2u), false);
    check("unsupported minor", mutate(valid, 76u, 1u), false);
    check("unsupported image type", mutate(valid, 78u, 2u), false);
    check("unsupported architecture", mutate(valid, 79u, 2u), false);
    check("bad header size", mutate(valid, 80u, 0u), false);
    check("bad section table offset", mutate(valid, 86u, 0u), false);
    check("imports are rejected", mutate(valid, 90u, 1u), false);
    check("relocations are rejected", mutate(valid, 98u, 1u), false);
    check("wrong image base", mutate(valid, 114u, 0x01u), false);
    check("reserved header bits", mutate(valid, 126u, 1u), false);
    check("text checksum mismatch", mutate(valid, 180u, valid[180] ^ 0x40u), false);
    check("entry outside text", mutate(valid, 107u, 0x00u), false);
    check("section table count too high", mutate(valid, 82u, 0xFFu), false);
    check("unknown section name", mutate(valid, 128u, 'X'), false);
    check("raw section overlaps header", mutate(valid, 144u, 0u), false);
    {
        auto image = valid;
        put32(image, 144u, 0xFFFFFFFFu);
        check("raw range outside image", image, false);
    }
    check("unaligned virtual section", mutate(valid, 136u, 1u), false);
    check("text W+X rejected", mutate(valid, 172u, 7u), false);
    check("text missing read flag", mutate(valid, 172u, 4u), false);
    check("section size zero", mutate(valid, 140u, 0u), false);

    {
        auto image = valid;
        put32(image, 128u + 24u + 8u, 0x1000u); // .rodata overlaps .text
        check("overlapping virtual sections", image, false);
    }
    {
        auto image = valid;
        put32(image, 128u + 24u + 16u, 176u); // raw overlap with .text
        check("overlapping raw sections", image, false);
    }
    {
        auto image = valid;
        put32(image, 128u + 24u + 16u, 3098u);
        image.push_back(0u);
        check("unreferenced payload gap", image, false);
    }
    {
        auto image = valid;
        image.push_back(0u);
        check("trailing bytes rejected", image, false);
    }
    {
        auto image = valid;
        put32(image, 128u + 20u, 3u); // .text writable, non-executable
        check("text must be executable", image, false);
    }
    {
        auto image = valid;
        put32(image, 128u + 8u, 0x04000000u);
        check("image span limit", image, false);
    }

    std::cout << "ECT Phase 2J parser self-test: " << passed
              << " passed, " << failed << " failed\n";
    return failed == 0u ? 0 : 1;
}
