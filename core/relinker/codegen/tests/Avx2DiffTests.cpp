// Differential test for the AVX2 emulation stubs.
//
// For every case the relinker's own matcher and stub emitter produce the bytes
// that would replace the original instruction in eboot.exe. Those bytes are
// copied into RWX memory and actually executed on this CPU (which has AVX1 but
// no AVX2), and the resulting register file is compared against a scalar C
// reference computed independently of the relinker.
//
// What is checked per case:
//   * the matcher's decode and the stub's length
//   * the 256-bit (or 128-bit) result in the destination register
//   * every other ymm register, every GPR, and the stack canary
//   * the RIP-relative disp32 rebase, at the offset the emitter reported
//
// Build/run: see build/avx2diff/run_avx2diff.ps1
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <codegen/x86/Avx2Types.hpp>
#include <codegen/x86/IAvx2InstructionMatcher.hpp>
#include <codegen/x86/IAvx2StubEmitter.hpp>

struct Avx2DiffCase {
    const char* Name;
    unsigned char Op;
    unsigned char Elem;
    unsigned char Ymm;
    unsigned char Imm;
    unsigned char Form;
    unsigned char Res;
    unsigned char A;      // VEX.vvvv (also the destination for the NDD shift form)
    unsigned char B;      // ModRM.rm
    unsigned char Ndd;
    unsigned RipOff;     // byte offset inside the data page (RIP-relative form)
};

#include "avx2_cases.inc"

extern "C" {
void avx2_diff_run(const void* in, void* out, const void* code, void* scratch);
void avx2_diff_cont(void);
extern const unsigned char* avx2_case_tbl[kAvx2CaseCount];
extern unsigned char avx2diff_data[];
}


enum {
    OP_AND, OP_ANDN, OP_OR, OP_XOR, OP_ADD, OP_SUB, OP_EQ, OP_GT,
    OP_MINU, OP_MAXU, OP_MULUDQ, OP_MULLD, OP_SADBW, OP_SHUFB, OP_SHUFD,
    OP_ALIGNR, OP_SLL, OP_SRL, OP_SRA, OP_BLENDD, OP_BCASTB, OP_BCASTW,
    OP_BCASTD, OP_BCASTQ, OP_BCASTSS, OP_BCASTSD
};

namespace {

constexpr unsigned EB = 0, EW = 1, ED = 2, EQ = 3;
constexpr unsigned F_REG = 0, F_M8 = 1, F_M32 = 2, F_SIB = 3, F_RIP = 4;

// ---------------------------------------------------------------- RWX layout
// One self-contained allocation, so every rel32 the patcher computes stays
// inside it and the test does not depend on where the loader placed us.
constexpr std::size_t kRwxSize = 0x400000;
constexpr std::size_t kDataOff = 0x000000;   // memory operands point here
constexpr std::size_t kStubOff = 0x100000;   // the generated stub
constexpr std::size_t kSiteOff = 0x102000;   // a copy of the original instruction
constexpr std::size_t kDataBOff = 0x200000;  // alternate data page (positive rip disp)
constexpr std::size_t kPageSize = 0x10000;

// The stack the stub runs on. The frame is at most 136 bytes below the call's
// return address, so 512 is a safe boundary for the canary check.
// The stub pushes its frame *below* the stack pointer it is given, so the
// harness hands it a pointer 0x1008 bytes into the buffer: that leaves 4 KiB of
// headroom underneath for the frame, and keeps rsp 16-aligned at the call.
constexpr std::size_t kScratchSize = 0x10000;
constexpr std::size_t kStackTop = 0x1008;
constexpr unsigned char kCanary = 0xA7;

struct RegFile {
    unsigned char Ymm[16][32];
    unsigned long long Gpr[16];  // 0=rax 1=rcx 2=rdx 3=rbx 4=rsp 5=rbp 6=rsi 7=rdi 8..15
};

std::uint8_t* gRwx = nullptr;
RegFile gIn;
RegFile gOut;
unsigned char* gScratch = nullptr;
// Distinct, valid, well-separated addresses: the stub is free to use any of
// these as a stack frame base, so none of them may be junk.
unsigned char* gRegs = nullptr;
constexpr std::size_t kRegsSize = 0x40000;
constexpr std::size_t kRegStride = 0x1000;
constexpr std::size_t kRegBase = 0x1000;  // headroom below each value
const char* gCurrentCase = "";
unsigned long long gExpectedR11 = 0;
bool gDumpStub = false;
unsigned char* gStub = nullptr;

LONG WINAPI Veh(EXCEPTION_POINTERS* ep) {
    char buf[192];
    const CONTEXT* c = ep->ContextRecord;
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "FAULT %s code=%08llx rip=%llx rsp=%llx r10=%llx r11=%llx\n",
                gCurrentCase,
                (unsigned long long)ep->ExceptionRecord->ExceptionCode,
                (unsigned long long)(ULONG_PTR)c->Rip,
                (unsigned long long)((ULONG_PTR)c->Rip - (ULONG_PTR)GetModuleHandleW(nullptr)),
                (unsigned long long)c->Rsp, (unsigned long long)c->Rbp,
                (unsigned long long)c->R12, (unsigned long long)c->Rdi,
                (unsigned long long)c->Rsi);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, lstrlenA(buf), nullptr, nullptr);
    ExitProcess(3);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------ reference
template <typename T>
T Load(const unsigned char* p, unsigned i) {
    T v;
    std::memcpy(&v, p + i * sizeof(T), sizeof(T));
    return v;
}

template <typename T>
void Store(unsigned char* p, unsigned i, T v) {
    std::memcpy(p + i * sizeof(T), &v, sizeof(T));
}

template <typename T, typename F>
void Map2(const unsigned char* a, const unsigned char* b, unsigned char* r, unsigned n, F f) {
    for (unsigned i = 0; i < n / sizeof(T); ++i) {
        Store<T>(r, i, f(Load<T>(a, i), Load<T>(b, i)));
    }
}

template <typename T, typename F>
void Map1(const unsigned char* a, unsigned char* r, unsigned n, F f) {
    for (unsigned i = 0; i < n / sizeof(T); ++i) {
        Store<T>(r, i, f(Load<T>(a, i)));
    }
}

// dst = OP(a) shifted by imm, with the immediate masked to the element width,
// exactly like the hardware does.
template <typename T, bool Arith>
void Shift1(const unsigned char* a, unsigned char* r, unsigned n, unsigned imm) {
    const unsigned bits = sizeof(T) * 8;
    const unsigned sh = (bits == 64) ? (imm & 63) : (imm & 31);
    for (unsigned i = 0; i < n / sizeof(T); ++i) {
        const T x = Load<T>(a, i);
        Store<T>(r, i, sh == 0 ? x : (Arith ? static_cast<T>(x >> sh) : static_cast<T>(x >> sh)));
    }
}

void ReferenceCompute(const Avx2DiffCase& c, const unsigned char* va,
                      const unsigned char* vb, unsigned char* out) {
    const unsigned n = c.Ymm ? 32u : 16u;
    std::memset(out, 0, 32);

    switch (c.Op) {
        case OP_AND:  Map2<unsigned char>(va, vb, out, n, [](unsigned char x, unsigned char y) { return x & y; }); break;
        case OP_ANDN: Map2<unsigned char>(va, vb, out, n, [](unsigned char x, unsigned char y) { return static_cast<unsigned char>(~x & y); }); break;
        case OP_OR:   Map2<unsigned char>(va, vb, out, n, [](unsigned char x, unsigned char y) { return x | y; }); break;
        case OP_XOR:  Map2<unsigned char>(va, vb, out, n, [](unsigned char x, unsigned char y) { return x ^ y; }); break;

        case OP_ADD:
            if (c.Elem == EB) Map2<unsigned char>(va, vb, out, n, [](unsigned char x, unsigned char y) { return static_cast<unsigned char>(x + y); });
            else if (c.Elem == EW) Map2<unsigned short>(va, vb, out, n, [](unsigned short x, unsigned short y) { return static_cast<unsigned short>(x + y); });
            else if (c.Elem == ED) Map2<unsigned>(va, vb, out, n, [](unsigned x, unsigned y) { return x + y; });
            else Map2<unsigned long long>(va, vb, out, n, [](unsigned long long x, unsigned long long y) { return x + y; });
            break;

        case OP_SUB:
            if (c.Elem == EB) Map2<unsigned char>(va, vb, out, n, [](unsigned char x, unsigned char y) { return static_cast<unsigned char>(x - y); });
            else if (c.Elem == EW) Map2<unsigned short>(va, vb, out, n, [](unsigned short x, unsigned short y) { return static_cast<unsigned short>(x - y); });
            else if (c.Elem == ED) Map2<unsigned>(va, vb, out, n, [](unsigned x, unsigned y) { return x - y; });
            else Map2<unsigned long long>(va, vb, out, n, [](unsigned long long x, unsigned long long y) { return x - y; });
            break;

        case OP_EQ:
            if (c.Elem == EB) Map2<unsigned char>(va, vb, out, n, [](unsigned char x, unsigned char y) { return x == y ? 0xFFu : 0u; });
            else if (c.Elem == EW) Map2<unsigned short>(va, vb, out, n, [](unsigned short x, unsigned short y) { return x == y ? 0xFFFFu : 0u; });
            else if (c.Elem == ED) Map2<unsigned>(va, vb, out, n, [](unsigned x, unsigned y) { return x == y ? 0xFFFFFFFFu : 0u; });
            else Map2<unsigned long long>(va, vb, out, n, [](unsigned long long x, unsigned long long y) { return x == y ? ~0ull : 0ull; });
            break;

        case OP_GT:
            if (c.Elem == EB) Map2<signed char>(va, vb, out, n, [](signed char x, signed char y) { return x > y ? 0xFFu : 0u; });
            else if (c.Elem == EW) Map2<short>(va, vb, out, n, [](short x, short y) { return x > y ? 0xFFFFu : 0u; });
            else if (c.Elem == ED) Map2<int>(va, vb, out, n, [](int x, int y) { return x > y ? 0xFFFFFFFFu : 0u; });
            else Map2<long long>(va, vb, out, n, [](long long x, long long y) { return x > y ? ~0ull : 0ull; });
            break;

        case OP_MINU: Map2<unsigned>(va, vb, out, n, [](unsigned x, unsigned y) { return x < y ? x : y; }); break;
        case OP_MAXU: Map2<unsigned>(va, vb, out, n, [](unsigned x, unsigned y) { return x > y ? x : y; }); break;

        case OP_MULLD:
            Map2<int>(va, vb, out, n, [](int x, int y) { return static_cast<unsigned>(x * y); });
            break;

        case OP_MULUDQ:
            for (unsigned i = 0; i < n / 16; ++i) {
                const unsigned long long p =
                    (unsigned long long)Load<unsigned>(va, i * 4 + 0) * Load<unsigned>(vb, i * 4 + 0) +
                    (unsigned long long)Load<unsigned>(va, i * 4 + 1) * Load<unsigned>(vb, i * 4 + 1);
                (void)p;
            }
            // VPMULUDQ: dst.q[i] = (u32)a[2i] * (u32)b[2i], odd qwords zeroed.
            for (unsigned lane = 0; lane < n / 16; ++lane) {
                for (unsigned q = 0; q < 2; ++q) {
                    const unsigned idx = lane * 4 + q * 2;
                    const unsigned long long v =
                        (unsigned long long)Load<unsigned>(va, idx) * Load<unsigned>(vb, idx);
                    Store<unsigned long long>(out, lane * 2 + q, v);
                }
            }
            break;

        case OP_SADBW:
            for (unsigned lane = 0; lane < n / 16; ++lane) {
                for (unsigned g = 0; g < 2; ++g) {
                    unsigned sum = 0;
                    for (unsigned k = 0; k < 8; ++k) {
                        const unsigned x = va[lane * 16 + g * 8 + k];
                        const unsigned y = vb[lane * 16 + g * 8 + k];
                        sum += x > y ? x - y : y - x;
                    }
                    Store<unsigned short>(out, lane * 2 + g, static_cast<unsigned short>(sum));
                }
            }
            break;

        case OP_SHUFB:
            for (unsigned lane = 0; lane < n / 16; ++lane) {
                for (unsigned k = 0; k < 16; ++k) {
                    const unsigned char m = vb[lane * 16 + k];
                    out[lane * 16 + k] = (m & 0x80) ? 0 : va[lane * 16 + (m & 0x0F)];
                }
            }
            break;

        case OP_SHUFD:
            for (unsigned lane = 0; lane < n / 16; ++lane) {
                for (unsigned j = 0; j < 4; ++j) {
                    const unsigned sel = (c.Imm >> (2 * j)) & 3;
                    Store<unsigned>(out, lane * 4 + j, Load<unsigned>(va, lane * 4 + sel));
                }
            }
            break;

        case OP_ALIGNR:
            for (unsigned lane = 0; lane < n / 16; ++lane) {
                unsigned char cat[32];
                std::memcpy(cat, va + lane * 16, 16);
                std::memcpy(cat + 16, vb + lane * 16, 16);
                for (unsigned k = 0; k < 16; ++k) {
                    out[lane * 16 + k] = cat[(c.Imm + k) & 0x1F];
                }
            }
            break;

        case OP_SLL:
            if (c.Elem == EW) Shift1<unsigned short, false>(va, out, n, c.Imm);
            else if (c.Elem == ED) Shift1<unsigned, false>(va, out, n, c.Imm);
            else Shift1<unsigned long long, false>(va, out, n, c.Imm);
            break;
        case OP_SRL:
            if (c.Elem == EW) Shift1<unsigned short, false>(va, out, n, c.Imm);
            else if (c.Elem == ED) Shift1<unsigned, false>(va, out, n, c.Imm);
            else Shift1<unsigned long long, false>(va, out, n, c.Imm);
            break;
        case OP_SRA:
            if (c.Elem == ED) Shift1<int, true>(va, out, n, c.Imm);
            else Shift1<long long, true>(va, out, n, c.Imm);
            break;

        case OP_BLENDD:
            for (unsigned i = 0; i < n / 4; ++i) {
                const unsigned v = ((c.Imm >> i) & 1) ? Load<unsigned>(vb, i) : Load<unsigned>(va, i);
                Store<unsigned>(out, i, v);
            }
            break;

        case OP_BCASTB: for (unsigned i = 0; i < n; ++i) out[i] = vb[0]; break;
        // Splat element 0 of the source across the whole register. These must not
        // go through Map1 with an identity function: that applies the function to
        // every element and so copies vb verbatim, which makes every broadcast
        // case compare against the unmodified source instead of the splat.
        case OP_BCASTW: { const unsigned short v = Load<unsigned short>(vb, 0);
                          for (unsigned i = 0; i < n / 2; ++i) Store<unsigned short>(out, i, v); } break;
        case OP_BCASTD: { const unsigned v = Load<unsigned>(vb, 0);
                          for (unsigned i = 0; i < n / 4; ++i) Store<unsigned>(out, i, v); } break;
        case OP_BCASTQ: { const unsigned long long v = Load<unsigned long long>(vb, 0);
                          for (unsigned i = 0; i < n / 8; ++i) Store<unsigned long long>(out, i, v); } break;
        case OP_BCASTSS: { const unsigned v = Load<unsigned>(vb, 0);
                           for (unsigned i = 0; i < n / 4; ++i) Store<unsigned>(out, i, v); } break;
        case OP_BCASTSD: { const unsigned long long v = Load<unsigned long long>(vb, 0);
                           for (unsigned i = 0; i < n / 8; ++i) Store<unsigned long long>(out, i, v); } break;
        default: break;
    }
}

// ------------------------------------------------------------------ test glue
unsigned Next(std::uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}

void FillPattern(unsigned char* p, std::size_t n, std::uint32_t seed) {
    for (std::size_t i = 0; i < n; ++i) {
        p[i] = static_cast<unsigned char>(Next(seed));
    }
}

std::uint32_t ReadU32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void WriteU32(unsigned char* p, std::uint32_t v) {
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
    p[2] = static_cast<unsigned char>(v >> 16);
    p[3] = static_cast<unsigned char>(v >> 24);
}

std::string Hex32(const unsigned char* p, std::size_t n) {
    std::string s;
    char buf[8];
    for (std::size_t i = 0; i < n; ++i) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02X", p[i]);
        if (i) s += ' ';
        s += buf;
    }
    return s;
}

struct Report {
    const char* Kind;
    std::string Case;
    std::string Detail;
};

int CheckCase(unsigned index, const Avx2DiffCase& c, const Codegen::IAvx2InstructionMatcher& matcher,
              const Codegen::IAvx2StubEmitter& emitter, bool verbose, std::vector<Report>& failures) {
    const unsigned char* orig = avx2_case_tbl[index];
    const bool rip = (c.Form == F_RIP);
    unsigned char* dataPage = gRwx + (rip && (index & 1) ? kDataBOff : kDataOff);
    unsigned char* site = gRwx + kSiteOff;
    unsigned char* stub = gRwx + kStubOff;
    gStub = stub;

    // The instruction is padded to 32 bytes; the matcher insists on an exact
    // length, so the first accepted length is the instruction length.
    std::size_t len = 0;
    for (std::size_t l = 4; l <= 16; ++l) {
        if (matcher.Match(orig, l)) { len = l; break; }
    }
    if (len == 0) {
        failures.push_back({c.Name, "unmatched", "no instruction length in 4..16 was accepted"});
        return 0;
    }
    if (len < 5) {
        // The converter skips these; they go through the VEH short-site table.
        if (verbose) printf("  [skip] %-34s %llu-byte site (VEH path)\n", c.Name, len);
        return 0;
    }

    // Re-anchor the RIP-relative operand onto the RWX data page, the same way
    // the image is relocated before conversion.
    int dispOffset = -1;
    if (rip) {
        const auto origTarget = reinterpret_cast<std::intptr_t>(avx2diff_data) + c.RipOff;
        const std::int32_t want = static_cast<std::int32_t>(
            origTarget - (reinterpret_cast<std::intptr_t>(orig) + static_cast<std::intptr_t>(len)));
        for (std::size_t i = 3; i + 4 <= len; ++i) {
            if (static_cast<std::int32_t>(ReadU32(orig + i)) == want) { dispOffset = static_cast<int>(i); break; }
        }
        if (dispOffset < 0) {
            failures.push_back({c.Name, "no-disp32", "could not locate the rip disp32 in the assembler encoding"});
            return 0;
        }
    }
    std::memcpy(site, orig, len);
    if (rip) {
        const auto target = reinterpret_cast<std::intptr_t>(dataPage) + c.RipOff -
                            (reinterpret_cast<std::intptr_t>(site) + static_cast<std::intptr_t>(len));
        WriteU32(site + dispOffset, static_cast<std::uint32_t>(static_cast<std::int32_t>(target)));
    }

    const auto match = matcher.Match(site, len);
    if (!match) {
        failures.push_back({c.Name, "unmatched", "the relocated encoding stopped matching"});
        return 0;
    }

    std::size_t rel32 = 0, ripOff = 0;
    std::vector<std::uint8_t> blob;
    try {
        blob = emitter.EmitDispatch(*match, rel32, ripOff);
    } catch (const std::exception& e) {
        failures.push_back({c.Name, "emit", e.what()});
        return 0;
    }
    if (blob.size() > 0x1000) {
        failures.push_back({c.Name, "emit", "stub does not fit the stub area"});
        return 0;
    }
    if (rel32 + 4 != blob.size()) {
        failures.push_back({c.Name, "emit", "the E9 tail is not at the end of the stub"});
        return 0;
    }

    if (gDumpStub) printf("  stub %-32s %s\n", c.Name, Hex32(blob.data(), blob.size()).c_str());
    std::memcpy(stub, blob.data(), blob.size());
    // The stub's tail jumps back to the instruction after the site; in the test
    // it jumps straight into the harness's continuation, which is only possible
    // if the RWX block was allocated within 2 GiB of this code (see main).
    {
        const std::int64_t from = static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(stub + rel32 + 4));
        WriteU32(stub + rel32, static_cast<std::uint32_t>(
            static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(&avx2_diff_cont) - from)));
    }
    if (rip) {
        if (ripOff == static_cast<std::size_t>(-1)) {
            failures.push_back({c.Name, "rip", "emitter reported no disp32 for a rip-relative operand"});
            return 0;
        }
        if (static_cast<std::int32_t>(ReadU32(site + dispOffset)) !=
            static_cast<std::int32_t>(ReadU32(stub + ripOff))) {
            char buf[192];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "emitter reported the copied disp32 at stub+%zu but the real field is at +%d",
                        ripOff, dispOffset);
            failures.push_back({c.Name, "rip-offset", buf});
        }
        const std::int64_t from = static_cast<std::int64_t>(kStubOff + ripOff + 4);
        const std::int64_t to = static_cast<std::int64_t>(dataPage - gRwx) + c.RipOff;
        WriteU32(stub + ripOff, static_cast<std::uint32_t>(static_cast<std::int32_t>(to - from)));
    }


    // ------------------------------------------------------------- run
    RegFile& in = gIn;
    RegFile& out = gOut;
    std::uint32_t seed = 0x1234u + index * 2654435761u;
    FillPattern(reinterpret_cast<unsigned char*>(in.Ymm), sizeof(in.Ymm), seed);
    FillPattern(gRwx + kDataOff, kPageSize, seed ^ 0x5A5A5A5Au);
    FillPattern(gRwx + kDataBOff, kPageSize, seed ^ 0xA5A5A5A5u);

    // Registers that are NOT memory bases must not look like pointers. The stub
    // picks a scratch GPR to hold its frame and, before this was fixed, assumed the
    // register already pointed at writable memory. In the emulator that meant
    // `vmovdqu [r10-0x40]` with r10 = 3, which faulted. Filling the registers with
    // pointers here hid that: only the two registers actually used as a memory base
    // (rsi for the m8/m32/sib forms, rcx as the SIB index) get a real address.
    for (unsigned i = 0; i < 16; ++i) {
        in.Gpr[i] = reinterpret_cast<unsigned long long>(gRegs + kRegBase + i * kRegStride);
    }
    for (unsigned i = 0; i < 16; ++i) {
        if (i != 6 && i != 1) {   // keep rsi and rcx; everything else is not a base
            in.Gpr[i] = 3ull + i; // small non-canonical value: writing near it faults
        }
    }
    in.Gpr[4] = 0;                                 // rsp is never loaded
    in.Gpr[6] = reinterpret_cast<unsigned long long>(dataPage);  // rsi: memory operand
    if (c.Form == F_SIB) {
        in.Gpr[1] = 0x40ull;                       // rcx: the SIB index
    }
    in.Gpr[11] = 0;  // r11 is the harness's code pointer, checked separately
    std::memset(&out, 0, sizeof(out));
    std::memset(gScratch, kCanary, kScratchSize);

    gCurrentCase = c.Name;
    gExpectedR11 = reinterpret_cast<unsigned long long>(stub);
    if (gDumpStub) { printf("         ...run\n"); fflush(stdout); }
    avx2_diff_run(&in, &out, stub, gScratch + kStackTop);
    if (gDumpStub) { printf("         ...returned\n"); fflush(stdout); }

    // ------------------------------------------------------------ verify
    unsigned char want[32];
    std::memcpy(want, in.Ymm[c.Res], 32);
    const unsigned char* va = in.Ymm[c.A];
    // Where the stub's memory operand actually points. The SIB form reads
    // [rsi + rcx*4] with rcx = 0x40, so its bytes start 0x100 into the page; the
    // rip1 cases read dataPage + 0x1800. Comparing against a correct stub while
    // referencing the page base made these look like emitter bugs.
    const unsigned char* vb = (c.Form == F_REG)  ? in.Ymm[c.B]
                       : (c.Form == F_SIB)  ? dataPage + 0x40 * 4
                       : (c.Form == F_RIP)  ? dataPage + c.RipOff
                                            : dataPage;
    ReferenceCompute(c, va, vb, want);
    if (!c.Ymm) {
        std::memcpy(want + 16, in.Ymm[c.Res] + 16, 16);  // 128-bit forms keep the top half
    }
    if (std::memcmp(want, out.Ymm[c.Res], 32) != 0) {
        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "dst ymm%d\n      want %s\n      got  %s",
                    c.Res, Hex32(want, 32).c_str(), Hex32(out.Ymm[c.Res], 32).c_str());
        failures.push_back({c.Name, "result", buf});
    }
    for (unsigned r = 0; r < 16; ++r) {
        if (r == c.Res) continue;
        if (std::memcmp(in.Ymm[r], out.Ymm[r], 32) != 0) {
            char buf[512];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "clobbered ymm%d (source of the scratch regs)\n      want %s\n      got  %s",
                        r, Hex32(in.Ymm[r], 32).c_str(), Hex32(out.Ymm[r], 32).c_str());
            failures.push_back({c.Name, "clobber", buf});
        }
    }
    for (unsigned r = 0; r < 16; ++r) {
        const unsigned long long expect = (r == 11) ? gExpectedR11 : in.Gpr[r];
        if (out.Gpr[r] != expect) {
            char buf[192];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "gpr%u clobbered: want %llx got %llx",
                        r, expect, out.Gpr[r]);
            failures.push_back({c.Name, "gpr", buf});
        }
    }
    // The stub's frame lives in the lowest 256 bytes below the harness's stack
    // top; anything outside that must still be the canary pattern.
    {
        unsigned char* top = gScratch + kStackTop;
        for (std::size_t i = 0; i < kScratchSize; ++i) {
            unsigned char* p = gScratch + i;
            if (p + 8 > top - 512 && p < top + 16) continue;
            // The trampoline's stash occupies top-0x318..top-0x2E0 by design.
            if (p >= top - 0x318 && p + 8 <= top - 0x2E0) continue;
            if (*p != kCanary) {
                char buf[192];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "stack canary broken at scratch+%zu (0x%02X)", i, *p);
                failures.push_back({c.Name, "canary", buf});
                break;
            }
        }
    }
    if (verbose) {
        printf("  [ok]   %-34s op=%u len=%zu\n", c.Name, static_cast<unsigned>(match->Op), len);
        if (gDumpStub) {
            std::string h = Hex32(blob.data(), blob.size());
            printf("         stub %s\n", h.c_str());
        }
    }
    return 0;
}

// Isolated check of the trampoline itself: a "stub" that is just `ret` must
// leave the whole register file untouched. Any mismatch here is a harness bug,
// not a stub bug.
int RunTrampolineSelfTest() {
    RegFile& in = gIn;
    RegFile& out = gOut;
    gRegs = static_cast<unsigned char*>(VirtualAlloc(nullptr, kRegsSize, MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_READWRITE));
    unsigned char* scratchBase = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, kScratchSize + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    std::memset(gRegs, 0x5C, kRegsSize);
    std::memset(scratchBase, kCanary, kScratchSize + 0x1000);
    gScratch = scratchBase;

    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    gRwx = static_cast<std::uint8_t*>(
        VirtualAlloc(reinterpret_cast<void*>(moduleBase + 0x04000000ull), kRwxSize,
                     MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!gRwx || !gRegs || !gScratch) { printf("selftest: allocation failed\n"); return 2; }

    unsigned char* code = gRwx + kStubOff;
    // A jmp to avx2_diff_cont, matching the shape of a real generated stub. A
    // bare `ret` would pop the trampoline's return address and leave rsp at R,
    // while a jmp tail leaves it at R-8 -- the harness only compensates for one.
    {
        const auto target = reinterpret_cast<std::intptr_t>(&avx2_diff_cont);
        const std::int32_t rel = static_cast<std::int32_t>(
            target - (reinterpret_cast<std::intptr_t>(code) + 5));
        code[0] = 0xE9;
        WriteU32(code + 1, static_cast<std::uint32_t>(rel));
    }

    std::uint32_t seed = 0xC0FFEEu;
    FillPattern(reinterpret_cast<unsigned char*>(in.Ymm), sizeof(in.Ymm), seed);
    for (unsigned i = 0; i < 16; ++i) {
        in.Gpr[i] = reinterpret_cast<unsigned long long>(gRegs + kRegBase + i * kRegStride);
    }
    in.Gpr[4] = 0;
    std::memset(&out, 0, sizeof(out));
    // Independent oracle: compare `out` against a copy taken before the call, not
    // against `in` itself. If `in` is disturbed by anything, comparing against the
    // live `in` reports a mismatch the harness invented.
        RegFile expect = in;
    gCurrentCase = "trampoline-selftest";

    printf("selftest: gRwx=%llx code=%llx R=%llx in.gpr11=%llx\\n",
           (unsigned long long)gRwx, (unsigned long long)code,
           (unsigned long long)(gScratch + kStackTop), in.Gpr[11]);
    printf("selftest: in=%p out=%p gRegs=%p out.Gpr[0]=%llx\n", (void*)&in, (void*)&out,
           (void*)gRegs, (unsigned long long)out.Gpr[0]);
    avx2_diff_run(&in, &out, code, gScratch + kStackTop);

    // The trampoline parks out/code/original-rsp in the scratch stack just below
    // the stub's entry rsp and reloads them afterwards. If they did not survive
    // untouched, the stub did not leave rsp where the trampoline expected and the
    // reload read garbage -- which shows up as "out" filled with arena bytes.
    {
        const unsigned long long* stash = reinterpret_cast<const unsigned long long*>(gScratch + kStackTop - 0x110);
        printf("selftest: stash[code]=%llx (want %llx)\n", stash[0], (unsigned long long)code);
        printf("selftest: stash[out] =%llx (want %llx)\n", stash[1], (unsigned long long)&out);
        printf("selftest: stash[rsp] =%llx (want %llx)\n", stash[2],
               (unsigned long long)(gScratch + kStackTop));
        printf("selftest: out=%p in=%p out.Gpr[0]=%llx\n", (void*)&out, (void*)&in,
               (unsigned long long)out.Gpr[0]);
    }

    int bad = 0;
    for (unsigned r = 0; r < 16; ++r) {
        if (std::memcmp(expect.Ymm[r], out.Ymm[r], 32) != 0) {
            printf("selftest: ymm%u not preserved\n", r); ++bad;
        }
        const unsigned long long want = (r == 11) ? reinterpret_cast<unsigned long long>(code) : expect.Gpr[r];
        if (out.Gpr[r] != want) {
            printf("selftest: gpr%u want %llx got %llx\n", r, want, out.Gpr[r]); ++bad;
        }
    }
    unsigned char* top = gScratch + kStackTop;
    for (std::size_t i = 0; i < kScratchSize; ++i) {
        unsigned char* p = gScratch + i;
        if (p + 8 > top - 512 && p < top + 16) continue;
        // The trampoline's own stash sits at top-0x300..top-0x2f0 by design: the
        // stub's frame reaches down past top-0x100, so the slots cannot live there.
        // They are written before the stub runs, so they are expected to differ.
        if (p >= top - 0x318 && p + 8 <= top - 0x2E0) continue;
        if (*p != kCanary) { printf("selftest: canary broken at +%zu\n", i); ++bad; break; }
    }
    printf("selftest: %s\n", bad == 0 ? "PASS" : "FAIL");
    return bad;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "-selftest") == 0) {
        AddVectoredExceptionHandler(1, Veh);
        return RunTrampolineSelfTest();
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool verbose = false;
    unsigned start = 0;
    unsigned count = 0xFFFFFFFFu;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0) verbose = true;
        if (std::strcmp(argv[i], "-d") == 0) gDumpStub = true;
        if (std::strcmp(argv[i], "-start") == 0 && i + 1 < argc) start = static_cast<unsigned>(atoi(argv[++i]));
        if (std::strcmp(argv[i], "-count") == 0 && i + 1 < argc) count = static_cast<unsigned>(atoi(argv[++i]));
    }

    // The stub's E9 tail has to reach avx2_diff_cont, so ask for the block
    // within 2 GiB of this module. A hint of base+64 MiB lands in the same
    // 2 GiB window without colliding with the loader.
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto hint = reinterpret_cast<void*>(moduleBase + 0x04000000ull);
    gRwx = static_cast<std::uint8_t*>(VirtualAlloc(hint, kRwxSize, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_EXECUTE_READWRITE));
    if (!gRwx) {
        fprintf(stderr, "VirtualAlloc near the module failed (%lu)\\n", GetLastError());
        return 2;
    }
    {
        const auto delta = static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(&avx2_diff_cont)) -
                           static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(gRwx));
        if (delta < INT32_MIN || delta > INT32_MAX) {
            fprintf(stderr, "RWX block at %llx is out of rel32 range of the harness\\n",
                    (unsigned long long)gRwx);
            return 2;
        }
    }
    void* scratchBase = VirtualAlloc(nullptr, kScratchSize + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    gRegs = static_cast<unsigned char*>(VirtualAlloc(nullptr, kRegsSize, MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_READWRITE));
    if (!gRwx || !scratchBase || !gRegs) {
        fprintf(stderr, "VirtualAlloc failed (%lu)\n", GetLastError());
        return 2;
    }
    gScratch = static_cast<unsigned char*>(scratchBase);
    std::memset(gRegs, 0x5C, kRegsSize);
    printf("rwx=%llx stub=%llx site=%llx dataA=%llx scratch=%llx cont=%llx\n",
           (unsigned long long)gRwx, (unsigned long long)(gRwx + kStubOff),
           (unsigned long long)(gRwx + kSiteOff),
           (unsigned long long)gRwx, (unsigned long long)gScratch,
           (unsigned long long)(void*)&avx2_diff_cont);
    AddVectoredExceptionHandler(1, Veh);

    const auto matcher = Codegen::MakeAvx2InstructionMatcher();
    const auto emitter = Codegen::MakeAvx2StubEmitter();

    std::vector<Report> failures;
    unsigned shortSites = 0;
    for (unsigned i = start; i < kAvx2CaseCount && i < start + count; ++i) {
        const std::size_t before = failures.size();
        if (verbose) printf("  [run]  %s\n", kAvx2Cases[i].Name);
        const unsigned char* orig = avx2_case_tbl[i];
        bool isShort = false;
        for (std::size_t l = 4; l <= 16; ++l) {
            if (matcher->Match(orig, l)) { isShort = l < 5; break; }
        }
        if (isShort) ++shortSites;
        CheckCase(i, kAvx2Cases[i], *matcher, *emitter, verbose, failures);
        if (failures.size() == before && !verbose) {
            printf("  [ok]   %s\n", kAvx2Cases[i].Name);
        }
    }

    printf("\n%d cases, %zu failures, %u of them 4-byte VEH sites\n",
           kAvx2CaseCount, failures.size(), shortSites);
    for (const auto& f : failures) {
        printf("FAIL %-12s %-32s %s\n", f.Kind, f.Case.c_str(), f.Detail.c_str());
    }
    return failures.empty() ? 0 : 1;
}
