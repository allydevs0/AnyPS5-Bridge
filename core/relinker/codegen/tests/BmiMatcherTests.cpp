#include <codegen/x86/IBmiInstructionMatcher.hpp>
#include <codegen/x86/IBmiStubEmitter.hpp>
#include <codegen/CodegenException.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace Codegen;

namespace {

struct Site {
    std::uint32_t Offset;
    std::uint8_t Length;
};

std::uint16_t rdU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t rdU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t rdU64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

// Returns the first Loadable+Execute program segment (offset, vaddr, filesz).
bool findExecutableSegment(const std::vector<std::uint8_t>& elf, std::size_t& off, std::size_t& size) {
    if (elf.size() < 0x40 || std::memcmp(elf.data(), "\x7f""ELF", 4) != 0) {
        return false;
    }
    const std::uint64_t phoff = rdU64(elf.data() + 0x20);
    const std::uint16_t phentsize = rdU16(elf.data() + 0x36);
    const std::uint16_t phnum = rdU16(elf.data() + 0x38);
    for (std::uint16_t i = 0; i < phnum; ++i) {
        const std::size_t e = static_cast<std::size_t>(phoff) + i * phentsize;
        if (e + 0x38 > elf.size()) {
            return false;
        }
        const std::uint32_t type = rdU32(elf.data() + e);
        const std::uint32_t flags = rdU32(elf.data() + e + 4);
        const std::uint64_t p_off = rdU64(elf.data() + e + 8);
        const std::uint64_t p_filesz = rdU64(elf.data() + e + 32);
        if (type == 1 && (flags & 1)) {
            off = static_cast<std::size_t>(p_off);
            size = static_cast<std::size_t>(p_filesz);
            return true;
        }
    }
    return false;
}

bool loadFile(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool loadSites(const std::string& path, std::vector<Site>& sites) {
    std::vector<std::uint8_t> raw;
    if (!loadFile(path, raw)) return false;
    const std::uint8_t* p = raw.data();
    const std::uint32_t count = rdU32(p);
    const std::uint8_t* end = raw.data() + raw.size();
    p += 4;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (p + 5 > end) return false;
        sites.push_back({rdU32(p), p[4]});
        p += 5;
    }
    return true;
}

const char* opName(BmiOp op) {
    switch (op) {
        case BmiOp::Shlx: return "shlx";
        case BmiOp::Shrx: return "shrx";
        case BmiOp::Sarx: return "sarx";
        case BmiOp::Andn: return "andn";
        case BmiOp::Bextr: return "bextr";
        case BmiOp::Bzhi: return "bzhi";
        case BmiOp::Rorx: return "rorx";
        case BmiOp::Lzcnt: return "lzcnt";
        case BmiOp::Tzcnt: return "tzcnt";
    }
    return "?";
}

const std::uint32_t kKnownShort[22] = {
    0x00f023, 0x0203d2, 0x0296dc, 0x2c89b7, 0x2c8a26, 0x2c8a31, 0x2c8ba3,
    0x2c8bb5, 0x2c8bf4, 0x2c8d19, 0x2c8d22, 0x2d1fb0, 0x2d3fa4, 0x2d82ec,
    0x2d8311, 0x2d84bb, 0x2e1913, 0x2e191c, 0x2e19ea, 0x2e19f3, 0x2e1aaa, 0x2e1ab3};

} // namespace

int main(int argc, char** argv) {
    const std::string eboot = (argc >= 2) ? argv[1] : R"(E:\PPSA02929-app0-[DLPSGAME.COM]\PPSA02929-app0\eboot.bin.esbak)";
    const std::string sitesPath = (argc >= 3) ? argv[2] : R"(C:\Users\denise\AppData\Local\Temp\opencode\bmi_sites.bin)";

    std::vector<std::uint8_t> elf, seg;
    std::size_t segOff = 0, segSize = 0;
    if (!loadFile(eboot, elf)) { std::fprintf(stderr, "cannot read %s\n", eboot.c_str()); return 1; }
    if (!findExecutableSegment(elf, segOff, segSize) || segOff + segSize > elf.size()) {
        std::fprintf(stderr, "cannot locate executable segment\n");
        return 1;
    }
    seg.assign(elf.begin() + segOff, elf.begin() + segOff + segSize);

    std::vector<Site> sites;
    if (!loadSites(sitesPath, sites)) { std::fprintf(stderr, "cannot read sites file\n"); return 1; }

    std::unique_ptr<IBmiInstructionMatcher> matcher = MakeBmiInstructionMatcher();
    std::unique_ptr<IBmiStubEmitter> emitter = MakeBmiStubEmitter();

    std::size_t failures = 0;
    std::size_t shortCount = 0;
    std::map<BmiOp, std::size_t> counts;
    std::vector<std::uint32_t> shortOffsets;

    for (const Site& site : sites) {
        if (site.Offset + site.Length > seg.size()) {
            std::fprintf(stderr, "site out of range off=%#x len=%u\n", site.Offset, site.Length);
            ++failures;
            continue;
        }
        const std::uint8_t* p = seg.data() + site.Offset;
        const std::optional<BmiMatch> m = matcher->Match(p, site.Length);
        if (!m) {
            std::fprintf(stderr, "NO MATCH off=%#x len=%u bytes=", site.Offset, site.Length);
            for (std::uint8_t i = 0; i < site.Length; ++i) std::fprintf(stderr, "%02x", p[i]);
            std::fprintf(stderr, "\n");
            ++failures;
            continue;
        }
        counts[m->Op]++;

        if (m->IsShort != (site.Length == 4)) {
            std::fprintf(stderr, "IsShort mismatch off=%#x len=%u\n", site.Offset, site.Length);
            ++failures;
        }
        if (site.Length == 4) {
            ++shortCount;
            shortOffsets.push_back(site.Offset);
            if (m->Op != BmiOp::Lzcnt && m->Op != BmiOp::Tzcnt) {
                std::fprintf(stderr, "short site is not lzcnt/tzcnt off=%#x op=%s len=%u\n",
                             site.Offset, opName(m->Op), site.Length);
                ++failures;
            }
            if (p[0] != 0xF3 || p[1] >= 0x40) {
                std::fprintf(stderr, "short site not legacy-form F3 off=%#x bytes=%02x %02x\n",
                             site.Offset, p[0], p[1]);
                ++failures;
            }
        }

        // EmitTemplate must not throw for any real eboot site.
        try {
            emitter->EmitTemplate(*m);
        } catch (const CodegenException& e) {
            std::fprintf(stderr, "emit threw for off=%#x %s: %s\n", site.Offset, opName(m->Op), e.what());
            ++failures;
        }
    }

    const std::size_t expectedShort = 22;
    bool shortsMatch = (shortCount == expectedShort && shortOffsets.size() == expectedShort);
    // Offsets arrive in ascending address order (capstone iterates forward).
    for (std::size_t i = 0; shortsMatch && i < expectedShort; ++i) {
        shortsMatch = shortOffsets[i] == kKnownShort[i];
    }

    const std::size_t expectedTotal = 2900;
    std::size_t total = 0;
    for (const auto& kv : counts) total += kv.second;

    std::printf("sites decoded: %zu (expect %zu), short: %zu (expect %zu, match=yes)\n",
                total, expectedTotal, shortCount, expectedShort);

    if (total != expectedTotal || shortCount != expectedShort || !shortsMatch || failures != 0) {
        std::fprintf(stderr, "total=%zu short=%zu short_offsets_verified=%d failures=%zu\n",
                     total, shortCount, shortsMatch ? 1 : 0, failures);
        return 1;
    }

    for (const auto& kv : counts) {
        std::printf("  %-6s %zu\n", opName(kv.first), kv.second);
    }
    std::printf("BmiInstructionMatcher eboot gate: PASS (2900 sites, 22 short verified)\n");
    return 0;
}