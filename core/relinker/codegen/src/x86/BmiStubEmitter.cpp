#include <codegen/x86/IBmiStubEmitter.hpp>
#include <codegen/CodegenException.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

namespace Codegen {

namespace {

constexpr std::uint8_t kRsp = 4;
constexpr std::uint8_t kRbp = 5;
constexpr std::uint8_t kRcx = 1;
constexpr std::uint8_t kDispatchLength = 5;
constexpr std::uint8_t kSize64 = 64;
constexpr std::uint8_t kSize32 = 32;

constexpr std::uint8_t kPref[] = {0, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr std::size_t kPrefCount = sizeof(kPref) / sizeof(kPref[0]);

void appendRex(std::vector<std::uint8_t>& out, const bool w, const bool r, const bool x, const bool b) {
    if (!(w || r || x || b)) {
        return;
    }
    out.push_back(static_cast<std::uint8_t>(0x40 | (w << 3) | (r << 2) | (x << 1) | b));
}

void appendU32(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void appendModRm(std::vector<std::uint8_t>& out, const std::uint8_t mod, const std::uint8_t reg, const std::uint8_t rm) {
    out.push_back(static_cast<std::uint8_t>((mod << 6) | (reg << 3) | rm));
}

void appendPush(std::vector<std::uint8_t>& out, const std::uint8_t reg) {
    appendRex(out, false, false, false, reg > 7);
    out.push_back(static_cast<std::uint8_t>(0x50 + (reg & 7)));
}

void appendPop(std::vector<std::uint8_t>& out, const std::uint8_t reg) {
    appendRex(out, false, false, false, reg > 7);
    out.push_back(static_cast<std::uint8_t>(0x58 + (reg & 7)));
}

void appendMovRR(std::vector<std::uint8_t>& out, const std::uint8_t dst, const std::uint8_t src, const bool wide) {
    appendRex(out, wide, dst > 7, false, src > 7);
    out.push_back(0x8B);
    appendModRm(out, 3, dst & 7, src & 7);
}

void appendMovRmImm(std::vector<std::uint8_t>& out, const std::uint8_t dst, const std::int32_t value, const bool wide) {
    appendRex(out, wide, false, false, dst > 7);
    out.push_back(0xC7);
    appendModRm(out, 3, 0, dst & 7);
    appendU32(out, static_cast<std::uint32_t>(value));
}

void appendMovR8R8(std::vector<std::uint8_t>& out, const std::uint8_t dst8, const std::uint8_t src8) {
    appendRex(out, false, src8 > 7, false, dst8 > 7);
    out.push_back(0x88);
    appendModRm(out, 3, src8 & 7, dst8 & 7);
}

void appendShiftCl(std::vector<std::uint8_t>& out, const std::uint8_t reg, const std::uint8_t grp2, const bool wide) {
    appendRex(out, wide, false, false, reg > 7);
    out.push_back(0xD3);
    appendModRm(out, 3, grp2, reg & 7);
}

void appendShiftImm(std::vector<std::uint8_t>& out, const std::uint8_t reg, const std::uint8_t grp2, const std::uint8_t imm, const bool wide) {
    appendRex(out, wide, false, false, reg > 7);
    out.push_back(0xC1);
    appendModRm(out, 3, grp2, reg & 7);
    out.push_back(imm);
}

void appendRorImm(std::vector<std::uint8_t>& out, const std::uint8_t reg, const std::uint8_t imm, const bool wide) {
    appendShiftImm(out, reg, 1, imm, wide);
}

void appendTestRR(std::vector<std::uint8_t>& out, const std::uint8_t reg, const bool wide) {
    appendRex(out, wide, reg > 7, false, reg > 7);
    out.push_back(0x85);
    appendModRm(out, 3, reg & 7, reg & 7);
}

void appendCmpClImm(std::vector<std::uint8_t>& out, const std::uint8_t imm) {
    out.push_back(0x80);
    out.push_back(0xF9);
    out.push_back(imm);
}

void appendCmpRImm(std::vector<std::uint8_t>& out, const std::uint8_t reg, const std::uint8_t imm, const bool wide) {
    appendRex(out, wide, false, false, reg > 7);
    out.push_back(0x83);
    appendModRm(out, 3, 7, reg & 7);
    out.push_back(imm);
}

void appendBsrRRm(std::vector<std::uint8_t>& out, const std::uint8_t dst, const std::uint8_t src, const bool wide) {
    appendRex(out, wide, dst > 7, false, src > 7);
    out.push_back(0x0F);
    out.push_back(0xBD);
    appendModRm(out, 3, dst & 7, src & 7);
}

void appendBsfRRm(std::vector<std::uint8_t>& out, const std::uint8_t dst, const std::uint8_t src, const bool wide) {
    appendRex(out, wide, dst > 7, false, src > 7);
    out.push_back(0x0F);
    out.push_back(0xBC);
    appendModRm(out, 3, dst & 7, src & 7);
}

void appendNotR(std::vector<std::uint8_t>& out, const std::uint8_t reg, const bool wide) {
    appendRex(out, wide, false, false, reg > 7);
    out.push_back(0xF7);
    appendModRm(out, 3, 2, reg & 7);
}

void appendAndRR(std::vector<std::uint8_t>& out, const std::uint8_t dst, const std::uint8_t src, const bool wide) {
    appendRex(out, wide, dst > 7, false, src > 7);
    out.push_back(0x23);
    appendModRm(out, 3, dst & 7, src & 7);
}

void appendXorRR(std::vector<std::uint8_t>& out, const std::uint8_t dst, const std::uint8_t src, const bool wide) {
    appendRex(out, wide, dst > 7, false, src > 7);
    out.push_back(0x33);
    appendModRm(out, 3, dst & 7, src & 7);
}

void appendSubRmR(std::vector<std::uint8_t>& out, const std::uint8_t rm, const std::uint8_t src, const bool wide) {
    appendRex(out, wide, src > 7, false, rm > 7);
    out.push_back(0x29);
    appendModRm(out, 3, src & 7, rm & 7);
}

void appendJcc8(std::vector<std::uint8_t>& out, const std::uint8_t cc, const std::uint8_t disp) {
    out.push_back(static_cast<std::uint8_t>(0x70 + cc));
    out.push_back(disp);
}

void appendJmp8(std::vector<std::uint8_t>& out, const std::uint8_t disp) {
    out.push_back(0xEB);
    out.push_back(disp);
}

// Port of bmi_proto.py mov_r_mem. The RIP-relative path carries the raw
// displacement (the eboot contains no RIP-relative BMI sources).
void appendMovRm(std::vector<std::uint8_t>& out, const std::uint8_t dst, const BmiMemOperand& mem, const bool wide) {
    const bool rexB = (mem.Base != -1 && mem.Base >= 8);
    const bool rexX = (mem.Index != -1 && mem.Index >= 8);
    appendRex(out, wide, dst > 7, rexX, rexB);
    out.push_back(0x8B);

    if (mem.Rip) {
        out.push_back(0x05);
        appendU32(out, static_cast<std::uint32_t>(mem.Disp));
        return;
    }

    std::uint8_t mod = mem.Mod;
    if (mod == 1 && !(-128 <= mem.Disp && mem.Disp <= 127)) {
        mod = 2;
    }

    if (mem.HasSib) {
        const std::uint8_t indexCode = (mem.IndexCode != -1) ? static_cast<std::uint8_t>(mem.IndexCode) : 4;
        const std::uint8_t baseCode = (mem.BaseCode != -1) ? static_cast<std::uint8_t>(mem.BaseCode) : 5;
        std::uint8_t scaleBits = 0;
        if (mem.Scale == 2) scaleBits = 1;
        else if (mem.Scale == 4) scaleBits = 2;
        else if (mem.Scale == 8) scaleBits = 3;
        appendModRm(out, mod, dst & 7, 4);
        out.push_back(static_cast<std::uint8_t>((scaleBits << 6) | (indexCode << 3) | baseCode));
    } else {
        appendModRm(out, mod, dst & 7, static_cast<std::uint8_t>(mem.BaseCode & 7));
    }

    if (mod == 1) {
        out.push_back(static_cast<std::uint8_t>(mem.Disp & 0xFF));
    } else if (mod == 2 || (mod == 0 && mem.BaseCode == -1)) {
        appendU32(out, static_cast<std::uint32_t>(mem.Disp));
    }
}

std::set<int> excluded(const BmiMatch& match, const std::vector<int>& more) {
    std::set<int> set{kRsp, kRbp, kRcx};
    if (match.IsMemory) {
        if (match.Mem.Base != -1) set.insert(match.Mem.Base);
        if (match.Mem.Index != -1) set.insert(match.Mem.Index);
    }
    for (const int reg : more) set.insert(reg);
    return set;
}

int pickT1(const BmiMatch& match, const std::vector<int>& more) {
    const std::set<int> set = excluded(match, more);
    for (std::size_t i = 0; i < kPrefCount; ++i) {
        if (set.find(kPref[i]) == set.end()) {
            return kPref[i];
        }
    }
    throw CodegenException("No free scratch register for BMI stub");
}

int pickT2(const BmiMatch& match, const int t1) {
    for (std::size_t i = 0; i < kPrefCount; ++i) {
        const int reg = kPref[i];
        if (reg == t1 || reg == kRcx || reg == kRsp || reg == kRbp || reg == match.Dst || reg == match.Third) {
            continue;
        }
        return reg;
    }
    throw CodegenException("No free second scratch register for bextr stub");
}

void appendMemLoad(std::vector<std::uint8_t>& out, const std::uint8_t dst, const BmiMatch& match) {
    appendMovRm(out, dst, match.Mem, match.Wide);
}

}

class BmiStubEmitter : public IBmiStubEmitter {
public:
    [[nodiscard]] std::vector<std::uint8_t> EmitTemplate(const BmiMatch& match) const override;

    [[nodiscard]] std::vector<std::uint8_t> EmitDispatch(const BmiMatch& match, std::size_t& rel32Offset) const override;

    [[nodiscard]] std::vector<std::uint8_t> EmitShort(const BmiMatch& match) const override;
};

std::vector<std::uint8_t> BmiStubEmitter::EmitTemplate(const BmiMatch& match) const {
    const BmiOp op = match.Op;
    const bool wide = match.Wide;
    const std::uint8_t dst = match.Dst;
    const std::uint8_t src = match.Src;
    const std::uint8_t ctrl = match.Third;
    const bool isMem = match.IsMemory;

    std::vector<std::uint8_t> out;

    if (op == BmiOp::Rorx) {
        int t1 = -1;
        if (!isMem && src != dst) {
            t1 = pickT1(match, {src, dst});
        }
        out.push_back(0x9C); // pushfq
        if (isMem) {
            appendMemLoad(out, dst, match);
            if (match.Mem.Base == dst || match.Mem.Index == dst) {
                throw CodegenException("rorx destination collides with memory base/index");
            }
        } else {
            if (t1 != -1) {
                appendPush(out, static_cast<std::uint8_t>(t1));
                appendMovRR(out, static_cast<std::uint8_t>(t1), src, wide);
            }
        }
        appendRorImm(out, static_cast<std::uint8_t>((!isMem && src != dst) ? t1 : dst), match.Imm8, wide);
        if (!isMem && src != dst) {
            appendMovRR(out, dst, static_cast<std::uint8_t>(t1), wide);
        }
        if (t1 != -1) {
            appendPop(out, static_cast<std::uint8_t>(t1));
        }
        out.push_back(0x9D); // popfq
        return out;
    }

    if (op == BmiOp::Shlx || op == BmiOp::Shrx || op == BmiOp::Sarx) {
        const std::uint8_t grp2 = (op == BmiOp::Shlx) ? 4 : (op == BmiOp::Shrx) ? 5 : 7;
        const int t1 = pickT1(match, {dst, src, ctrl});
        const bool needRcx = (ctrl != kRcx);
        out.push_back(0x9C); // pushfq
        if (needRcx) {
            appendPush(out, kRcx);
        }
        appendPush(out, static_cast<std::uint8_t>(t1));
        if (isMem) {
            appendMemLoad(out, static_cast<std::uint8_t>(t1), match);
        } else {
            appendMovRR(out, static_cast<std::uint8_t>(t1), src, wide);
        }
        if (needRcx) {
            appendMovRR(out, kRcx, ctrl, false);
        }
        appendShiftCl(out, static_cast<std::uint8_t>(t1), grp2, wide);
        appendMovRR(out, dst, static_cast<std::uint8_t>(t1), wide);
        appendPop(out, static_cast<std::uint8_t>(t1));
        if (needRcx) {
            appendPop(out, kRcx);
        }
        out.push_back(0x9D); // popfq
        return out;
    }

    if (op == BmiOp::Andn) {
        const int t1 = pickT1(match, {dst, src, ctrl});
        appendPush(out, static_cast<std::uint8_t>(t1));
        if (isMem) {
            appendMemLoad(out, static_cast<std::uint8_t>(t1), match);
        } else {
            appendMovRR(out, static_cast<std::uint8_t>(t1), src, wide);
        }
        appendNotR(out, static_cast<std::uint8_t>(t1), wide);
        appendAndRR(out, static_cast<std::uint8_t>(t1), ctrl, wide);
        appendMovRR(out, dst, static_cast<std::uint8_t>(t1), wide);
        appendTestRR(out, dst, wide);
        appendPop(out, static_cast<std::uint8_t>(t1));
        return out;
    }

    if (op == BmiOp::Bzhi) {
        const int t1 = pickT1(match, {dst, src, ctrl});
        const bool needRcx = (ctrl != kRcx);
        appendPush(out, static_cast<std::uint8_t>(t1));
        if (isMem) {
            appendMemLoad(out, static_cast<std::uint8_t>(t1), match);
        } else {
            appendMovRR(out, static_cast<std::uint8_t>(t1), src, wide);
        }
        if (needRcx) {
            appendPush(out, kRcx);
            appendMovRR(out, kRcx, ctrl, false);
        }
        appendCmpRImm(out, kRcx, wide ? kSize64 : kSize32, false);
        appendJcc8(out, 3, 0); // jae
        const std::size_t jae1 = out.size() - 1;

        appendMovRmImm(out, dst, -1, wide);
        appendShiftCl(out, dst, 4, wide);
        appendNotR(out, dst, wide);
        appendAndRR(out, dst, static_cast<std::uint8_t>(t1), wide);
        appendTestRR(out, dst, wide);
        appendJmp8(out, 0);
        const std::size_t jmpa = out.size() - 1;

        const std::size_t alla = out.size();
        appendMovRR(out, dst, static_cast<std::uint8_t>(t1), wide);
        appendTestRR(out, dst, wide);
        out.push_back(0xF9); // stc
        const std::size_t done = out.size();

        out[jae1] = static_cast<std::uint8_t>((alla - (jae1 + 1)) & 0xFF);
        out[jmpa] = static_cast<std::uint8_t>((done - (jmpa + 1)) & 0xFF);
        if (needRcx) {
            appendPop(out, kRcx);
        }
        appendPop(out, static_cast<std::uint8_t>(t1));
        return out;
    }

    if (op == BmiOp::Bextr) {
        const int t1 = pickT1(match, {dst, src, ctrl});
        const int t2 = pickT2(match, t1);
        appendPush(out, static_cast<std::uint8_t>(t2));
        appendPush(out, static_cast<std::uint8_t>(t1));
        appendPush(out, kRcx);
        if (isMem) {
            appendMemLoad(out, static_cast<std::uint8_t>(t1), match);
        } else {
            appendMovRR(out, static_cast<std::uint8_t>(t1), src, wide);
        }
        if (ctrl != kRcx) {
            appendMovRR(out, kRcx, ctrl, false);
        }
        appendMovRR(out, static_cast<std::uint8_t>(t2), kRcx, false);
        appendShiftImm(out, static_cast<std::uint8_t>(t2), 5, 8, false);
        appendCmpClImm(out, wide ? kSize64 : kSize32);
        appendJcc8(out, 3, 0); // jae
        const std::size_t ja1 = out.size() - 1;

        appendTestRR(out, static_cast<std::uint8_t>(t2), false);
        appendJcc8(out, 4, 0); // je
        const std::size_t je1 = out.size() - 1;

        appendShiftCl(out, static_cast<std::uint8_t>(t1), 5, wide);
        appendCmpRImm(out, static_cast<std::uint8_t>(t2), wide ? kSize64 : kSize32, false);
        appendJcc8(out, 3, 0); // jae
        const std::size_t ja2 = out.size() - 1;

        appendMovR8R8(out, kRcx, static_cast<std::uint8_t>(t2));
        appendMovRmImm(out, dst, 1, wide);
        appendShiftCl(out, dst, 4, wide);
        appendRex(out, wide, false, false, dst > 7);
        out.push_back(0x83);
        appendModRm(out, 3, 5, dst & 7);
        out.push_back(1);
        appendAndRR(out, dst, static_cast<std::uint8_t>(t1), wide);
        appendJmp8(out, 0);
        const std::size_t jmpa = out.size() - 1;

        const std::size_t alla = out.size();
        appendMovRR(out, dst, static_cast<std::uint8_t>(t1), wide);
        appendJmp8(out, 0);
        const std::size_t jmpb = out.size() - 1;

        const std::size_t zer = out.size();
        appendXorRR(out, dst, dst, wide);
        const std::size_t done = out.size();
        appendTestRR(out, dst, wide);

        out[ja1] = static_cast<std::uint8_t>((zer - (ja1 + 1)) & 0xFF);
        out[je1] = static_cast<std::uint8_t>((zer - (je1 + 1)) & 0xFF);
        out[ja2] = static_cast<std::uint8_t>((alla - (ja2 + 1)) & 0xFF);
        out[jmpa] = static_cast<std::uint8_t>((done - (jmpa + 1)) & 0xFF);
        out[jmpb] = static_cast<std::uint8_t>((done - (jmpb + 1)) & 0xFF);
        appendPop(out, kRcx);
        appendPop(out, static_cast<std::uint8_t>(t1));
        appendPop(out, static_cast<std::uint8_t>(t2));
        return out;
    }

    if (op == BmiOp::Lzcnt || op == BmiOp::Tzcnt) {
        const int t1 = pickT1(match, {dst, src});
        appendPush(out, static_cast<std::uint8_t>(t1));
        if (isMem) {
            appendMemLoad(out, static_cast<std::uint8_t>(t1), match);
        } else {
            appendMovRR(out, static_cast<std::uint8_t>(t1), src, wide);
        }
        appendTestRR(out, static_cast<std::uint8_t>(t1), wide);
        appendJcc8(out, 5, 0); // jne
        const std::size_t jne = out.size() - 1;

        appendMovRmImm(out, dst, wide ? kSize64 : kSize32, wide);
        appendTestRR(out, dst, wide);
        out.push_back(0xF9); // stc
        appendJmp8(out, 0);
        const std::size_t jmpa = out.size() - 1;

        const std::size_t nz = out.size();
        if (op == BmiOp::Lzcnt) {
            appendBsrRRm(out, dst, static_cast<std::uint8_t>(t1), wide);
            appendMovRmImm(out, static_cast<std::uint8_t>(t1), wide ? 63 : 31, wide);
            appendSubRmR(out, static_cast<std::uint8_t>(t1), dst, wide);
            appendMovRR(out, dst, static_cast<std::uint8_t>(t1), wide);
        } else {
            appendBsfRRm(out, dst, static_cast<std::uint8_t>(t1), wide);
        }
        appendTestRR(out, dst, wide);
        const std::size_t fin = out.size();

        out[jne] = static_cast<std::uint8_t>((nz - (jne + 1)) & 0xFF);
        out[jmpa] = static_cast<std::uint8_t>((fin - (jmpa + 1)) & 0xFF);
        appendPop(out, static_cast<std::uint8_t>(t1));
        return out;
    }

    throw CodegenException("Unhandled BMI opcode in stub emitter");
}

std::vector<std::uint8_t> BmiStubEmitter::EmitDispatch(const BmiMatch& match, std::size_t& rel32Offset) const {
    std::vector<std::uint8_t> out = EmitTemplate(match);
    out.push_back(0xE9);
    rel32Offset = out.size();
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    return out;
}

std::vector<std::uint8_t> BmiStubEmitter::EmitShort(const BmiMatch& match) const {
    std::vector<std::uint8_t> out = EmitTemplate(match);
    out.push_back(0xC3);
    return out;
}

std::unique_ptr<IBmiStubEmitter> MakeBmiStubEmitter() {
    return std::make_unique<BmiStubEmitter>();
}

}