#include <codegen/x86/IBmiInstructionMatcher.hpp>
#include <cstring>
#include <memory>

namespace Codegen {

namespace {

constexpr std::uint8_t TwoByteEscape = 0x0F;
constexpr std::uint8_t LegacyBc = 0xBC; // tzcnt
constexpr std::uint8_t LegacyBd = 0xBD; // lzcnt

// Mirror of the validated Python decoder (bmi_proto.py decode_operands,
// became gen_vectors.py's source of truth and validated natively at 1163/1163).
struct DecodedOperands {
    std::uint8_t Dst = 0;
    std::uint8_t Src = 0;   // 0 for memory forms
    std::size_t PAfter = 0; // absolute byte offset just past the operand
    BmiMemOperand Mem;
};

std::int8_t readI8(const std::uint8_t* data) {
    std::int8_t value{};
    std::memcpy(&value, data, 1);
    return value;
}

std::int32_t readI32(const std::uint8_t* data) {
    std::int32_t value{};
    std::memcpy(&value, data, 4);
    return value;
}

DecodedOperands decodeOperands(const std::uint8_t* data, const std::size_t pos, const bool isLegacy, const std::uint8_t legRex) {
    const std::uint8_t mr = data[pos];
    const auto mod = static_cast<std::uint8_t>(mr >> 6);
    const auto reg = static_cast<std::uint8_t>((mr >> 3) & 7);
    const auto rm = static_cast<std::uint8_t>(mr & 7);

    const unsigned r = isLegacy ? ((legRex >> 2) & 1) : 0;
    const unsigned b = isLegacy ? (legRex & 1) : 0;
    const unsigned x = isLegacy ? ((legRex >> 1) & 1) : 0;

    DecodedOperands out;
    out.Mem.Mod = mod;
    out.PAfter = pos + 1;
    out.Dst = static_cast<std::uint8_t>(reg | (r << 3));

    if (mod == 3) {
        out.Src = static_cast<std::uint8_t>(rm | (b << 3));
        return out;
    }

    std::size_t p = pos + 1;
    if (rm == 4) {
        const std::uint8_t sib = data[p];
        ++p;
        out.Mem.HasSib = true;
        out.Mem.Scale = static_cast<std::uint8_t>(1u << (sib >> 6));
        const auto indexCode = static_cast<std::uint8_t>((sib >> 3) & 7);
        const auto baseCode = static_cast<std::uint8_t>(sib & 7);
        if (indexCode != 4) {
            out.Mem.Index = indexCode | (x << 3);
            out.Mem.IndexCode = indexCode;
        }
        if (mod == 0 && baseCode == 5) {
            out.Mem.Disp = readI32(data + p);
            p += 4;
        } else {
            out.Mem.Base = baseCode | (b << 3);
            out.Mem.BaseCode = baseCode;
        }
        if (mod == 1) {
            out.Mem.Disp = readI8(data + p);
            p += 1;
        } else if (mod == 2) {
            out.Mem.Disp = readI32(data + p);
            p += 4;
        }
    } else if (mod == 0 && rm == 5) {
        out.Mem.Rip = true;
        out.Mem.Disp = readI32(data + p);
        p += 4;
    } else if (mod == 1) {
        out.Mem.Base = rm | (b << 3);
        out.Mem.BaseCode = rm;
        out.Mem.Disp = readI8(data + p);
        p += 1;
    } else if (mod == 2) {
        out.Mem.Base = rm | (b << 3);
        out.Mem.BaseCode = rm;
        out.Mem.Disp = readI32(data + p);
        p += 4;
    } else {
        out.Mem.Base = rm | (b << 3);
        out.Mem.BaseCode = rm;
    }

    out.PAfter = p;
    return out;
}

}

class BmiInstructionMatcher : public IBmiInstructionMatcher {
public:
    [[nodiscard]] std::optional<BmiMatch> Match(
        const std::uint8_t* data,
        std::size_t length
    ) const override;
};

std::optional<BmiMatch> BmiInstructionMatcher::Match(const std::uint8_t* data, const std::size_t length) const {
    if (length < 4) {
        return std::nullopt;
    }

    BmiMatch match;
    match.Length = length;

    if (data[0] == 0xF3) {
        // Legacy F3 [REX] 0F BC/BD r32/r64, r/m32/64
        std::size_t p = 1;
        std::uint8_t legRex = 0;
        if (p < length && data[p] >= 0x40 && data[p] <= 0x4F) {
            legRex = data[p];
            ++p;
        }
        if (p + 2 > length || data[p] != TwoByteEscape) {
            return std::nullopt;
        }
        const std::uint8_t opcode = data[p + 1];
        if (opcode != LegacyBc && opcode != LegacyBd) {
            return std::nullopt;
        }
        p += 2;
        if (p >= length) {
            return std::nullopt;
        }
        const DecodedOperands decoded = decodeOperands(data, p, true, legRex);
        if (decoded.PAfter != length) {
            return std::nullopt;
        }
        match.Op = (opcode == LegacyBc) ? BmiOp::Tzcnt : BmiOp::Lzcnt;
        match.Wide = (legRex & 8) != 0;
        match.Dst = decoded.Dst;
        match.IsMemory = decoded.Mem.Mod != 3;
        if (match.IsMemory) {
            match.Mem = decoded.Mem;
        } else {
            match.Src = decoded.Src;
        }
        match.IsShort = (match.Length == 4);
    } else if (data[0] == 0xC4) {
        // VEX3 C4 lb1 lb2 opc [modrm [sib [disp]] [imm8]]
        if (length < 5) {
            return std::nullopt;
        }
        const std::uint8_t b1 = data[1];
        const std::uint8_t b2 = data[2];
        const auto map = static_cast<std::uint8_t>(b1 & 0x1F);
        const unsigned r = static_cast<unsigned>(((b1 >> 7) & 1) ^ 1);
        const unsigned b = static_cast<unsigned>(((b1 >> 5) & 1) ^ 1);
        const unsigned x = static_cast<unsigned>(((b1 >> 6) & 1) ^ 1);
        const bool wide = (b2 >> 7) != 0;
        const auto vvvv = static_cast<std::uint8_t>(((b2 >> 3) & 0x0F) ^ 0x0F);
        const auto pp = static_cast<std::uint8_t>(b2 & 3);
        const std::uint8_t opcode = data[3];

        if (map == 2 && opcode == 0xF7) {
            switch (pp) {
                case 1: match.Op = BmiOp::Shlx; break;
                case 2: match.Op = BmiOp::Sarx; break;
                case 3: match.Op = BmiOp::Shrx; break;
                case 0: match.Op = BmiOp::Bextr; break;
                default: return std::nullopt;
            }
        } else if (map == 2 && opcode == 0xF5 && pp == 0) {
            match.Op = BmiOp::Bzhi;
        } else if (map == 2 && opcode == 0xF2 && pp == 0) {
            match.Op = BmiOp::Andn;
        } else if (map == 0x03 && opcode == 0xF0 && pp == 3) {
            match.Op = BmiOp::Rorx;
        } else {
            return std::nullopt;
        }

        const DecodedOperands decoded = decodeOperands(data, 4, false, 0);
        match.Wide = wide;
        match.Dst = static_cast<std::uint8_t>((decoded.Dst & 7) | (r << 3));
        match.Third = vvvv;
        match.IsMemory = decoded.Mem.Mod != 3;
        if (match.IsMemory) {
            match.Mem = decoded.Mem;
            if (match.Mem.Base != -1) {
                match.Mem.Base = (match.Mem.Base & 7) | (b << 3);
                match.Mem.BaseCode = match.Mem.Base & 7;
            }
            if (match.Mem.Index != -1) {
                match.Mem.Index = (match.Mem.Index & 7) | (x << 3);
                match.Mem.IndexCode = match.Mem.Index & 7;
            }
        } else {
            match.Src = static_cast<std::uint8_t>((decoded.Src & 7) | (b << 3));
        }

        std::size_t consumed = decoded.PAfter;
        if (match.Op == BmiOp::Rorx) {
            if (consumed >= length) {
                return std::nullopt;
            }
            match.Imm8 = data[consumed];
            consumed += 1;
        }
        if (consumed != length) {
            return std::nullopt;
        }
        match.IsShort = false;
    } else {
        return std::nullopt;
    }

    return match;
}

std::unique_ptr<IBmiInstructionMatcher> MakeBmiInstructionMatcher() {
    return std::make_unique<BmiInstructionMatcher>();
}

}