#include <codegen/x86/IAvx2InstructionMatcher.hpp>

#include <cstring>
#include <memory>

namespace Codegen {

namespace {

constexpr std::uint8_t Vex3 = 0xC4;
constexpr std::uint8_t Vex2 = 0xC5;

struct VexPrefix {
    unsigned r = 0, x = 0, b = 0;
    bool wide = false;
    std::uint8_t vvvv = 0;
    bool ymm = false;
    std::uint8_t pp = 0;
    std::uint8_t map = 1;
    std::size_t pos = 0;   // index of the opcode byte
};

bool parseVex(const std::uint8_t* data, const std::size_t length, VexPrefix& out) {
    if (data[0] == Vex2) {
        if (length < 3) {
            return false;
        }
        const std::uint8_t b1 = data[1];
        out.r = ((b1 >> 7) & 1) ^ 1;
        out.x = 0;  // not encoded in the 2-byte form
        out.b = 0;
        out.vvvv = static_cast<std::uint8_t>(((b1 >> 3) & 0x0F) ^ 0x0F);
        out.ymm = ((b1 >> 2) & 1) != 0;
        out.pp = static_cast<std::uint8_t>(b1 & 3);
        out.wide = false;
        out.map = 1;
        out.pos = 2;
        return true;
    }
    if (data[0] == Vex3) {
        if (length < 5) {
            return false;
        }
        const std::uint8_t b1 = data[1];
        const std::uint8_t b2 = data[2];
        out.r = ((b1 >> 7) & 1) ^ 1;
        out.x = ((b1 >> 6) & 1) ^ 1;
        out.b = ((b1 >> 5) & 1) ^ 1;
        out.map = static_cast<std::uint8_t>(b1 & 0x1F);
        out.wide = ((b2 >> 7) & 1) != 0;
        out.vvvv = static_cast<std::uint8_t>(((b2 >> 3) & 0x0F) ^ 0x0F);
        out.ymm = ((b2 >> 2) & 1) != 0;
        out.pp = static_cast<std::uint8_t>(b2 & 3);
        out.pos = 3;
        return true;
    }
    return false;
}

std::int32_t readDisp(const std::uint8_t* data) {
    std::int32_t value{};
    std::memcpy(&value, data, 4);
    return value;
}

// Decodes the ModRM plus any SIB/displacement. Mirrors the BMI matcher's
// operand walk so both decoders agree on what `Mem` means.
struct Operands {
    std::uint8_t reg = 0;
    std::uint8_t rm = 0;
    bool isMem = false;
    BmiMemOperand mem;
    std::size_t after = 0;
};

Operands decodeOperands(const std::uint8_t* data, const std::size_t pos, const unsigned r, const unsigned x, const unsigned b) {
    Operands out;
    const std::uint8_t mr = data[pos];
    const auto mod = static_cast<std::uint8_t>(mr >> 6);
    out.reg = static_cast<std::uint8_t>(((mr >> 3) & 7) | (r << 3));
    const auto rm = static_cast<std::uint8_t>(mr & 7);
    out.after = pos + 1;

    if (mod == 3) {
        out.rm = static_cast<std::uint8_t>(rm | (b << 3));
        return out;
    }

    out.isMem = true;
    out.mem.Mod = mod;
    std::size_t p = pos + 1;
    if (rm == 4) {
        const std::uint8_t sib = data[p];
        ++p;
        out.mem.HasSib = true;
        out.mem.Scale = static_cast<std::uint8_t>(1u << (sib >> 6));
        const auto indexCode = static_cast<std::uint8_t>((sib >> 3) & 7);
        const auto baseCode = static_cast<std::uint8_t>(sib & 7);
        if (indexCode != 4) {
            out.mem.Index = static_cast<std::uint8_t>(indexCode | (x << 3));
            out.mem.IndexCode = indexCode;
        }
        if (mod == 0 && baseCode == 5) {
            out.mem.Disp = readDisp(data + p);
            p += 4;
        } else {
            out.mem.Base = static_cast<std::uint8_t>(baseCode | (b << 3));
            out.mem.BaseCode = baseCode;
        }
        if (mod == 1) {
            std::int8_t d{};
            std::memcpy(&d, data + p, 1);
            out.mem.Disp = d;
            p += 1;
        } else if (mod == 2) {
            out.mem.Disp = readDisp(data + p);
            p += 4;
        }
    } else if (mod == 0 && rm == 5) {
        out.mem.Rip = true;
        out.mem.Disp = readDisp(data + p);
        p += 4;
    } else if (mod == 1) {
        std::int8_t d{};
        std::memcpy(&d, data + p, 1);
        out.mem.Disp = d;
        out.mem.Base = static_cast<std::uint8_t>(rm | (b << 3));
        out.mem.BaseCode = rm;
        p += 1;
    } else if (mod == 2) {
        out.mem.Disp = readDisp(data + p);
        out.mem.Base = static_cast<std::uint8_t>(rm | (b << 3));
        out.mem.BaseCode = rm;
        p += 4;
    } else {
        out.mem.Base = static_cast<std::uint8_t>(rm | (b << 3));
        out.mem.BaseCode = rm;
    }

    out.after = p;
    return out;
}

// One row per VEX opcode we know how to emulate.
// VEX.W requirement of a row.
//
// Intel writes this as W0 / W1 / WIG in the opcode tables. WIG ("write ignored")
// matters more than it looks: a WIG instruction may legally be encoded with the
// two-byte VEX form, which has no W field at all. Gas does exactly that --
// `vpaddq %ymm2,%ymm1,%ymm0` comes out as the 4-byte C5 F5 D4 C2. Requiring W=1
// for such a row therefore rejects perfectly legal encodings, which is how
// VPADDQ / VPSUBQ / VPMULUDQ / VPCMPEQQ ended up reported as "unmatched".
//
// The word-size shifts really do depend on W (W0 = dword, W1 = qword), so they
// stay kWideW1.
enum class WideReq { kWideW0, kWideW1, kWideIgnored };

struct OpRow {
    Avx2Op Op;
    std::uint8_t Map;
    std::uint8_t Opcode;
    std::uint8_t Digits;   // 0 = ModRM.reg is a real register; else /digit
    WideReq Wide;
    bool NeedsImm;         // opcode is followed by imm8
    bool AnyL;             // illegal at 128 bits too
    bool RegSourceOnly;    // memory source is a legal AVX1 encoding
};

// Rows spell the W requirement directly: W0, W1 or WIG.
#define ROW(op, map, opcode, digits, wide, imm, anyl, regonly) \
    Avx2Op::op, map, opcode, digits, WideReq::wide, imm, anyl, regonly

constexpr OpRow kRows[] = {
    // shape A / D: only the 256-bit encoding traps
    ROW(And, 1, 0xDB, 0, kWideW0, false, false, false),
    // VPANDN is 0F DF with a plain /r ModRM. It is not a /digit member of the
    // 0F DB group, and gas/LLVM encode it that way, so anything else would leave
    // every VPANDN in the image executing as raw AVX2 -- an immediate #UD.
    ROW(Andn, 1, 0xDF, 0, kWideW0, false, false, false),

    ROW(Or, 1, 0xEB, 0, kWideW0, false, false, false),
    ROW(Xor, 1, 0xEF, 0, kWideW0, false, false, false),
    ROW(AddB, 1, 0xFC, 0, kWideW0, false, false, false),
    ROW(AddW, 1, 0xFD, 0, kWideW0, false, false, false),
    ROW(AddD, 1, 0xFE, 0, kWideW0, false, false, false),
    ROW(AddQ, 1, 0xD4, 0, kWideIgnored, false, false, false),
    ROW(SubB, 1, 0xF8, 0, kWideW0, false, false, false),
    ROW(SubW, 1, 0xF9, 0, kWideW0, false, false, false),
    ROW(SubD, 1, 0xFA, 0, kWideW0, false, false, false),
    ROW(SubQ, 1, 0xFB, 0, kWideIgnored, false, false, false),
    ROW(CmpEqB, 1, 0x74, 0, kWideW0, false, false, false),
    ROW(CmpEqW, 1, 0x75, 0, kWideW0, false, false, false),
    ROW(CmpEqD, 1, 0x76, 0, kWideW0, false, false, false),
    ROW(CmpEqQ, 2, 0x29, 0, kWideW0, false, false, false),
    ROW(CmpGtB, 1, 0x64, 0, kWideW0, false, false, false),
    ROW(CmpGtW, 1, 0x65, 0, kWideW0, false, false, false),
    ROW(CmpGtD, 1, 0x66, 0, kWideW0, false, false, false),
    ROW(CmpGtQ, 2, 0x37, 0, kWideW0, false, false, false),
    ROW(MinU, 2, 0x3B, 0, kWideW0, false, false, false),
    ROW(MaxU, 2, 0x3F, 0, kWideW0, false, false, false),
    ROW(MulUdq, 1, 0xF4, 0, kWideIgnored, false, false, false),
    ROW(MulD, 2, 0x40, 0, kWideW0, false, false, false),
    ROW(Sadbw, 1, 0xF6, 0, kWideW0, false, false, false),
    ROW(Shufb, 2, 0x00, 0, kWideW0, false, false, false),
    ROW(Shufd, 1, 0x70, 0, kWideW0, true, false, false),
    ROW(Alignr, 3, 0x0F, 0, kWideW0, true, false, false),
    ROW(Blendd, 3, 0x02, 0, kWideW0, true, true, false),

    // shape D: shift by imm8. The VEX encoding renumbers the legacy group, so
    // the digits below were measured with the assembler, not from the SDM
    // (VPSLLD is 0F 72 /6, VPSRLD /2, VPSRAD /4, VPSLLQ 0F 73 /6, ...).
    //
    // The qword shifts are WIG, so they must also match the two-byte VEX form that
    // carries no W field -- the emulator's own binary is full of `c5 f5 73 f1 ..`
    // (VPSRLQ xmm) and rejecting those left them as live AVX2 in the guest image.
    ROW(ShiftRImm, 1, 0x72, 2, kWideW0, true, false, false),
    ROW(ShiftRImmArith, 1, 0x72, 4, kWideW0, true, false, false),
    ROW(ShiftLImm, 1, 0x72, 6, kWideW0, true, false, false),
    ROW(ShiftLImm, 1, 0x72, 4, kWideIgnored, true, false, false),   // VPSLLQ xmm
    ROW(ShiftRImm, 1, 0x71, 2, kWideW0, true, false, false),
    ROW(ShiftLImm, 1, 0x71, 6, kWideW0, true, false, false),
    ROW(ShiftRImm, 1, 0x73, 2, kWideW1, true, false, false),
    ROW(ShiftRImm, 1, 0x73, 2, kWideIgnored, true, false, false),   // VPSRLQ xmm (WIG)
    ROW(ShiftLImm, 1, 0x73, 6, kWideW1, true, false, false),
    ROW(ShiftLImm, 1, 0x73, 6, kWideIgnored, true, false, false),   // VPSLLQ xmm (WIG)

    // shape C: broadcasts trap at 128 bits as well. Both the 0F38 58-5B
    // (vector source) and the 0F38 78-7B spellings are accepted, because
    // different toolchains pick different ones for the same instruction.
    ROW(BroadcastB, 2, 0x58, 0, kWideW0, false, true, false),
    ROW(BroadcastD, 2, 0x5A, 0, kWideW0, false, true, false),
    ROW(BroadcastQ, 2, 0x5B, 0, kWideW0, false, true, false),
    ROW(BroadcastB, 2, 0x78, 0, kWideW0, false, true, false),
    ROW(BroadcastW, 2, 0x79, 0, kWideW0, false, true, false),
    ROW(BroadcastD, 2, 0x7A, 0, kWideW0, false, true, false),
    ROW(BroadcastQ, 2, 0x7B, 0, kWideIgnored, false, true, false),
    // 0F38 59 is VPBROADCASTQ, both with a register and with an m64 source --
    // that is what GNU as and LLVM emit for vpbroadcastq. The SDM also lists
    // VPBROADCASTW at 0F38 59 for a register source, but the two would be
    // indistinguishable here and VPBROADCASTW's real encodings are 59-as-m16
    // (never produced) and 79, so 59 is decoded as the qword splat.
    ROW(BroadcastQ, 2, 0x59, 0, kWideIgnored, false, true, false),

    ROW(BroadcastSs, 2, 0x18, 0, kWideW0, false, true, true),
    ROW(BroadcastSd, 2, 0x19, 0, kWideW0, false, true, true),
};

#undef ROW

}

class Avx2InstructionMatcher : public IAvx2InstructionMatcher {
public:
    [[nodiscard]] std::optional<Avx2Match> Match(
        const std::uint8_t* data,
        std::size_t length
    ) const override;
};

std::optional<Avx2Match> Avx2InstructionMatcher::Match(const std::uint8_t* data, const std::size_t length) const {
    if (length < 4) {
        return std::nullopt;
    }
    if (data[0] != Vex2 && data[0] != Vex3) {
        return std::nullopt;
    }

    VexPrefix vex;
    if (!parseVex(data, length, vex) || vex.pos >= length) {
        return std::nullopt;
    }
    if (vex.pp != 1) {
        return std::nullopt;  // only the 0x66 form is used by these opcodes
    }
    if (vex.vvvv == 0 && vex.pos == 3) {
        // vvvv == 0 is legal, but a 0x66 three-operand form that decodes to
        // nothing useful is suspicious; still, it is not filtered here.
    }

    const std::uint8_t opcode = data[vex.pos];
    const std::size_t modrmPos = vex.pos + 1;
    if (modrmPos >= length) {
        return std::nullopt;
    }
    const std::uint8_t regField = static_cast<std::uint8_t>((data[modrmPos] >> 3) & 7);

    // Several rows share one opcode and are told apart by the /digit in
    // ModRM.reg (the 0x72 shift group has /2, /3 and /4), so the digit has to
    // take part in the lookup rather than being checked afterwards.
    const OpRow* row = nullptr;
    for (const auto& candidate : kRows) {
        if (candidate.Map != vex.map || candidate.Opcode != opcode) {
            continue;
        }
        // WIG rows accept either W value -- and the two-byte VEX form reports
        // W=0 because it has no W field, so requiring W=1 there would be wrong.
        const bool wideOk = candidate.Wide == WideReq::kWideIgnored ||
                            (candidate.Wide == WideReq::kWideW1 && vex.wide) ||
                            (candidate.Wide == WideReq::kWideW0 && !vex.wide);
        if (!wideOk) {
            continue;
        }
        if (candidate.Digits != 0 && candidate.Digits != regField) {
            continue;
        }
        row = &candidate;
        break;
    }
    if (row == nullptr) {
        return std::nullopt;
    }

    const Operands ops = decodeOperands(data, modrmPos, vex.r, vex.x, vex.b);
    if (row->RegSourceOnly && ops.isMem) {
        return std::nullopt;  // VBROADCASTSS/SD from memory is AVX1
    }
    if (!row->AnyL && !vex.ymm) {
        return std::nullopt;  // the 128-bit form runs fine on this CPU
    }

    std::size_t consumed = ops.after;
    Avx2Match match;
    if (row->NeedsImm) {
        if (consumed >= length) {
            return std::nullopt;
        }
        match.Imm8 = data[consumed];
        consumed += 1;
    }
    if (consumed != length) {
        return std::nullopt;
    }

    match.Op = row->Op;
    match.Length = length;
    match.UseYmm = vex.ymm;
    match.Map = vex.map;
    match.Pp = vex.pp;
    match.Opcode = opcode;
    match.Wide = vex.wide;
    match.Digits = row->Digits;
    match.Dst = ops.reg;
    match.Src1 = vex.vvvv;
    match.Src2 = ops.rm;
    match.IsMemory = ops.isMem;
    if (ops.isMem) {
        match.Mem = ops.mem;
    }
    return match;
}

std::unique_ptr<IAvx2InstructionMatcher> MakeAvx2InstructionMatcher() {
    return std::make_unique<Avx2InstructionMatcher>();
}

}
