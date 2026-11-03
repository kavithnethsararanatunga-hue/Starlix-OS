// Starlix OS - OpenRPX xsOft - Trade secret. See project LICENSE.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include "lbr_format.hpp"
#include "../common/crc32.hpp"

using namespace starlix::lbr;

static void field(const char* label, const std::string& value) {
    std::printf("%-17s: %s\n", label, value.c_str());
}

struct LoadedLbr {
    LBR_HEADER header{};
    std::vector<EXPORT_ENTRY> exports;
    std::vector<uint8_t> code;
    std::vector<uint8_t> signature;
};

static bool load_lbr(const char* path, LoadedLbr& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(&out.header), sizeof(out.header));
    if (in.gcount() != (std::streamsize)sizeof(out.header)) return false;

    out.exports.resize(out.header.export_count);
    if (out.header.export_count > 0) {
        in.seekg(out.header.export_table_offset, std::ios::beg);
        in.read(reinterpret_cast<char*>(out.exports.data()),
                (std::streamsize)(sizeof(EXPORT_ENTRY) * out.header.export_count));
    }
    out.code.resize(out.header.code_size);
    if (out.header.code_size > 0) {
        in.seekg(out.header.code_offset, std::ios::beg);
        in.read(reinterpret_cast<char*>(out.code.data()), (std::streamsize)out.header.code_size);
    }
    if (out.header.has_signature && out.header.signature_size > 0) {
        out.signature.resize(out.header.signature_size);
        in.seekg(out.header.signature_offset, std::ios::beg);
        in.read(reinterpret_cast<char*>(out.signature.data()), (std::streamsize)out.header.signature_size);
    }
    return true;
}

static std::vector<uint8_t> integrity_blob(const LoadedLbr& l) {
    std::vector<uint8_t> blob;
    blob.insert(blob.end(),
        reinterpret_cast<const uint8_t*>(l.exports.data()),
        reinterpret_cast<const uint8_t*>(l.exports.data()) + sizeof(EXPORT_ENTRY) * l.exports.size());
    blob.insert(blob.end(), l.code.begin(), l.code.end());
    return blob;
}

static uint32_t recompute_checksum(const LoadedLbr& l) {
    auto blob = integrity_blob(l);
    return starlix::compute_crc32(blob.data(), blob.size());
}

static std::vector<uint8_t> hmac_sha256(const std::string& key, const uint8_t* data, size_t len) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    HMAC(EVP_sha256(), key.data(), (int)key.size(), data, len, digest, &digestLen);
    return std::vector<uint8_t>(digest, digest + digestLen);
}

static int cmd_inspect(const char* path) {
    LoadedLbr l;
    if (!load_lbr(path, l)) { std::fprintf(stderr, "lbr-tool: cannot read %s\n", path); return 1; }

    const char* reason = nullptr;
    bool structOk = validate_header(l.header, &reason);
    bool checksumOk = structOk && (recompute_checksum(l) == l.header.checksum);
    if (structOk && !checksumOk) reason = "checksum mismatch";
    bool valid = structOk && checksumOk;

    std::printf("STARLIX LBR BINARY INSPECTOR\n\n");
    field("File", path);
    field("Magic", std::string(l.header.magic, 2));
    field("Binary Banner", l.header.banner);
    field("Format", l.header.format_name);
    field("Version", std::to_string(l.header.version_major) + "." + std::to_string(l.header.version_minor));
    field("Architecture", l.header.architecture == ARCH_X86_64 ? "x86_64" : "Unknown");
    field("Header Size", std::to_string(l.header.header_size) + " bytes");
    field("Exports", std::to_string(l.header.export_count));
    for (auto& e : l.exports)
        std::printf("                   - %s (offset %llu)\n", e.name, (unsigned long long)e.code_offset);
    field("Imports", std::to_string(l.header.import_count));
    field("Dependencies", std::to_string(l.header.dependency_count));
    field("Relocations", std::to_string(l.header.relocation_count));
    field("Code Size", std::to_string(l.header.code_size) + " bytes");
    field("Signed", l.header.has_signature ? "YES (HMAC-SHA256)" : "NO");
    field("Validation", valid ? "PASS" : (std::string("FAIL (") + (reason ? reason : "unknown") + ")"));

    return valid ? 0 : 1;
}

static int cmd_verify(int argc, char** argv) {
    const char* path = argv[2];
    std::string key = (argc > 3) ? argv[3] : "";

    LoadedLbr l;
    if (!load_lbr(path, l)) { std::fprintf(stderr, "lbr-tool: cannot read %s\n", path); return 1; }

    const char* reason = nullptr;
    bool structOk = validate_header(l.header, &reason);
    uint32_t actual = structOk ? recompute_checksum(l) : 0;
    bool checksumOk = structOk && (actual == l.header.checksum);

    std::printf("STARLIX LBR INTEGRITY VERIFICATION\n\n");
    field("File", path);
    field("Structure", structOk ? "VALID" : (std::string("INVALID (") + (reason ? reason : "?") + ")"));

    bool sigOk = true, sigChecked = false;
    if (structOk) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08X", l.header.checksum);
        field("Stored Checksum", buf);
        std::snprintf(buf, sizeof(buf), "0x%08X", actual);
        field("Computed Checksum", buf);
        field("Checksum Match", checksumOk ? "YES" : "NO");

        if (l.header.has_signature) {
            if (!key.empty()) {
                auto blob = integrity_blob(l);
                auto expected = hmac_sha256(key, blob.data(), blob.size());
                sigChecked = true;
                sigOk = (expected.size() == l.signature.size()) &&
                        (std::memcmp(expected.data(), l.signature.data(), expected.size()) == 0);
                field("Signature", sigOk ? "VALID" : "INVALID");
            } else {
                field("Signature", "PRESENT (no key supplied, skipped)");
            }
        } else {
            field("Signature", "NONE");
        }
    }

    bool valid = structOk && checksumOk && (!sigChecked || sigOk);
    field("Overall", valid ? "PASS" : "FAIL");
    return valid ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage:\n  %s inspect <file.lbr>\n  %s verify <file.lbr> [key]\n", argv[0], argv[0]);
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "inspect") return cmd_inspect(argv[2]);
    if (cmd == "verify") return cmd_verify(argc, argv);
    std::fprintf(stderr, "Unknown command: %s\n", cmd.c_str());
    return 1;
}
