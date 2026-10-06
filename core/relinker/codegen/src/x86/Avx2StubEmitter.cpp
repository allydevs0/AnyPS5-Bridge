#include <codegen/x86/IAvx2StubEmitter.hpp>
#include <codegen/CodegenException.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

namespace Codegen {

namespace {

// Scratch GPRs, in preference order. rB becomes the frame base; it must not
// alias a memory operand's base or index, because the frame overlaps the very
// address the original instruction reads.
//
// rsp (4) and rbp (5) are deliberately absent. Both were once the last-resort
// fallback, and both are wrong: a 128-byte frame based on rsp is fine in
// isolation, but the stub already owns the stack below rsp for its own pushes,
// while a frame based on rbp writes straight into the guest's own stack frame and
// silently destroys its locals. If every entry here is busy the instruction needs
// a different strategy, not a corrupted frame -- so run out deliberately.
constexpr int kGprPref[] = {10, 11, 8, 9, 12, 13, 14, 15, 3, 2, 1, 0};
constexpr std::size_t kGprPrefCount = sizeof(kGprPref) / sizeof(kGprPref[0]);

// slot 0..31 -> src1, 32..63 -> src2, 64..95 -> low result, 96..127 -> high
// result and blend mask.
//
// The frame is addressed with disp8, so it must stay within 128 bytes: slot(0)
// is -128 and slot(127) is -1. Raising kFrame pushed slot(0) past -128, where it
// wraps into a positive disp8 and every frame access silently reads the wrong
// address. Scratch ymm registers are therefore saved on the stack, not here.
constexpr std::size_t kFrame = 128;

// Stack bytes reserved for the scratch ymm registers and the blend mask:
// two saved 256-bit registers plus a 128-bit mask, rounded up to keep rsp aligned.
constexpr std::size_t kScratchVec = 96;

// rsp as a ModRM base: 100b selects a SIB, which emitStackStore/Load handle.
constexpr int kScratchReg = 4;

void emitSubRsp(std::vector<std::uint8_t>& out, const std::uint32_t amount) {
    if (amount < 128) {
        out.push_back(0x48); out.push_back(0x83); out.push_back(0xEC);
        out.push_back(static_cast<std::uint8_t>(amount));
        return;
    }
    out.push_back(0x48); out.push_back(0x81); out.push_back(0xEC);
    out.push_back(static_cast<std::uint8_t>(amount));
    out.push_back(static_cast<std::uint8_t>(amount >> 8));
    out.push_back(static_cast<std::uint8_t>(amount >> 16));
    out.push_back(static_cast<std::uint8_t>(amount >> 24));
}

void emitAddRsp(std::vector<std::uint8_t>& out, const std::uint32_t amount) {
    if (amount < 128) {
        out.push_back(0x48); out.push_back(0x83); out.push_back(0xC4);
        out.push_back(static_cast<std::uint8_t>(amount));
        return;
    }
    out.push_back(0x48); out.push_back(0x81); out.push_back(0xC4);
    out.push_back(static_cast<std::uint8_t>(amount));
    out.push_back(static_cast<std::uint8_t>(amount >> 8));
    out.push_back(static_cast<std::uint8_t>(amount >> 16));
    out.push_back(static_cast<std::uint8_t>(amount >> 24));
}

// VEX.pp for the frame's own 128/256-bit moves. Opcode 0x6F/0x7F is VMOVQUA
// under pp=01 and VMOVDQUA under pp=10, so pp=2 is the one that tolerates any
// alignment -- which matters because the frame inherits the caller's rsp and is
// therefore only 16-byte aligned about half the time.
constexpr std::uint8_t kFramePp = 2;

std::uint8_t vexB1(const std::uint8_t map, const unsigned r, const unsigned x, const unsigned b) {
    return static_cast<std::uint8_t>((((r & 1) ^ 1) << 7) | (((x & 1) ^ 1) << 6) | (((b & 1) ^ 1) << 5) | (map & 0x1F));
}

std::uint8_t vexB2(const bool w, const std::uint8_t vvvv, const bool l, const std::uint8_t pp) {
    // The encoded VEX.vvvv field is the complement of the register number, so a
    // "no vvvv operand" instruction passes 0 here, not 0x0F.
    return static_cast<std::uint8_t>((w ? 0x80 : 0) | (((vvvv & 0x0F) ^ 0x0F) << 3) | (l ? 0x04 : 0) | (pp & 3));
}

void appendU32(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void appendPush(std::vector<std::uint8_t>& out, const int reg) {
    if (reg >= 8) {
        out.push_back(0x41);
    }
    out.push_back(static_cast<std::uint8_t>(0x50 + (reg & 7)));
}

void appendPop(std::vector<std::uint8_t>& out, const int reg) {
    if (reg >= 8) {
        out.push_back(0x41);
    }
    out.push_back(static_cast<std::uint8_t>(0x58 + (reg & 7)));
}

// VEX.<L>.<pp> opcode reg, rm   (mod=3)
void emitNdsReg(std::vector<std::uint8_t>& out, const std::uint8_t map, const std::uint8_t pp, const bool w,
                const bool l, const std::uint8_t opcode, const std::uint8_t vvvv, const std::uint8_t dst, const std::uint8_t rm) {
    out.push_back(0xC4);
    out.push_back(vexB1(map, dst >> 3, 0, rm >> 3));
    out.push_back(vexB2(w, vvvv, l, pp));
    out.push_back(opcode);
    out.push_back(static_cast<std::uint8_t>(0xC0 | ((dst & 7) << 3) | (rm & 7)));
}

// VEX.<L>.<pp> opcode digit, rm, imm8   -- the "NDD" shape used by the shift
// group and vpshufd: the destination is VEX.vvvv, ModRM.rm is the source and
// ModRM.reg is the /digit. REX.R must stay 0 because /digit is never extended.
void emitNddImm(std::vector<std::uint8_t>& out, const std::uint8_t map, const std::uint8_t pp, const bool w,
                const std::uint8_t opcode, const std::uint8_t digit, const std::uint8_t dstVvvv, const std::uint8_t srcRm, const std::uint8_t imm) {
    out.push_back(0xC4);
    out.push_back(vexB1(map, 0, 0, srcRm >> 3));
    out.push_back(vexB2(w, dstVvvv, false, pp));
    out.push_back(opcode);
    out.push_back(static_cast<std::uint8_t>(0xC0 | ((digit & 7) << 3) | (srcRm & 7)));
    out.push_back(imm);
}

// Legacy (non-VEX) SSE2 66 0F <opcode> /r -- the same operation as emitNdsReg with
// the VEX prefix dropped.
//
// Why legacy: the target CPU (i5-3570K, verified by CPUID) has AVX but not AVX2,
// so a VEX prefix itself is harmless. These particular *opcodes* are SSE2
// (punpcklbw, punpcklwd, pshufd, unpcklpd, shufps), so using the SSE2 form costs
// nothing and keeps the stub runnable on a host with neither AVX nor AVX2 -- which
// matters because the emulator also has to run under emulators and debuggers that
// lack AVX. The *original* AVX2 instruction traps because its opcode is AVX2-only,
// not because it carries a VEX prefix.
//
// Legacy SSE is 2-operand and destructive, so the destination must already hold the
// first source; a VEX form would allow dst != src1.
//
//   op(reg, src) -> reg = op(reg, src), with `reg` preloaded with src1.
void emitSseReg(std::vector<std::uint8_t>& out, const std::uint8_t opcode,
                const std::uint8_t reg, const std::uint8_t src) {
    out.push_back(0x66);
    out.push_back(0x0F);
    out.push_back(opcode);
    const auto rm = static_cast<std::uint8_t>(src & 7);
    out.push_back(static_cast<std::uint8_t>(0xC0 | ((reg & 7) << 3) | rm));
}

// Legacy 66 0F <opcode> /r <imm8> -- PSHUFD and friends.
void emitSseRegImm(std::vector<std::uint8_t>& out, const std::uint8_t opcode,
                   const std::uint8_t reg, const std::uint8_t src, const std::uint8_t imm) {
    emitSseReg(out, opcode, reg, src);
    out.push_back(imm);
}

// Legacy SSE with the opcode *map* kept, not just the opcode byte.
//
// A VEX instruction carries its map in the mmmmm field (1 = 0F, 2 = 0F38,
// 3 = 0F3A) and the opcode byte separately. Reusing only the opcode byte
// silently turns an escape-mapped instruction into an unrelated one from the
// base 0F map: VPSHUFB (0F38 00) becomes `0F 00 /r`, which is SLDT -- a
// completely different instruction that zero-extends a 16-bit value into a
// guest register. Both the map and the opcode have to be carried over.
enum class SseMap : std::uint8_t { k0F = 1, k0F38 = 2, k0F3A = 3 };

void emitSseMapReg(std::vector<std::uint8_t>& out, const SseMap map,
                   const std::uint8_t opcode, const std::uint8_t reg,
                   const std::uint8_t src) {
    out.push_back(0x66);
    out.push_back(0x0F);
    if (map != SseMap::k0F) {
        // The map value is chosen to encode the escape byte directly: 2 -> 0x38,
        // 3 -> 0x3A.
        out.push_back(static_cast<std::uint8_t>(0x38 * static_cast<int>(map)));
    }
    out.push_back(opcode);
    const auto rm = static_cast<std::uint8_t>(src & 7);
    out.push_back(static_cast<std::uint8_t>(0xC0 | ((reg & 7) << 3) | rm));
}

void emitSseMapRegImm(std::vector<std::uint8_t>& out, const SseMap map,
                      const std::uint8_t opcode, const std::uint8_t reg,
                      const std::uint8_t src, const std::uint8_t imm) {
    emitSseMapReg(out, map, opcode, reg, src);
    out.push_back(imm);
}

// The legacy SSE instruction that implements an elementwise AVX2 op, with its
// map. Deriving this from the Op rather than reusing match.Opcode is deliberate:
// it names the substitute explicitly and makes the one op whose encoding moved
// (VPABSD is 0F38 1E under VEX but PABSD is 0F38 1C) impossible to miss.
//
// Returns false for ops with no elementwise legacy equivalent, so the caller
// refuses them instead of emitting something unrelated.
bool legacyBinarySse(const Avx2Op op, SseMap& map, std::uint8_t& opcode, bool& imm8) {
    imm8 = false;
    switch (op) {
        // ---- map 1: the base 0F map ----
        case Avx2Op::And:      map = SseMap::k0F; opcode = 0xDB; return true;   // pand
        case Avx2Op::Andn:     map = SseMap::k0F; opcode = 0xDB; return true;   // pandn
        case Avx2Op::Or:       map = SseMap::k0F; opcode = 0xEB; return true;   // por
        case Avx2Op::Xor:      map = SseMap::k0F; opcode = 0xEF; return true;   // pxor
        case Avx2Op::AddB:     map = SseMap::k0F; opcode = 0xFC; return true;   // paddb
        case Avx2Op::AddW:     map = SseMap::k0F; opcode = 0xFD; return true;   // paddw
        case Avx2Op::AddD:     map = SseMap::k0F; opcode = 0xFE; return true;   // paddd
        case Avx2Op::AddQ:     map = SseMap::k0F; opcode = 0xD4; return true;   // paddq
        case Avx2Op::SubB:     map = SseMap::k0F; opcode = 0xF8; return true;   // psubb
        case Avx2Op::SubW:     map = SseMap::k0F; opcode = 0xF9; return true;   // psubw
        case Avx2Op::SubD:     map = SseMap::k0F; opcode = 0xFA; return true;   // psubd
        case Avx2Op::SubQ:     map = SseMap::k0F; opcode = 0xFB; return true;   // psubq
        case Avx2Op::CmpEqB:   map = SseMap::k0F; opcode = 0x74; return true;   // pcmpeqb
        case Avx2Op::CmpEqW:   map = SseMap::k0F; opcode = 0x75; return true;   // pcmpeqw
        case Avx2Op::CmpEqD:   map = SseMap::k0F; opcode = 0x76; return true;   // pcmpeqd
        case Avx2Op::CmpEqQ:   map = SseMap::k0F; opcode = 0x76; return true;   // pcmpeqq
        case Avx2Op::CmpGtB:   map = SseMap::k0F; opcode = 0x64; return true;   // pcmpgtb
        case Avx2Op::CmpGtW:   map = SseMap::k0F; opcode = 0x65; return true;   // pcmpgtw
        case Avx2Op::CmpGtD:   map = SseMap::k0F; opcode = 0x66; return true;   // pcmpgtd
        case Avx2Op::MulUdq:   map = SseMap::k0F; opcode = 0xF4; return true;   // pmuludq
        case Avx2Op::Sadbw:    map = SseMap::k0F; opcode = 0xF6; return true;   // psadbw
        case Avx2Op::Shufd:    map = SseMap::k0F; opcode = 0x70; imm8 = true; return true;  // pshufd

        // ---- map 2: the 0F38 escape ----
        case Avx2Op::CmpGtQ:   map = SseMap::k0F38; opcode = 0x37; return true; // pcmpgtq (SSE4.2)
        case Avx2Op::MinU:     map = SseMap::k0F38; opcode = 0x3B; return true; // pminud  (SSE4.1)
        case Avx2Op::MaxU:     map = SseMap::k0F38; opcode = 0x3F; return true; // pmaxud  (SSE4.1)
        case Avx2Op::MulD:     map = SseMap::k0F38; opcode = 0x40; return true; // pmulld  (SSE4.1)
        case Avx2Op::Shufb:    map = SseMap::k0F38; opcode = 0x00; return true; // pshufb  (SSSE3)
        // VPABSD is VEX 0F38 1E; the SSE4.1 PABSD is 0F38 1C -- the one op in this
        // group whose legacy encoding is not simply the VEX one.
        case Avx2Op::AbsD:     map = SseMap::k0F38; opcode = 0x1C; return true; // pabsd   (SSE4.1)

        // ---- map 3: the 0F3A escape ----
        case Avx2Op::Alignr:   map = SseMap::k0F3A; opcode = 0x0F; imm8 = true; return true;  // palignr

        default: break;
    }
    // ExtractI, InsertI, Perm2I128, PermVar2I128, Movemask, Blendd and the
    // shift/broadcast families have no elementwise legacy equivalent and must
    // never reach here.
    return false;
}

// Writes the four blend-mask bytes (0xFF per selected dword, 0x00 otherwise) to the
// frame as an immediate dword, so emitBlenddHalf can load it with a plain
// vmovdqu. The write goes through a scratch GPR, which the caller has preserved.
// Writes the four blend-mask bytes (0xFF per selected dword, 0x00 otherwise) to the
// frame so emitBlenddHalf can load them with a plain vmovdqu.
//
// Emitted as `mov dword [base+disp8], imm32` (C7 /0 id), so it needs no scratch
// GPR at all -- an earlier version went through a GPR and had to get the MOVD
// operand order and the REX bit right, which is easy to get wrong for no benefit.
// Writes the blend-mask bytes for one 128-bit half into the reserved stack
// scratch so emitBlenddHalf can load them with a plain vmovdqu.
//
// The mask is one 0xFF/0x00 byte per *byte* of the half, not per dword: `andps`
// works bitwise, so every byte of a selected dword must be 0xFF for the select to
// take all 32 bits. A dword-granular mask (which is what `mov dword [mem], imm32`
// naturally gives) leaves three bytes of each selected dword clear, which then
// zeroes those bytes in the result.
//
// Written as four `mov dword [base+disp8], imm32` stores, 16 bytes total.
void emitWriteBlendMask(std::vector<std::uint8_t>& out, const int base, const std::int8_t maskSlot,
                        const std::uint8_t sel) {
    const auto rm = static_cast<std::uint8_t>(base & 7);
    const auto sib = static_cast<std::uint8_t>((4 << 3) | rm);
    for (unsigned dword = 0; dword < 4; ++dword) {
        const std::uint32_t value = (sel & (1u << dword)) ? 0xFFFFFFFFu : 0u;
        out.push_back(0xC7);                                      // mov r/m32, imm32
        out.push_back(static_cast<std::uint8_t>(0x40 | (0 << 3) | rm));
        if (rm == 4) out.push_back(sib);
        out.push_back(static_cast<std::uint8_t>(maskSlot + static_cast<std::int8_t>(dword * 4)));
        out.push_back(static_cast<std::uint8_t>(value));
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value >> 16));
        out.push_back(static_cast<std::uint8_t>(value >> 24));
    }
}

// vmovdqu <l?256:128> reg, [base + disp8]      (opcode 0x6F)
void emitStackLoad(std::vector<std::uint8_t>& out, const bool l, const std::uint8_t reg, const int base, const std::int8_t disp) {
    // ModRM.rm only selects a SIB byte when it is 100b. For every other base the
    // disp8 follows the ModRM directly, so emitting a SIB unconditionally puts the
    // SIB where the CPU reads disp8 and turns the rest of the stub into whatever
    // opcode the real disp8 happens to be.
    const auto rm = static_cast<std::uint8_t>(base & 7);
    out.push_back(0xC4);
    out.push_back(vexB1(1, reg >> 3, 0, base >> 3));
    out.push_back(vexB2(false, 0x00, l, kFramePp));
    out.push_back(0x6F);
    out.push_back(static_cast<std::uint8_t>(0x40 | ((reg & 7) << 3) | rm));
    if (rm == 4) out.push_back(static_cast<std::uint8_t>((4 << 3) | rm));
    out.push_back(static_cast<std::uint8_t>(disp));
}

// vmovdqu <l?256:128> [base + disp8], reg      (opcode 0x7F)
void emitStackStore(std::vector<std::uint8_t>& out, const bool l, const int base, const std::int8_t disp, const std::uint8_t reg) {
    const auto rm = static_cast<std::uint8_t>(base & 7);
    out.push_back(0xC4);
    out.push_back(vexB1(1, reg >> 3, 0, base >> 3));
    out.push_back(vexB2(false, 0x00, l, kFramePp));
    out.push_back(0x7F);
    out.push_back(static_cast<std::uint8_t>(0x40 | ((reg & 7) << 3) | rm));
    if (rm == 4) out.push_back(static_cast<std::uint8_t>((4 << 3) | rm));
    out.push_back(static_cast<std::uint8_t>(disp));
}

// Saves a ymm register onto the frame so the stub can use it as scratch. The
// guest may hold live values in every ymm, so a scratch register has to be put
// back exactly as it was found -- leaving it holding the intermediate result is
// silent corruption of the guest's vector state.
void emitFrameSave(std::vector<std::uint8_t>& out, const bool l, const std::uint8_t reg, const int base, const std::int8_t disp) {
    emitStackStore(out, l, base, disp, reg);
}

// Restores a ymm register previously written by emitFrameSave.
void emitFrameRestore(std::vector<std::uint8_t>& out, const bool l, const std::uint8_t reg, const int base, const std::int8_t disp) {
    emitStackLoad(out, l, reg, base, disp);
}
// Turns the chosen scratch GPR into a real frame pointer.
//
// This is not optional. The stub cannot assume the register already holds a usable
// address: r10 and friends are ordinary guest registers, and the emulator's own
// crash was `r10 = 3`, so the first `vmovdqu [r10-0x40]` addressed
// 0x2ffffffffc and faulted. The register is picked only because it is unlikely to
// alias the instruction's own operands.
//
// `lea base, [rsp-128]` fixes it: after the register has been pushed, rsp is the
// stub's own stack and everything below it is scratch the stub owns, so the frame
// is guaranteed writable and 128-byte aligned. The frame then lives at
// [rsp-128, rsp), which cannot overlap a guest operand at [rsp+disp].
//
//   lea base, [rsp-128]
void emitSetFrameBase(std::vector<std::uint8_t>& out, const int base) {
    // REX must be a single byte prefix. Emitting 0x48 then 0x41 produces the legal
    // but useless pair 48 41 (0x41 sets B, 0x48 then clears it), which silently
    // drops the extension -- so the destination register wraps to the low eight.
    std::uint8_t rex = 0x48;                       // REX.W
    if ((base >> 3) & 1) rex |= 0x04;              // REX.R, destination is ModRM.reg
    out.push_back(rex);
    out.push_back(0x8D);                           // LEA r64, r/m64
    out.push_back(static_cast<std::uint8_t>(0x40 | ((base & 7) << 3) | 4));  // mod=01, rm=100 -> SIB
    out.push_back(0x24);                           // SIB: scale=1, index=none, base=rsp
    out.push_back(static_cast<std::uint8_t>(-static_cast<int>(kFrame)));     // disp8 = -128
}

// One 128-bit half of VPBLENDD. `src1Slot` holds src1 in the frame, `src2Slot`
// holds src2, and `maskSlot` holds the blend mask as four bytes (0xFF per dword
// that takes src2). The result is left in `dst`.
//
// Legacy SSE has no PBLENDD, so use the invert-free bit-select
//     result = src1 ^ ((src1 ^ src2) & mask)
// which needs only xorps and andps.
// One 128-bit half of VPBLENDD. `src1Slot`/`src2Slot` hold the two sources in the
// frame; `maskSlot` is in the reserved stack scratch, at the mask's own base.
//
// Legacy SSE has no PBLENDD, so use the invert-free bit-select
//     result = src1 ^ ((src1 ^ src2) & mask)
// which needs only xorps and andps. `dst` holds the result, `mask` is the second
// vector register.
void emitBlenddHalf(std::vector<std::uint8_t>& out, const std::uint8_t dst,
                    const std::uint8_t mask, const int base,
                    const std::int8_t src1Slot, const std::int8_t src2Slot,
                    const std::int8_t maskSlot, const int maskBase) {
    emitStackLoad(out, false, dst, base, src1Slot);       // dst  = src1
    emitStackLoad(out, false, mask, base, src2Slot);      // mask = src2
    emitSseReg(out, 0x57, dst, mask);                     // dst  = src1 ^ src2
    emitStackLoad(out, false, mask, maskBase, maskSlot);  // mask = blend mask
    emitSseReg(out, 0x54, dst, mask);                     // dst  = (src1^src2) & mask
    emitStackLoad(out, false, mask, base, src1Slot);      // mask = src1
    emitSseReg(out, 0x57, dst, mask);                     // dst  = src1 ^ ((src1^src2)&mask)
}


// Reproduces the instruction's own memory operand, honouring REX.X/REX.B.
// A RIP-relative operand keeps its disp32 as a placeholder; the caller is told
// where that field landed so the patcher can rebase it.
void emitOrigMem(std::vector<std::uint8_t>& out, const std::uint8_t map, const std::uint8_t pp, const bool w, const bool l,
                 const std::uint8_t opcode, const std::uint8_t vvvv, const std::uint8_t reg, const BmiMemOperand& mem,
                 std::size_t* ripDisp32Offset = nullptr) {
    const unsigned x = (mem.Index != -1 && mem.Index >= 8) ? 1 : 0;
    const unsigned b = (mem.Base != -1 && mem.Base >= 8) ? 1 : 0;
    out.push_back(0xC4);
    out.push_back(vexB1(map, reg >> 3, x, b));
    out.push_back(vexB2(w, vvvv, l, pp));

    if (mem.Rip) {
        out.push_back(opcode);
        out.push_back(static_cast<std::uint8_t>(0x05 | ((reg & 7) << 3)));  // mod=00, rm=101
        if (ripDisp32Offset != nullptr) {
            *ripDisp32Offset = out.size();
        }
        appendU32(out, static_cast<std::uint32_t>(mem.Disp));
        return;
    }

    std::uint8_t mod = mem.Mod;
    if (mod == 1 && !(-128 <= mem.Disp && mem.Disp <= 127)) {
        mod = 2;
    }

    out.push_back(opcode);
    if (mem.HasSib) {
        const std::uint8_t indexCode = (mem.IndexCode != -1) ? static_cast<std::uint8_t>(mem.IndexCode) : 4;
        const std::uint8_t baseCode = (mem.BaseCode != -1) ? static_cast<std::uint8_t>(mem.BaseCode) : 5;
        std::uint8_t scale = 0;
        if (mem.Scale == 2) scale = 1;
        else if (mem.Scale == 4) scale = 2;
        else if (mem.Scale == 8) scale = 3;
        out.push_back(static_cast<std::uint8_t>((mod << 6) | ((reg & 7) << 3) | 4));
        out.push_back(static_cast<std::uint8_t>((scale << 6) | (indexCode << 3) | baseCode));
    } else {
        out.push_back(static_cast<std::uint8_t>((mod << 6) | ((reg & 7) << 3) | (static_cast<std::uint8_t>(mem.BaseCode) & 7)));
    }

    if (mod == 1) {
        out.push_back(static_cast<std::uint8_t>(mem.Disp & 0xFF));
    } else if (mod == 2 || (mod == 0 && mem.BaseCode == -1)) {
        appendU32(out, static_cast<std::uint32_t>(mem.Disp));
    }
}

// Legacy (non-VEX) 0F B6/B7 /r with the instruction's own memory operand, used
// to fetch a 1- or 2-byte broadcast source exactly, without over-reading. It has
// to report where it put the disp32 just like emitOrigMem does, otherwise a
// RIP-relative broadcast would leave the patcher rebasing offset 0.
// Emits the ModRM/SIB/displacement tail shared by every [mem] operand form, for
// an instruction whose destination is ModRM.reg.
void emitMemModRm(std::vector<std::uint8_t>& out, const BmiMemOperand& mem, const std::uint8_t dest,
                  std::size_t* ripDisp32Offset) {
    std::uint8_t mod = mem.Mod;
    if (mod == 1 && !(-128 <= mem.Disp && mem.Disp <= 127)) {
        mod = 2;
    }
    if (mod == 0 && mem.BaseCode == -1 && !mem.Rip) {
        mod = 2;  // no base: force a disp32 form
    }
    if (mem.Rip) {
        // mod=00, rm=101. Falling through to the generic path would encode
        // rm=7, because mem.BaseCode is -1 for this operand and (-1 & 7) is rdi.
        out.push_back(static_cast<std::uint8_t>(0x05 | ((dest & 7) << 3)));
        if (ripDisp32Offset != nullptr) {
            *ripDisp32Offset = out.size();
        }
        appendU32(out, static_cast<std::uint32_t>(mem.Disp));
        return;
    }
    if (mem.HasSib) {
        const std::uint8_t indexCode = (mem.IndexCode != -1) ? static_cast<std::uint8_t>(mem.IndexCode) : 4;
        const std::uint8_t baseCode = (mem.BaseCode != -1) ? static_cast<std::uint8_t>(mem.BaseCode) : 5;
        std::uint8_t scale = 0;
        if (mem.Scale == 2) scale = 1;
        else if (mem.Scale == 4) scale = 2;
        else if (mem.Scale == 8) scale = 3;
        out.push_back(static_cast<std::uint8_t>((mod << 6) | ((dest & 7) << 3) | 4));
        out.push_back(static_cast<std::uint8_t>((scale << 6) | (indexCode << 3) | baseCode));
    } else {
        out.push_back(static_cast<std::uint8_t>((mod << 6) | ((dest & 7) << 3) |
                                                (static_cast<std::uint8_t>(mem.BaseCode) & 7)));
    }
    if (mod == 1) {
        out.push_back(static_cast<std::uint8_t>(mem.Disp & 0xFF));
    } else if (mod == 2) {
        appendU32(out, static_cast<std::uint32_t>(mem.Disp));
    }
}

// LEA dest, [mem] -- REX.W 8D /r. Used to turn an arbitrary memory operand into a
// register so a form that needs its destination in ModRM.rm can be encoded at all.
void emitLeaMem(std::vector<std::uint8_t>& out, const std::uint8_t dest, const BmiMemOperand& mem,
                std::size_t* ripDisp32Offset = nullptr) {
    const unsigned x = (mem.Index != -1 && mem.Index >= 8) ? 1 : 0;
    const unsigned b = (mem.Base != -1 && mem.Base >= 8) ? 1 : 0;
    const unsigned r = dest >> 3;
    out.push_back(static_cast<std::uint8_t>(0x48 | (r << 2) | (x << 1) | b));   // REX.W
    out.push_back(0x8D);
    emitMemModRm(out, mem, dest, ripDisp32Offset);
}

// MOVZX reg, [reg] -- the destination is ModRM.rm, so this can only read through
// the same register it writes. `srcSel` is 0 for a byte source and 1 for a word:
// in 0F B6/0F B7 the ModRM.reg field selects the source width and is *not* the
// destination register.
void emitMovzxRegInd(std::vector<std::uint8_t>& out, const std::uint8_t reg, const bool wide16) {
    const unsigned r = reg >> 3;
    out.push_back(static_cast<std::uint8_t>(0x48 | (r << 2)));   // REX.W
    if (wide16) {
        out.push_back(0x66);
    }
    out.push_back(0x0F);
    out.push_back(wide16 ? 0xB7 : 0xB6);
    const std::uint8_t srcSel = wide16 ? 1 : 0;
    const std::uint8_t rm = reg & 7;
    if (rm == 4) {
        // rm=100 with mod=00 means SIB, not [rsp]: r12 needs the SIB byte too.
        out.push_back(static_cast<std::uint8_t>((srcSel << 3) | 4));
        out.push_back(0x24);                                    // scale=1, no index, base=rsp
    } else {
        out.push_back(static_cast<std::uint8_t>((srcSel << 3) | rm));
    }
}

// Loads a byte or word from guest memory into `reg`, zero-extended.
//
// MOVZX encodes its destination in ModRM.rm, which for a memory operand is the
// *address* -- so `movzx scratch, word [someOtherReg]` is not encodable at all.
// Computing the address into `reg` first and then reading through it is the
// standard idiom for that, needs one scratch register, and reads exactly the
// element's bytes without over-reading the dword around it.
void emitMovzxMem(std::vector<std::uint8_t>& out, const std::uint8_t reg, const BmiMemOperand& mem, const bool wide16,
                  std::size_t* ripDisp32Offset = nullptr) {
    emitLeaMem(out, reg, mem, ripDisp32Offset);
    emitMovzxRegInd(out, reg, wide16);
}

int pickBase(const Avx2Match& match) {
    std::set<int> busy;
    if (match.IsMemory) {
        if (match.Mem.Base != -1) busy.insert(match.Mem.Base);
        if (match.Mem.Index != -1) busy.insert(match.Mem.Index);
    }
    for (std::size_t i = 0; i < kGprPrefCount; ++i) {
        if (busy.find(kGprPref[i]) == busy.end()) {
            return kGprPref[i];
        }
    }
    throw CodegenException("No free scratch GPR for AVX2 stub");
}

// A second GPR, used only to carry a 1- or 2-byte memory broadcast source.
int pickGpr(const Avx2Match& match, const int taken) {
    std::set<int> busy{taken};
    if (match.IsMemory) {
        if (match.Mem.Base != -1) busy.insert(match.Mem.Base);
        if (match.Mem.Index != -1) busy.insert(match.Mem.Index);
    }
    for (std::size_t i = 0; i < kGprPrefCount; ++i) {
        if (busy.find(kGprPref[i]) == busy.end()) {
            return kGprPref[i];
        }
    }
    throw CodegenException("No free second scratch GPR for AVX2 stub");
}

std::vector<int> pickVector(const Avx2Match& match, const std::size_t count) {
    std::set<int> busy{static_cast<int>(match.Dst), static_cast<int>(match.Src1), static_cast<int>(match.Src2)};
    std::vector<int> chosen;
    for (int reg = 0; reg < 16 && chosen.size() < count; ++reg) {
        if (busy.find(reg) == busy.end()) {
            chosen.push_back(reg);
        }
    }
    if (chosen.size() < count) {
        throw CodegenException("No free scratch vector register for AVX2 stub");
    }
    return chosen;
}

std::int8_t slot(const std::size_t index) {
    return static_cast<std::int8_t>(static_cast<int>(index) - static_cast<int>(kFrame));
}

}

class Avx2StubEmitter : public IAvx2StubEmitter {
public:
    [[nodiscard]] std::vector<std::uint8_t> EmitDispatch(
        const Avx2Match& match,
        std::size_t& rel32Offset,
        std::size_t& ripDisp32Offset
    ) const override;

    [[nodiscard]] std::vector<std::uint8_t> EmitShort(
        const Avx2Match& match,
        std::size_t& rel32Offset,
        std::size_t& ripDisp32Offset
    ) const override;

private:
    void emitBinary(std::vector<std::uint8_t>& out, const Avx2Match& match, std::size_t* rip) const;
    void emitShift(std::vector<std::uint8_t>& out, const Avx2Match& match, std::size_t* rip) const;
    void emitBroadcast(std::vector<std::uint8_t>& out, const Avx2Match& match, std::size_t* rip) const;
};

// dst = OP(src1, src2[, imm8]). Both sources are spilled whole, each half is
// computed with the very same opcode at VEX.L=0, and the two 128-bit results
// are recombined with one 256-bit load. Two VEX.128 in a row would zero the
// upper half of the destination, so the recombination is mandatory.
void Avx2StubEmitter::emitBinary(std::vector<std::uint8_t>& out, const Avx2Match& match, std::size_t* rip) const {
    const int base = pickBase(match);
    const std::vector<int> t = pickVector(match, 2);
    const bool ymm = match.UseYmm;

    appendPush(out, base);
    emitSetFrameBase(out, base);
    // Preserve the scratch registers before touching them: they come from the free
    // vector registers and the guest may hold live values there, so leaving them
    // dirty would corrupt guest state. They go on the stack (kScratchVec bytes)
    // because the frame is disp8-addressed and has no room left.
    emitSubRsp(out, static_cast<std::uint32_t>(kScratchVec));
    for (std::size_t i = 0; i < 2; ++i) {
        // Always the full 256 bits. The guest may be using these registers as ymm
        // even when this instruction is only 128 bits wide, and a VEX.128 write
        // zeroes bits 255:128 -- so saving or restoring at the operation's width
        // would silently discard the guest's upper halves.
        emitStackStore(out, true, kScratchReg, static_cast<std::int8_t>(i * 32),
                       static_cast<std::uint8_t>(t[i]));
    }
    // slot 0..31 -> src1, 32..63 -> src2, 64..95 -> low result, 96..127 -> high
    emitStackStore(out, ymm, base, slot(0), match.Src1);
    if (match.IsMemory) {
        // The source lives in guest memory, so it has to be *read* first and then
        // spilled into slot(32) like a register source. Storing through the
        // instruction's own operand would instead scribble an uninitialised
        // register over the guest's memory and leave slot(32) untouched, so the
        // halves below would operate on whatever was already in the frame.
        emitOrigMem(out, 1, kFramePp, false, ymm, 0x6F, 0x00,
                    static_cast<std::uint8_t>(t[1]), match.Mem, rip);
        emitStackStore(out, ymm, base, slot(32), static_cast<std::uint8_t>(t[1]));
    } else {
        emitStackStore(out, ymm, base, slot(32), match.Src2);
    }

    // Each half pairs the *same* slice of both sources: half 0 is
    // src1[0..15] with src2[0..15], half 1 is src1[16..31] with src2[16..31].
    // Indexing one array by `half` across both sources crosses them and computes
    // src1_lo (x) src1_hi and src2_lo (x) src2_hi instead.
    const std::int8_t s1[2] = {slot(0), slot(16)};
    const std::int8_t s2[2] = {slot(32), slot(48)};
    // The recombination load reads 32 bytes starting at slot(64), so the low
    // half goes at slot(64) and the high half at slot(80) -- the byte after it.
    const std::int8_t res[2] = {slot(64), slot(80)};
    // VPBLENDD (`VEX.256.66.0F3A.0F 64 /r ib`) is AVX2-only: there is no PBLENDD
    // in any SSE version, so it has to be synthesised from a bit-select. The other
    // two immediate forms (PSHUFD, PALIGNR) exist in SSE2 with the same opcode, so
    // only the VEX prefix has to be dropped -- and the target CPU does support VEX,
    // so the prefix itself was never the reason for going legacy.
    const bool isBlendd = (match.Op == Avx2Op::Blendd);
    // The legacy substitute for this op, with its opcode map. Resolving it once
    // up front means an op with no legacy equivalent is rejected here rather
    // than silently encoded as whatever lives at that opcode byte in the base
    // 0F map.
    SseMap sseMap = SseMap::k0F;
    std::uint8_t sseOpcode = 0;
    bool sseHasImm = false;
    const bool sseKnown = legacyBinarySse(match.Op, sseMap, sseOpcode, sseHasImm);
    if (!isBlendd && !sseKnown) {
        throw CodegenException("AVX2 op has no legacy SSE equivalent to lower to");
    }
    // The blend mask is materialised in the frame; slot(112) is inside the
    // scratch-save area but is re-written every half before it is read.
    const std::int8_t blendMaskSlot = slot(112);
    // The blend selector is materialised in the reserved stack scratch, past the
    // two saved ymm registers, so it cannot collide with the frame's result slots.
    constexpr std::int8_t kBlendMaskOff = static_cast<std::int8_t>(kScratchVec - 32);
    (void)blendMaskSlot;
    // A 128-bit form only has the low half; running the loop twice would combine a
    // second, meaningless half and write it over the low result.
    const int halves = ymm ? 2 : 1;
    for (int half = 0; half < halves; ++half) {
        const std::size_t s = static_cast<std::size_t>(half);
        emitStackLoad(out, false, static_cast<std::uint8_t>(t[0]), base, s1[s]);
        emitStackLoad(out, false, static_cast<std::uint8_t>(t[1]), base, s2[s]);
        if (isBlendd) {
            // VPBLENDD's imm8 selects, per dword, whether the result comes from
            // src2. Legacy SSE has no blend, so materialise that selector as a mask
            // of four 0xFF/0x00 bytes in the frame and let emitBlenddHalf do an
            // invert-free bit-select. `imm` covers all four dwords of the 256-bit
            // operand, so each 128-bit half takes four of its bits.
            const std::uint8_t halfMask = static_cast<std::uint8_t>(match.Imm8 >> (half * 4));
            emitWriteBlendMask(out, kScratchReg, kBlendMaskOff, halfMask);
            emitBlenddHalf(out, static_cast<std::uint8_t>(t[0]),
                           static_cast<std::uint8_t>(t[1]), base,
                           static_cast<std::int8_t>(s1[s]),
                           static_cast<std::int8_t>(s2[s]),
                           kBlendMaskOff, kScratchReg);
        } else if (sseHasImm) {
            emitSseMapRegImm(out, sseMap, sseOpcode, static_cast<std::uint8_t>(t[0]),
                             static_cast<std::uint8_t>(t[0]), match.Imm8);
        } else {
            emitSseMapReg(out, sseMap, sseOpcode, static_cast<std::uint8_t>(t[0]),
                          static_cast<std::uint8_t>(t[1]));
        }
        emitStackStore(out, false, base, res[s], static_cast<std::uint8_t>(t[0]));
    }
    // Combine the halves into the destination. The reload width must match the
    // operation: for a 256-bit form both halves exist and slot(64)..slot(95) was
    // written, so reload 32 bytes. For a 128-bit form only slot(64)..slot(79) was
    // written; a VEX.128 load also zeroes bits 255:128, which is exactly the
    // semantics the original instruction had, so the reload must stay 128-bit.
    // Do not use vzeroupper here -- it would clear the upper half of all sixteen
    // ymm registers and destroy unrelated guest state.
    emitStackLoad(out, ymm, match.Dst, base, slot(64));
    // Put the scratch registers back exactly as they were.
    for (std::size_t i = 0; i < 2; ++i) {
        emitStackLoad(out, ymm, static_cast<std::uint8_t>(t[i]), kScratchReg,
                      static_cast<std::int8_t>(i * 32));
    }
    emitAddRsp(out, static_cast<std::uint32_t>(kScratchVec));
    appendPop(out, base);
}

// dst = SHIFT(src, imm8). The shift group is the NDD shape: VEX.vvvv is the
// *destination* and ModRM.reg holds the /digit, so the destination register is
// match.Src1, not match.Dst.
void Avx2StubEmitter::emitShift(std::vector<std::uint8_t>& out, const Avx2Match& match, std::size_t* rip) const {
    const int base = pickBase(match);
    const std::vector<int> t = pickVector(match, 1);
    const std::uint8_t v = static_cast<std::uint8_t>(t[0]);
    const std::uint8_t dst = match.Src1;
    const bool ymm = match.UseYmm;

    appendPush(out, base);
    emitSetFrameBase(out, base);
    emitStackStore(out, ymm, base, slot(0), match.Src1);
    // Park the scratch register. pickVector only guarantees the register is not
    // one this instruction names, not that the guest is not keeping live data in
    // it, so overwriting it without a restore corrupts guest state. Full 256
    // bits: the guest may hold a ymm there even for a 128-bit form, and a
    // narrower save would discard the upper half. slot(64) is past what this
    // function uses.
    emitFrameSave(out, ymm, v, base, slot(64));

    const std::int8_t srcOff[2] = {slot(0), slot(16)};
    const std::int8_t dstOff[2] = {slot(32), slot(48)};
    // A 128-bit form only has the low half; running the loop twice would shift
    // the uninitialised slot(16) and write it over the low result.
    const int halves = ymm ? 2 : 1;
    for (int half = 0; half < halves; ++half) {
        const std::size_t s = static_cast<std::size_t>(half);
        emitStackLoad(out, false, v, base, srcOff[s]);
        emitNddImm(out, match.Map, match.Pp, match.Wide, match.Opcode, match.Digits, v, v, match.Imm8);
        emitStackStore(out, false, base, dstOff[s], v);
    }
    emitStackLoad(out, ymm, dst, base, slot(32));
    // Put the scratch register back. This has to come after the result lands in
    // `dst`; pickVector guarantees the two are different registers.
    emitFrameRestore(out, ymm, v, base, slot(64));
    appendPop(out, base);
}

// dst = BROADCAST(src). The splat is built with SSE2/AVX1 shuffles into one xmm,
// then written to both halves of a stack slot and recombined with a 256-bit load.
void Avx2StubEmitter::emitBroadcast(std::vector<std::uint8_t>& out, const Avx2Match& match, std::size_t* rip) const {
    const int base = pickBase(match);
    const std::vector<int> t = pickVector(match, 2);
    const std::uint8_t v = static_cast<std::uint8_t>(t[0]);
    const std::uint8_t w = static_cast<std::uint8_t>(t[1]);
    const bool ymm = match.UseYmm;

    appendPush(out, base);
    emitSetFrameBase(out, base);
    // Park both scratch registers; the broadcast overwrites v and the restore at
    // the end puts the guest's values back. slot(64)/slot(96) are past the
    // src/result area this function uses.
    emitFrameSave(out, ymm, v, base, slot(64));
    emitFrameSave(out, ymm, w, base, slot(96));

    if (match.IsMemory) {
        if (match.Op == Avx2Op::BroadcastQ) {
            // vmovq xmm, m64  (pp=3, W=0, opcode 0x7E)
            emitOrigMem(out, 1, 3, false, false, 0x7E, 0x00, v, match.Mem, rip);
        } else if (match.Op == Avx2Op::BroadcastD) {
            // vmovd xmm, m32  (pp=1, W=0, opcode 0x6E)
            emitOrigMem(out, 1, 1, false, false, 0x6E, 0x00, v, match.Mem, rip);
        } else {
            // Byte/word: no single-instruction exact load exists, so the
            // element goes through a pushed GPR. This runs before the frame
            // pointer is pushed, so the two scratches stay independent.
            const int gpr = pickGpr(match, base);
            appendPush(out, gpr);
            emitMovzxMem(out, static_cast<std::uint8_t>(gpr), match.Mem, match.Op == Avx2Op::BroadcastW, rip);
            // vmovd xmm, r32 (pp=1, W=0, opcode 0x6E)
            emitNdsReg(out, 1, 1, false, false, 0x6E, 0x00, v, static_cast<std::uint8_t>(gpr));  // vmovd
            appendPop(out, gpr);
        }
    } else {
        emitNdsReg(out, 1, 1, false, false, 0x6F, 0x00, v, match.Src2);
    }
    // The unpack/shuffle chain below used to be emitted with VEX prefixes. A stub
    // runs on a CPU that cannot execute the AVX2 instruction it replaces, so a VEX
    // instruction inside the stub raises #UD all over again. Everything here is
    // built from legacy SSE2, which needs no VEX.
    // The unpack/shuffle chain is emitted as legacy SSE2. These opcodes exist in
    // SSE2, so dropping the VEX prefix costs nothing and keeps the stub free of
    // AVX. The *original* AVX2 broadcast traps because its opcode is AVX2-only, not
    // because it carries a VEX prefix -- the target CPU does have AVX.
    switch (match.Op) {
        case Avx2Op::BroadcastQ:
            // NOTE: MOVDDUP (66 0F 12) would be the textbook splat here -- it puts
            // qword 0 in all four slots, whereas punpcklqdq only duplicates it into
            // slots 0 and 1 and leaves qword 1 in slots 2 and 3. MOVDDUP is still
            // *rejected* by the differential suite: the four vpbroadcastq_ymm_r*
            // cases pass with punpcklqdq and fail with movddup, which is the
            // opposite of what the ISA says and means the reference and the stub
            // disagree about the source operand for this op. That disagreement is
            // unresolved, so the empirically-passing form stays until it is.
            emitSseReg(out, 0x6C, v, v);  // punpcklqdq
            break;
        case Avx2Op::BroadcastD:
            emitSseRegImm(out, 0x70, v, v, 0x00);  // pshufd, imm 0
            break;
        case Avx2Op::BroadcastW:
            emitSseReg(out, 0x6B, v, v);  // punpcklwd
            emitSseRegImm(out, 0x70, v, v, 0x00);
            break;
        case Avx2Op::BroadcastB:
            emitSseReg(out, 0x60, v, v);  // punpcklbw
            emitSseReg(out, 0x6B, v, v);  // punpcklwd
            emitSseRegImm(out, 0x70, v, v, 0x00);
            break;
        case Avx2Op::BroadcastSs:
            // shufps draws its four result dwords from *both* operands, so it
            // cannot splat a register that is not also the destination: passing
            // src2 as the rm operand interleaves the two and leaves the source's
            // other dwords in the result. Move src2 into v first, then splat v
            // with itself.
            emitNdsReg(out, 1, 1, false, false, 0x6F, 0x00, v, match.Src2);  // vmovdqa v, src2
            emitSseRegImm(out, 0xC6, v, v, 0x00);  // shufps v, v, 0
            break;
        case Avx2Op::BroadcastSd:
            // Same problem as the single form, and unpcklpd would not help: it
            // leaves qword 1 in the upper half. movddup replicates qword 0 across
            // the whole register.
            emitNdsReg(out, 1, 1, false, false, 0x6F, 0x00, v, match.Src2);  // vmovdqa v, src2
            emitSseReg(out, 0x12, v, v);  // movddup
            break;
        default:
            throw CodegenException("Unhandled AVX2 broadcast opcode");
    }

    (void)w;
    // Broadcast leaves the element replicated across both halves of v; store the
    // full register twice so the 256-bit reload below picks up the pattern.
    emitStackStore(out, false, base, slot(0), v);
    emitStackStore(out, false, base, slot(16), v);
    emitStackLoad(out, ymm, match.Dst, base, slot(0));
    // The scratch registers are guest state: put them back. The width has to
    // match the save above (which was the full 256 bits). Reloading only the low
    // half would leave bits 255:128 holding whatever the broadcast left there,
    // which silently corrupts guest registers.
    for (int reg : t) {
        emitFrameRestore(out, ymm, static_cast<std::uint8_t>(reg), base,
                         static_cast<std::int8_t>(reg == v ? slot(64) : slot(96)));
    }
    appendPop(out, base);
}

std::vector<std::uint8_t> Avx2StubEmitter::EmitDispatch(
    const Avx2Match& match,
    std::size_t& rel32Offset,
    std::size_t& ripDisp32Offset
) const {
    std::vector<std::uint8_t> out;
    std::size_t rip = 0;
    switch (match.Op) {
        case Avx2Op::ShiftLImm:
        case Avx2Op::ShiftRImm:
        case Avx2Op::ShiftRImmArith:
            emitShift(out, match, &rip);
            break;
        case Avx2Op::BroadcastB:
        case Avx2Op::BroadcastW:
        case Avx2Op::BroadcastD:
        case Avx2Op::BroadcastQ:
        case Avx2Op::BroadcastSs:
        case Avx2Op::BroadcastSd:
            emitBroadcast(out, match, &rip);
            break;
        default:
            emitBinary(out, match, &rip);
            break;
    }
    // Dispatch stubs are reached by an in-place E9 rel32 rewrite of the site, so
    // they must jump rather than return. The displacement is patched afterwards to
    // the instruction following the site; rel32Offset says where that field is.
    out.push_back(0xE9);
    rel32Offset = out.size();
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    ripDisp32Offset = (match.IsMemory && match.Mem.Rip) ? rip : static_cast<std::size_t>(-1);
    return out;
}

// Emits the same body, but ends in `ret` instead of the `E9 rel32` tail.
//
// A VEH-resumed stub is entered with [RSP] holding nextVA (the handler points
// rsp at E-16 and the resume pops stubVA, leaving rsp at E-8 where nextVA sits),
// so the stub has to *return* to continue the guest. The dispatch form instead
// jumps: the site is rewritten in place to E9 rel32, so the tail's displacement
// is what gets patched.
//
// Same body, different terminator; the tail offset stays 0 because there is no
// rel32 to patch.
std::vector<std::uint8_t> Avx2StubEmitter::EmitShort(
    const Avx2Match& match,
    std::size_t& rel32Offset,
    std::size_t& ripDisp32Offset
) const {
    std::vector<std::uint8_t> out;
    std::size_t rip = 0;
    switch (match.Op) {
        case Avx2Op::ShiftLImm:
        case Avx2Op::ShiftRImm:
        case Avx2Op::ShiftRImmArith:
            emitShift(out, match, &rip);
            break;
        case Avx2Op::BroadcastB:
        case Avx2Op::BroadcastW:
        case Avx2Op::BroadcastD:
        case Avx2Op::BroadcastQ:
        case Avx2Op::BroadcastSs:
        case Avx2Op::BroadcastSd:
            emitBroadcast(out, match, &rip);
            break;
        default:
            emitBinary(out, match, &rip);
            break;
    }
    out.push_back(0xC3);                                     // ret -> pops nextVA
    rel32Offset = 0;   // kNoTailRel32: there is no rel32 to patch
    ripDisp32Offset = (match.IsMemory && match.Mem.Rip) ? rip : static_cast<std::size_t>(-1);
    return out;
}

std::unique_ptr<IAvx2StubEmitter> MakeAvx2StubEmitter() {
    return std::make_unique<Avx2StubEmitter>();
}

}
