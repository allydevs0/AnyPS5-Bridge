#pragma once
// Emulation of BMI1/BMI2 and POPCNT/LZCNT/TZCNT on Intel CPUs lacking them
// (e.g. Intel Core i5-3570K / Ivy Bridge, which has neither BMI1 nor BMI2).
//
// The PS5's Zen 2 CPU has BMI1/BMI2, so the guest compiler emits shlx/shrx/sarx/andn/bextr/
// bzhi/mulx/pdep/pext/rorx/lzcnt/tzcnt freely. The relinker converts those sites of the main
// executable into dispatch stubs (--bmi-compat), but the guest modules it emits beside it
// (app0/sce_module/*.guest.prx) are written with their original code: they are loaded as plain
// PE images and run natively, so every one of their sites raises STATUS_ILLEGAL_INSTRUCTION.
// The vectored exception handler decodes the instruction, computes the result, updates the
// CONTEXT and resumes, the same way the AVX2, SSE4A and MOVBE handlers do.
//
// Encodings were taken from the GNU assembler as the reference:
//
//   popcnt  r, r/m      F3 (REX.W) 0F B8 /r        BMI-less, decoded here because Ivy Bridge
//   tzcnt   r, r/m      F3 (REX.W) 0F BC /r        has no TZCNT and treats these as BSF,
//   lzcnt   r, r/m      F3 (REX.W) 0F BD /r        which silently differs for a zero input.
//   andn    r, r/m, r   VEX.NDS.LZ.NP.0F38 F2 /r
//   bextr   r, r/m, r   VEX.NDS.LZ.NP.0F38 F7 /r
//   sarx    r, r/m, r   VEX.NDS.LZ.F3.0F38 F7 /r
//   shlx    r, r/m, r   VEX.NDS.LZ.66.0F38 F7 /r
//   shrx    r, r/m, r   VEX.NDS.LZ.F2.0F38 F7 /r
//   bzhi    r, r/m, r   VEX.NDS.LZ.NP.0F38 F5 /r
//   pdep    r, r/m, r   VEX.NDS.LZ.F2.0F38 F5 /r
//   pext    r, r/m, r   VEX.NDS.LZ.F3.0F38 F5 /r
//   mulx    r, r/m, r   VEX.NDS.LZ.F3.0F38 F6 /r
//   rorx    r, r/m, ib  VEX.LZ.F3.0F3A F0 /r ib
//   blsr/blsmsk/bls/blsi  VEX.NDS.LZ.NP.0F38 F3 /1../5
#ifdef _WIN32
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bmi_emu {

inline std::uint64_t GetGpr(const CONTEXT& ctx, unsigned reg) {
    switch (reg & 15) {
        case 0:  return ctx.Rax;
        case 1:  return ctx.Rcx;
        case 2:  return ctx.Rdx;
        case 3:  return ctx.Rbx;
        case 4:  return ctx.Rsp;
        case 5:  return ctx.Rbp;
        case 6:  return ctx.Rsi;
        case 7:  return ctx.Rdi;
        case 8:  return ctx.R8;
        case 9:  return ctx.R9;
        case 10: return ctx.R10;
        case 11: return ctx.R11;
        case 12: return ctx.R12;
        case 13: return ctx.R13;
        case 14: return ctx.R14;
        case 15: return ctx.R15;
    }
    return 0;
}

inline void SetGpr(CONTEXT& ctx, unsigned reg, std::uint64_t value, int sizeInBytes) {
    auto update = [&](DWORD64& target) {
        if (sizeInBytes == 8) target = value;
        // A 32-bit result zero-extends into the full 64-bit register.
        else if (sizeInBytes == 4) target = static_cast<std::uint32_t>(value);
    };
    switch (reg & 15) {
        case 0:  update(ctx.Rax); break;
        case 1:  update(ctx.Rcx); break;
        case 2:  update(ctx.Rdx); break;
        case 3:  update(ctx.Rbx); break;
        case 4:  update(ctx.Rsp); break;
        case 5:  update(ctx.Rbp); break;
        case 6:  update(ctx.Rsi); break;
        case 7:  update(ctx.Rdi); break;
        case 8:  update(ctx.R8); break;
        case 9:  update(ctx.R9); break;
        case 10: update(ctx.R10); break;
        case 11: update(ctx.R11); break;
        case 12: update(ctx.R12); break;
        case 13: update(ctx.R13); break;
        case 14: update(ctx.R14); break;
        case 15: update(ctx.R15); break;
    }
}

inline std::uint64_t Mask(int sizeInBytes) {
    return sizeInBytes == 8 ? ~0ULL : (sizeInBytes == 4 ? 0xffffffffULL : 0xffffULL);
}

inline unsigned PopCount(std::uint64_t value, int sizeInBytes) {
    unsigned count = 0;
    const auto bits = static_cast<unsigned>(sizeInBytes * 8);
    for (unsigned i = 0; i < bits; ++i) count += static_cast<unsigned>((value >> i) & 1);
    return count;
}

inline unsigned CountTrailingZeros(std::uint64_t value, int sizeInBytes) {
    const auto masked = value & Mask(sizeInBytes);
    if (masked == 0) return static_cast<unsigned>(sizeInBytes * 8);
    unsigned index = 0;
    while (((masked >> index) & 1) == 0) ++index;
    return index;
}

inline std::uint64_t Deposit(std::uint64_t mask, std::uint64_t source, int sizeInBytes) {
    std::uint64_t result = 0;
    unsigned out = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(sizeInBytes * 8); ++i) {
        if (((mask >> i) & 1) == 0) continue;
        if (((source >> out) & 1) != 0) result |= 1ULL << i;
        ++out;
    }
    return result;
}

inline std::uint64_t Extract(std::uint64_t mask, std::uint64_t source, int sizeInBytes) {
    std::uint64_t result = 0;
    unsigned out = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(sizeInBytes * 8); ++i) {
        if (((mask >> i) & 1) == 0) continue;
        if (((source >> i) & 1) != 0) result |= 1ULL << out;
        ++out;
    }
    return result;
}

// The BMI instructions are not all flag-neutral: ANDN, BEXTR, BZHI, the BLS group, MULX,
// PDEP, PEXT and the bit counts write flags, and the guest's branches test them right after.
enum class Flags : unsigned { None, ZeroFromResult, CarryClear, CarryOnly };

inline void WriteFlags(CONTEXT& ctx, std::uint64_t value, Flags mode, bool carry = false) {
    DWORD64 flags = ctx.EFlags;
    if (mode == Flags::None) return;
    if (mode == Flags::CarryOnly) { // MULX only defines CF and OF
        flags = carry ? (flags | 0x001u | 0x800u) : (flags & ~(0x001u | 0x800u));
        return;
    }
    flags = value == 0 ? (flags | 0x40u) : (flags & ~0x40u);                    // ZF
    flags = (value & 0x8000000000000000ULL) != 0 ? (flags | 0x80u) : (flags & ~0x80u); // SF
    if (mode == Flags::CarryClear) {
        flags = carry ? (flags | 0x001u) : (flags & ~0x001u);                    // CF
        flags &= ~(0x800u | 0x010u | 0x004u);                                    // OF, AF, PF
    }
    ctx.EFlags = flags;
}

// One decoded operand: a register, or a memory reference already resolved to an address.
struct Operand {
    bool isMemory = false;
    unsigned reg = 0;
    std::uint64_t address = 0;
};

struct Instruction {
    std::uint8_t map = 0;   // 2 = 0F38, 3 = 0F3A
    std::uint8_t opcode = 0;
    std::uint8_t pp = 0;    // 0 = none, 1 = 66, 2 = F3, 3 = F2
    std::uint8_t regDigit = 0;
    int sizeInBytes = 4;
    bool rexW = false;
    bool rexR = false;
    unsigned reg = 0;       // ModRM.reg, VEX/REX extended
    unsigned vvvv = 0;      // ~VEX.vvvv
    Operand rm;
    std::uint8_t immediate = 0;
    bool hasImmediate = false;
    std::size_t length = 0;
};

inline bool DecodeMemory(const std::uint8_t* bytes, std::size_t available, std::size_t& cursor,
                         std::uint8_t modrm, std::uint8_t indexExtension, std::uint8_t baseExtension,
                         const CONTEXT& ctx, bool addressSize32, Operand& out) {
    const auto mod = static_cast<std::uint8_t>(modrm >> 6);
    const auto rm = static_cast<std::uint8_t>(modrm & 7);
    const auto rexX = static_cast<std::uint8_t>(indexExtension);
    const auto rexB = static_cast<std::uint8_t>(baseExtension);

    std::uint64_t address = 0;
    bool ripRelative = false;
    bool absolute = false;
    int64_t displacement = 0;

    if (rm == 4) { // SIB byte
        if (cursor >= available) return false;
        const std::uint8_t sib = bytes[cursor++];
        const auto scale = static_cast<unsigned>(sib >> 6);
        const auto index = static_cast<unsigned>(((sib >> 3) & 7) | (rexX << 3));
        const auto base = static_cast<unsigned>((sib & 7) | (rexB << 3));
        if (mod == 0 && (sib & 7) == 5 && rexB == 0) {
            absolute = true; // No base register: the disp32 is the address.
        } else {
            address = GetGpr(ctx, base);
        }
        if (index != 4) address += GetGpr(ctx, index) << scale;
    } else if (mod == 0 && rm == 5) {
        ripRelative = true;
    } else {
        address = GetGpr(ctx, static_cast<unsigned>(rm | (rexB << 3)));
    }

    if (mod == 1 || mod == 2 || (mod == 0 && rm == 5)) {
        const std::size_t width = mod == 1 ? 1u : 4u;
        if (cursor + width > available) return false;
        if (width == 1) {
            displacement = static_cast<std::int8_t>(bytes[cursor]);
        } else {
            std::int32_t value = 0;
            std::memcpy(&value, bytes + cursor, 4);
            displacement = value;
        }
        cursor += width;
    }
    if (mod == 0 && absolute) {
        if (cursor + 4 > available) return false;
        std::int32_t value = 0;
        std::memcpy(&value, bytes + cursor, 4);
        cursor += 4;
        address = static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
    } else {
        address += static_cast<std::uint64_t>(displacement);
    }
    if (ripRelative) address = ctx.Rip + cursor + static_cast<std::uint64_t>(displacement);
    if (addressSize32) address &= 0xffffffffULL;

    out.isMemory = true;
    out.address = address;
    return true;
}

inline bool Decode(const std::uint8_t* bytes, std::size_t available, const CONTEXT& ctx, Instruction& out) {
    std::size_t cursor = 0;
    std::uint8_t rex = 0;
    bool addressSize32 = false;
    bool sawF3 = false;
    bool sawF2 = false;

    while (cursor < available) {
        const std::uint8_t prefix = bytes[cursor];
        if (prefix == 0x66) cursor++;
        else if (prefix == 0x67) { addressSize32 = true; cursor++; }
        else if (prefix == 0xF2) { sawF2 = true; cursor++; }
        else if (prefix == 0xF3) { sawF3 = true; cursor++; }
        else if (prefix >= 0x40 && prefix <= 0x4F) { rex = prefix; cursor++; }
        else break;
    }
    if (cursor >= available) return false;

    std::uint8_t indexExtension = static_cast<std::uint8_t>((rex >> 1) & 1);
    std::uint8_t baseExtension = static_cast<std::uint8_t>(rex & 1);
    auto regExtension = static_cast<std::uint8_t>((rex >> 2) & 1);
    out.rexW = (rex & 0x08) != 0;
    out.sizeInBytes = out.rexW ? 8 : 4;

    if (bytes[cursor] == 0x0F) {
        // The only legacy encodings here are the F3-prefixed bit count instructions.
        if (!sawF3 || sawF2 || cursor + 2 >= available) return false;
        const std::uint8_t second = bytes[cursor + 1];
        if (second != 0xB8 && second != 0xBC && second != 0xBD) return false;
        cursor += 2;
        const std::uint8_t modrm = bytes[cursor++];
        out.map = 1;
        out.opcode = second;
        out.regDigit = static_cast<std::uint8_t>((modrm >> 3) & 7);
        out.reg = (out.regDigit | (regExtension << 3));
        out.vvvv = 0;
        out.rm.isMemory = (modrm >> 6) != 3;
        if (!out.rm.isMemory) out.rm.reg = (modrm & 7) | (baseExtension << 3);
        if (out.rm.isMemory &&
            !DecodeMemory(bytes, available, cursor, modrm, indexExtension, baseExtension, ctx, addressSize32, out.rm)) {
            return false;
        }
        out.length = cursor;
        return true;
    }

    if (bytes[cursor] == 0xC5) {
        if (cursor + 2 >= available) return false;
        const std::uint8_t second = bytes[cursor + 1];
        out.rexW = (second & 0x80) != 0;
        out.sizeInBytes = out.rexW ? 8 : 4;
        indexExtension = 0;
        // The three extension bits of a VEX prefix are stored inverted.
        baseExtension = static_cast<std::uint8_t>((~second >> 2) & 1);
        regExtension = static_cast<std::uint8_t>((~second >> 7) & 1);
        out.vvvv = (~(second >> 3)) & 0x0F;
        out.pp = second & 0x03;
        out.map = 2;
        cursor += 2;
    } else if (bytes[cursor] == 0xC4) {
        if (cursor + 3 >= available) return false;
        const std::uint8_t first = bytes[cursor + 1];
        const std::uint8_t second = bytes[cursor + 2];
        out.rexW = (second & 0x80) != 0;
        out.sizeInBytes = out.rexW ? 8 : 4;
        indexExtension = static_cast<std::uint8_t>((~first >> 6) & 1);
        baseExtension = static_cast<std::uint8_t>((~first >> 5) & 1);
        regExtension = static_cast<std::uint8_t>((~first >> 7) & 1);
        out.vvvv = (~(second >> 3)) & 0x0F;
        out.pp = second & 0x03;
        out.map = first & 0x1F;
        cursor += 3;
    } else {
        return false;
    }
    if (out.map != 2 && out.map != 3) return false;

    out.opcode = bytes[cursor++];
    const std::uint8_t modrm = bytes[cursor++];
    out.regDigit = static_cast<std::uint8_t>((modrm >> 3) & 7);
    out.reg = out.regDigit | (regExtension << 3);
    out.rm.isMemory = (modrm >> 6) != 3;
    if (!out.rm.isMemory) out.rm.reg = (modrm & 7) | (baseExtension << 3);
    if (out.rm.isMemory &&
        !DecodeMemory(bytes, available, cursor, modrm, indexExtension, baseExtension, ctx, addressSize32, out.rm)) {
        return false;
    }
    if (out.opcode == 0xF0 && out.map == 3) { // RORX carries an immediate byte.
        if (cursor >= available) return false;
        out.immediate = bytes[cursor++];
        out.hasImmediate = true;
    }
    out.length = cursor;
    return true;
}

inline std::uint64_t ReadRm(const Instruction& instruction, const CONTEXT& ctx) {
    if (!instruction.rm.isMemory) return GetGpr(ctx, instruction.rm.reg) & Mask(instruction.sizeInBytes);
    std::uint64_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(instruction.rm.address),
                static_cast<std::size_t>(instruction.sizeInBytes));
    return value & Mask(instruction.sizeInBytes);
}

inline bool Emulate(const std::uint8_t* bytes, std::size_t available, CONTEXT& ctx) {
    Instruction instruction;
    if (!Decode(bytes, available, ctx, instruction)) return false;

    const int size = instruction.sizeInBytes;
    const auto bits = static_cast<unsigned>(size * 8);
    const std::uint64_t rm = ReadRm(instruction, ctx);
    std::uint64_t result = 0;

    if (instruction.map == 1) { // POPCNT / TZCNT / LZCNT
        const auto counted = static_cast<unsigned>(size * 8);
        if (instruction.opcode == 0xB8) result = PopCount(rm, size);
        else if (instruction.opcode == 0xBC) result = CountTrailingZeros(rm, size);
        else result = counted - PopCount(rm, size); // LZCNT, and 0 for an all-zero input gives the width
        SetGpr(ctx, instruction.reg, result, size);
        // POPCNT clears every flag except ZF; LZCNT and TZCNT set CF when the source is zero.
        WriteFlags(ctx, result, Flags::CarryClear, instruction.opcode == 0xB8 ? false : rm == 0);
        ctx.Rip += instruction.length;
        return true;
    }

    const std::uint64_t vvvv = GetGpr(ctx, instruction.vvvv) & Mask(size);

    if (instruction.map == 3) { // RORX r, r/m, imm8
        if (instruction.opcode != 0xF0) return false;
        const auto rotate = static_cast<unsigned>(instruction.immediate & 0x3F);
        result = rotate == 0 ? rm : ((rm >> rotate) | (rm << ((bits - rotate) & (bits - 1))));
        SetGpr(ctx, instruction.reg, result, size);
        ctx.Rip += instruction.length;
        return true;
    }

    switch (instruction.opcode) {
        case 0xF2: // ANDN r, r/m, vvvv
            if (instruction.pp != 0) return false;
            result = ~vvvv & rm;
            SetGpr(ctx, instruction.reg, result, size);
            WriteFlags(ctx, result, Flags::CarryClear);
            break;
        case 0xF5:
            if (instruction.pp == 0) { // BZHI: the index selects the bits to keep
                const auto index = static_cast<unsigned>(vvvv & 0xFF);
                const auto usable = index > bits ? bits : index;
                result = rm & (usable == 0 ? 0 : ((1ULL << usable) - 1));
                SetGpr(ctx, instruction.reg, result, size);
                WriteFlags(ctx, result, Flags::CarryClear, vvvv > bits);
            } else if (instruction.pp == 2) { // PEXT r, r/m, vvvv
                result = Extract(rm, vvvv, size);
                SetGpr(ctx, instruction.reg, result, size);
                WriteFlags(ctx, result, Flags::CarryClear);
            } else if (instruction.pp == 3) { // PDEP r, r/m, vvvv
                result = Deposit(rm, vvvv, size);
                SetGpr(ctx, instruction.reg, result, size);
                WriteFlags(ctx, result, Flags::CarryClear);
            } else {
                return false;
            }
            break;
        case 0xF6: // MULX: ModRM.reg gets the low half, VEX.vvvv the high half
            if (instruction.pp != 3) return false;
            if (size == 8) {
                const __uint128_t product = static_cast<__uint128_t>(rm) * static_cast<__uint128_t>(vvvv);
                SetGpr(ctx, instruction.reg, static_cast<std::uint64_t>(product), size);
                SetGpr(ctx, instruction.vvvv, static_cast<std::uint64_t>(product >> 64), size);
            } else {
                const auto product = static_cast<std::uint32_t>(rm) * static_cast<std::uint32_t>(vvvv);
                SetGpr(ctx, instruction.reg, product, size);
                SetGpr(ctx, instruction.vvvv, product >> 16, size);
            }
            WriteFlags(ctx, 0, Flags::CarryOnly, GetGpr(ctx, instruction.vvvv) != 0);
            break;
        case 0xF7:
            if (instruction.pp == 0) { // BEXTR: start in vvvv[7:0], length in vvvv[15:8]
                const auto start = static_cast<unsigned>(vvvv & 0xFF);
                const auto length = static_cast<unsigned>((vvvv >> 8) & 0xFF);
                if (start >= bits || length == 0) result = 0;
                else {
                    const auto usable = length > bits - start ? bits - start : length;
                    result = (rm >> start) & ((1ULL << usable) - 1);
                }
                SetGpr(ctx, instruction.reg, result, size);
                WriteFlags(ctx, result, Flags::CarryClear);
            } else if (instruction.pp == 1) { // SHLX: a shift of at least the width yields zero
                const auto shift = static_cast<unsigned>(vvvv & (bits - 1));
                result = shift == 0 ? rm : (rm << shift);
                SetGpr(ctx, instruction.reg, result, size);
            } else if (instruction.pp == 2) { // SARX: arithmetic, the sign bit fills the top
                const auto shift = static_cast<unsigned>(vvvv & (bits - 1));
                result = rm;
                if (shift != 0) {
                    result = rm >> shift;
                    if (((rm >> (bits - 1)) & 1) != 0) result |= ~((1ULL << (bits - shift)) - 1);
                }
                SetGpr(ctx, instruction.reg, result, size);
            } else if (instruction.pp == 3) { // SHRX: logical, zero fills the top
                const auto shift = static_cast<unsigned>(vvvv & (bits - 1));
                result = shift == 0 ? rm : (rm >> shift);
                SetGpr(ctx, instruction.reg, result, size);
            } else {
                return false;
            }
            break;
        case 0xF3: { // BLSR / BLSMSK / BLSI / BLS: the destination is VEX.vvvv, ModRM.reg is the /digit
            if (instruction.pp != 0) return false;
            const bool zero = rm == 0;
            switch (instruction.regDigit) {
                case 1: result = rm & (rm - 1); break;                                   // BLSR
                case 2: result = ~rm & (rm - 1); break;                                  // BLSMSK
                case 3: result = zero ? ~0ULL : rm; break;                               // BLSI: src | -(src != 0)
                case 5: result = zero ? 0 : rm; break;                                   // BLS: src & -(src != 0)
                default: return false;
            }
            SetGpr(ctx, instruction.vvvv, result, size);
            WriteFlags(ctx, result, Flags::CarryClear);
            break;
        }
        default:
            return false;
    }

    ctx.Rip += instruction.length;
    return true;
}

} // namespace bmi_emu
#endif



