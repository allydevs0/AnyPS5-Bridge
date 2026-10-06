#include <codegen/x86/BmiTypes.hpp>
#include <codegen/x86/IBmiStubEmitter.hpp>
#include <codegen/CodegenException.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace Codegen;

namespace {

struct FixtureCase {
    BmiOp Op;
    bool Wide;
    bool IsMemory;
    std::uint8_t Dst;
    std::uint8_t Src;
    std::uint8_t Third;
    std::uint8_t Imm8;
    BmiMemOperand Mem;
    std::vector<std::uint8_t> Blob;
};

std::uint32_t readU32(const std::uint8_t*& p) {
    std::uint32_t v;
    v = static_cast<std::uint32_t>(p[0])
      | (static_cast<std::uint32_t>(p[1]) << 8)
      | (static_cast<std::uint32_t>(p[2]) << 16)
      | (static_cast<std::uint32_t>(p[3]) << 24);
    p += 4;
    return v;
}

std::uint8_t readU8(const std::uint8_t*& p) {
    return *p++;
}

std::int8_t readI8(const std::uint8_t*& p) {
    std::int8_t v;
    std::memcpy(&v, p, 1);
    ++p;
    return v;
}

std::int32_t readI32(const std::uint8_t*& p) {
    return static_cast<std::int32_t>(readU32(p));
}

bool loadFixture(const std::string& path, std::vector<FixtureCase>& cases) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::uint8_t* p = raw.data();
    const std::uint8_t* end = raw.data() + raw.size();
    const std::uint32_t count = readU32(p);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (p + 7 > end) {
            return false;
        }
        FixtureCase c;
        c.Op = static_cast<BmiOp>(readU8(p));
        c.Wide = readU8(p) != 0;
        c.IsMemory = readU8(p) != 0;
        c.Dst = readU8(p);
        c.Src = readU8(p);      // 0xFF when memory form
        c.Third = readU8(p);    // 0xFF when none
        c.Imm8 = readU8(p);
        if (c.IsMemory) {
            c.Mem.Mod = readU8(p);
            c.Mem.HasSib = readU8(p) != 0;
            c.Mem.Base = readI8(p);
            c.Mem.Index = readI8(p);
            c.Mem.IndexCode = readI8(p);
            c.Mem.BaseCode = readI8(p);
            c.Mem.Scale = readU8(p);
            c.Mem.Disp = readI32(p);
            c.Src = 0;
        }
        const std::uint32_t blobLen = readU32(p);
        if (p + blobLen > end) {
            return false;
        }
        c.Blob.assign(p, p + blobLen);
        p += blobLen;
        cases.push_back(std::move(c));
    }
    return true;
}

std::string opName(BmiOp op) {
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

} // namespace

int main(int argc, char** argv) {
    const std::string path = (argc >= 2) ? argv[1] : R"(C:\Users\denise\AppData\Local\Temp\opencode\bmi_blobs.bin)";

    std::vector<FixtureCase> cases;
    if (!loadFixture(path, cases)) {
        std::fprintf(stderr, "failed to load fixture: %s\n", path.c_str());
        return 1;
    }

    std::unique_ptr<IBmiStubEmitter> emitter = MakeBmiStubEmitter();
    std::size_t failures = 0;

    for (std::size_t i = 0; i < cases.size(); ++i) {
        const FixtureCase& c = cases[i];
        BmiMatch m;
        m.Op = c.Op;
        m.Length = 5;
        m.Wide = c.Wide;
        m.Dst = c.Dst;
        m.Src = c.Src;
        m.Third = c.Third;
        m.Imm8 = c.Imm8;
        m.IsMemory = c.IsMemory;
        if (c.IsMemory) {
            m.Mem = c.Mem;
        }

        const std::vector<std::uint8_t> expected = c.Blob;
        std::vector<std::uint8_t> actual;
        try {
            actual = emitter->EmitTemplate(m);
        } catch (const CodegenException& e) {
            std::fprintf(stderr, "[%zu] %s w%d %s: threw: %s\n",
                         i, opName(c.Op).c_str(), c.Wide ? 64 : 32,
                         c.IsMemory ? "mem" : "reg", e.what());
            ++failures;
            continue;
        }

        if (actual.size() != expected.size() || actual != expected) {
            std::fprintf(stderr, "[%zu] MISMATCH %s w%d %s dst=r%d src=r%d third=r%d imm=%d\n",
                         i, opName(c.Op).c_str(), c.Wide ? 64 : 32,
                         c.IsMemory ? "mem" : "reg", c.Dst, c.Src, c.Third, c.Imm8);
            std::fprintf(stderr, "  expected %zu bytes: ", expected.size());
            for (std::uint8_t b : expected) std::fprintf(stderr, "%02x ", b);
            std::fprintf(stderr, "\n  actual   %zu bytes: ", actual.size());
            for (std::uint8_t b : actual) std::fprintf(stderr, "%02x ", b);
            std::fprintf(stderr, "\n");
            ++failures;
        }

        std::size_t rel32Offset = 0;
        const std::vector<std::uint8_t> dispatch = emitter->EmitDispatch(m, rel32Offset);
        // Dispatch = template, then E9 rel32 (opcode at index template.size(),
        // displacement field spanning [template.size()+1, template.size()+5)).
        if (dispatch.size() != expected.size() + 5
            || rel32Offset != expected.size() + 1
            || dispatch[rel32Offset - 1] != 0xE9) {
            std::fprintf(stderr, "[%zu] dispatch layout wrong for %s\n", i, opName(c.Op).c_str());
            ++failures;
        }

        const std::vector<std::uint8_t> shortStub = emitter->EmitShort(m);
        if (shortStub.size() != expected.size() + 1 || shortStub.back() != 0xC3) {
            std::fprintf(stderr, "[%zu] short layout wrong for %s\n", i, opName(c.Op).c_str());
            ++failures;
        }
    }

    if (failures != 0) {
        std::fprintf(stderr, "%zu/%zu cases failed\n", failures, cases.size());
        return 1;
    }
    std::printf("BmiStubEmitter parity: %zu/%zu cases OK\n", cases.size(), cases.size());
    return 0;
}