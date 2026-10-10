#pragma once
// Emulation of AVX2 instructions not supported on older CPUs (e.g. Intel Core i5-3570K / Ivy Bridge).
//
// Ivy Bridge supports AVX1 (floating point vector operations, 256-bit VMOVUPS/VXORPS, etc.),
// but lacks AVX2 (variable vector shifts, integer permutes, 256-bit lane inserts/extracts).
// When the guest executes one of these instructions, the CPU raises STATUS_ILLEGAL_INSTRUCTION (0xC000001D).
// The crash reporter's vectored exception handler catches it, emulates the instruction on the
// thread CONTEXT, and resumes execution.

#ifdef _WIN32
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace avx2_emu {

struct Vec256 {
    uint8_t b[32];

    uint32_t u32(std::size_t i) const {
        uint32_t v;
        std::memcpy(&v, &b[i * 4], 4);
        return v;
    }
    void set_u32(std::size_t i, uint32_t v) {
        std::memcpy(&b[i * 4], &v, 4);
    }
    uint64_t u64(std::size_t i) const {
        uint64_t v;
        std::memcpy(&v, &b[i * 8], 8);
        return v;
    }
    void set_u64(std::size_t i, uint64_t v) {
        std::memcpy(&b[i * 8], &v, 8);
    }
};

inline uint64_t GetGpr(const CONTEXT& ctx, unsigned reg) {
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

inline M128A* GetYmmHigh(CONTEXT& ctx) {
    using PFN_LocateXStateFeature = PVOID (WINAPI *)(PCONTEXT, DWORD, PDWORD);
    static auto pfnLocate = reinterpret_cast<PFN_LocateXStateFeature>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "LocateXStateFeature"));
    if (pfnLocate && (ctx.ContextFlags & CONTEXT_XSTATE)) {
        return reinterpret_cast<M128A*>(pfnLocate(&ctx, 2 /* XSTATE_AVX */, nullptr));
    }
    return nullptr;
}

inline Vec256 LoadVec(const CONTEXT& ctx, const M128A* ymmHigh, unsigned reg, bool is256) {
    Vec256 v{};
    std::memcpy(&v.b[0], &ctx.FltSave.XmmRegisters[reg & 15], 16);
    if (is256 && ymmHigh) {
        std::memcpy(&v.b[16], &ymmHigh[reg & 15], 16);
    }
    return v;
}

inline void StoreVec(CONTEXT& ctx, M128A* ymmHigh, unsigned reg, const Vec256& v, bool is256) {
    std::memcpy(&ctx.FltSave.XmmRegisters[reg & 15], &v.b[0], 16);
    if (ymmHigh) {
        if (is256) {
            std::memcpy(&ymmHigh[reg & 15], &v.b[16], 16);
        } else {
            std::memset(&ymmHigh[reg & 15], 0, 16);
        }
    }
}

inline bool DecodeAddress(const uint8_t* bytes, std::size_t available, std::size_t& cursor,
                          const CONTEXT& ctx, uint8_t modrm, uint8_t vex_x, uint8_t vex_b,
                          uint64_t& outAddr) {
    uint8_t mod = modrm >> 6;
    uint8_t rm = modrm & 7;
    uint8_t rm_reg = rm | ((~vex_b & 1) << 3);

    if (rm == 4) { // SIB byte present
        if (cursor >= available) return false;
        uint8_t sib = bytes[cursor++];
        uint8_t scale_shift = sib >> 6;
        uint8_t index_reg = ((sib >> 3) & 7) | ((~vex_x & 1) << 3);
        uint8_t base_reg = (sib & 7) | ((~vex_b & 1) << 3);

        uint64_t addr = 0;
        if (mod == 0 && (sib & 7) == 5) {
            if (cursor + 4 > available) return false;
            int32_t disp = 0;
            std::memcpy(&disp, &bytes[cursor], 4);
            cursor += 4;
            addr = static_cast<uint64_t>(static_cast<int64_t>(disp));
        } else {
            addr = GetGpr(ctx, base_reg);
        }

        if (index_reg != 4) {
            addr += (GetGpr(ctx, index_reg) << scale_shift);
        }

        if (mod == 1) {
            if (cursor >= available) return false;
            int8_t disp = static_cast<int8_t>(bytes[cursor++]);
            addr += disp;
        } else if (mod == 2) {
            if (cursor + 4 > available) return false;
            int32_t disp = 0;
            std::memcpy(&disp, &bytes[cursor], 4);
            cursor += 4;
            addr += disp;
        }
        outAddr = addr;
        return true;
    } else if (mod == 0 && rm == 5) { // RIP-relative
        if (cursor + 4 > available) return false;
        int32_t disp = 0;
        std::memcpy(&disp, &bytes[cursor], 4);
        cursor += 4;
        outAddr = ctx.Rip + cursor + disp;
        return true;
    } else {
        uint64_t addr = GetGpr(ctx, rm_reg);
        if (mod == 1) {
            if (cursor >= available) return false;
            int8_t disp = static_cast<int8_t>(bytes[cursor++]);
            addr += disp;
        } else if (mod == 2) {
            if (cursor + 4 > available) return false;
            int32_t disp = 0;
            std::memcpy(&disp, &bytes[cursor], 4);
            cursor += 4;
            addr += disp;
        }
        outAddr = addr;
        return true;
    }
}

inline bool Emulate(const uint8_t* bytes, std::size_t available, CONTEXT& ctx) {
    if (available < 5) return false;
    std::size_t cursor = 0;

    // We expect a 3-byte VEX prefix (0xC4)
    if (bytes[cursor++] != 0xC4) return false;

    uint8_t byte1 = bytes[cursor++];
    uint8_t vex_r = (byte1 >> 7) & 1;
    uint8_t vex_x = (byte1 >> 6) & 1;
    uint8_t vex_b = (byte1 >> 5) & 1;
    uint8_t map = byte1 & 0x1F;

    if (map != 2 && map != 3) return false; // 0F38 or 0F3A

    uint8_t byte2 = bytes[cursor++];
    uint8_t vex_w = (byte2 >> 7) & 1;
    uint8_t vvvv = (byte2 >> 3) & 0x0F;
    uint8_t vex_l = (byte2 >> 2) & 1; // 0: 128-bit, 1: 256-bit
    uint8_t pp = byte2 & 3;

    if (pp != 1) return false; // 0x66 prefix

    uint8_t opcode = bytes[cursor++];
    uint8_t modrm = bytes[cursor++];

    uint8_t mod = modrm >> 6;
    uint8_t reg = ((modrm >> 3) & 7) | ((~vex_r & 1) << 3);
    uint8_t rm_reg = (modrm & 7) | ((~vex_b & 1) << 3);
    uint8_t src1_reg = (~vvvv) & 0x0F;

    bool is256 = (vex_l != 0);
    M128A* ymmHigh = GetYmmHigh(ctx);

    Vec256 v1 = LoadVec(ctx, ymmHigh, src1_reg, is256);
    Vec256 v2{};

    uint64_t memAddr = 0;
    if (mod == 3) {
        v2 = LoadVec(ctx, ymmHigh, rm_reg, is256);
    } else {
        if (!DecodeAddress(bytes, available, cursor, ctx, modrm, vex_x, vex_b, memAddr)) {
            return false;
        }
        std::size_t loadSize = is256 ? 32 : 16;
        if (map == 2) {
            if (opcode == 0x22 || opcode == 0x32) {
                loadSize = is256 ? 4 : 2;
            } else if (opcode == 0x21 || opcode == 0x31 || opcode == 0x24 || opcode == 0x34) {
                loadSize = is256 ? 8 : 4;
            } else if (opcode == 0x20 || opcode == 0x30 || opcode == 0x23 || opcode == 0x33 || opcode == 0x25 || opcode == 0x35) {
                loadSize = is256 ? 16 : 8;
            } else if (opcode == 0x58) {
                loadSize = 4; // VPBROADCASTD reads a single dword
            } else if (opcode == 0x59) {
                loadSize = 8; // VPBROADCASTQ reads a single qword
            } else if (opcode == 0x78) {
                loadSize = 1; // VPBROADCASTB reads a single byte
            }
        }
        std::memcpy(&v2.b[0], reinterpret_cast<const void*>(memAddr), loadSize);
    }

    Vec256 res{};

    if (map == 2) { // 0F 38 map
        if (opcode == 0x16 && is256) { // VPERMPS (256-bit only)
            for (std::size_t i = 0; i < 8; ++i) {
                unsigned idx = v1.u32(i) & 7;
                res.set_u32(i, v2.u32(idx));
            }
            StoreVec(ctx, ymmHigh, reg, res, true);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x20) { // VPMOVSXBW
            std::size_t count = is256 ? 16 : 8;
            for (std::size_t i = 0; i < count; ++i) {
                int8_t sbyte = static_cast<int8_t>(v2.b[i]);
                int16_t sval = static_cast<int16_t>(sbyte);
                std::memcpy(&res.b[i * 2], &sval, 2);
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x21) { // VPMOVSXBD
            std::size_t count = is256 ? 8 : 4;
            for (std::size_t i = 0; i < count; ++i) {
                int8_t sbyte = static_cast<int8_t>(v2.b[i]);
                int32_t sval = static_cast<int32_t>(sbyte);
                res.set_u32(i, static_cast<uint32_t>(sval));
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x22) { // VPMOVSXBQ
            std::size_t count = is256 ? 4 : 2;
            for (std::size_t i = 0; i < count; ++i) {
                int8_t sbyte = static_cast<int8_t>(v2.b[i]);
                int64_t sval = static_cast<int64_t>(sbyte);
                res.set_u64(i, static_cast<uint64_t>(sval));
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x23) { // VPMOVSXWD
            std::size_t count = is256 ? 8 : 4;
            for (std::size_t i = 0; i < count; ++i) {
                int16_t sval = 0;
                std::memcpy(&sval, &v2.b[i * 2], 2);
                int32_t s32 = static_cast<int32_t>(sval);
                res.set_u32(i, static_cast<uint32_t>(s32));
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x24) { // VPMOVSXWQ
            std::size_t count = is256 ? 4 : 2;
            for (std::size_t i = 0; i < count; ++i) {
                int16_t sval = 0;
                std::memcpy(&sval, &v2.b[i * 2], 2);
                int64_t s64 = static_cast<int64_t>(sval);
                res.set_u64(i, static_cast<uint64_t>(s64));
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x25) { // VPMOVSXDQ
            std::size_t count = is256 ? 4 : 2;
            for (std::size_t i = 0; i < count; ++i) {
                int32_t sval = static_cast<int32_t>(v2.u32(i));
                int64_t s64 = static_cast<int64_t>(sval);
                res.set_u64(i, static_cast<uint64_t>(s64));
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x30) { // VPMOVZXBW
            std::size_t count = is256 ? 16 : 8;
            for (std::size_t i = 0; i < count; ++i) {
                uint16_t val = static_cast<uint16_t>(v2.b[i]);
                std::memcpy(&res.b[i * 2], &val, 2);
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x31) { // VPMOVZXBD
            std::size_t count = is256 ? 8 : 4;
            for (std::size_t i = 0; i < count; ++i) {
                res.set_u32(i, static_cast<uint32_t>(v2.b[i]));
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x32) { // VPMOVZXBQ
            std::size_t count = is256 ? 4 : 2;
            for (std::size_t i = 0; i < count; ++i) {
                res.set_u64(i, static_cast<uint64_t>(v2.b[i]));
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x33) { // VPMOVZXWD
            std::size_t count = is256 ? 8 : 4;
            for (std::size_t i = 0; i < count; ++i) {
                uint16_t val = 0;
                std::memcpy(&val, &v2.b[i * 2], 2);
                res.set_u32(i, static_cast<uint32_t>(val));
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x34) { // VPMOVZXWQ
            std::size_t count = is256 ? 4 : 2;
            for (std::size_t i = 0; i < count; ++i) {
                uint16_t val = 0;
                std::memcpy(&val, &v2.b[i * 2], 2);
                res.set_u64(i, static_cast<uint64_t>(val));
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x35) { // VPMOVZXDQ
            std::size_t count = is256 ? 4 : 2;
            for (std::size_t i = 0; i < count; ++i) {
                res.set_u64(i, static_cast<uint64_t>(v2.u32(i)));
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x45) { // VPSRLVD / VPSRLVQ
            if (vex_w == 0) { // dword
                std::size_t count = is256 ? 8 : 4;
                for (std::size_t i = 0; i < count; ++i) {
                    uint32_t shift = v2.u32(i);
                    res.set_u32(i, (shift < 32) ? (v1.u32(i) >> shift) : 0);
                }
            } else { // qword
                std::size_t count = is256 ? 4 : 2;
                for (std::size_t i = 0; i < count; ++i) {
                    uint64_t shift = v2.u64(i);
                    res.set_u64(i, (shift < 64) ? (v1.u64(i) >> shift) : 0);
                }
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x47) { // VPSLLVD / VPSLLVQ
            if (vex_w == 0) { // dword
                std::size_t count = is256 ? 8 : 4;
                for (std::size_t i = 0; i < count; ++i) {
                    uint32_t shift = v2.u32(i);
                    res.set_u32(i, (shift < 32) ? (v1.u32(i) << shift) : 0);
                }
            } else { // qword
                std::size_t count = is256 ? 4 : 2;
                for (std::size_t i = 0; i < count; ++i) {
                    uint64_t shift = v2.u64(i);
                    res.set_u64(i, (shift < 64) ? (v1.u64(i) << shift) : 0);
                }
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x46) { // VPSRAVD
            std::size_t count = is256 ? 8 : 4;
            for (std::size_t i = 0; i < count; ++i) {
                uint32_t shift = v2.u32(i);
                if (shift >= 32) shift = 31;
                int32_t val = static_cast<int32_t>(v1.u32(i));
                res.set_u32(i, static_cast<uint32_t>(val >> shift));
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x36 && is256) { // VPERMD (256-bit only)
            for (std::size_t i = 0; i < 8; ++i) {
                unsigned idx = v1.u32(i) & 7;
                res.set_u32(i, v2.u32(idx));
            }
            StoreVec(ctx, ymmHigh, reg, res, true);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x58) { // VPBROADCASTD: a dword fills every element
            const std::size_t count = is256 ? 8 : 4;
            for (std::size_t i = 0; i < count; ++i) res.set_u32(i, v2.u32(0));
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x59) { // VPBROADCASTQ: a qword fills every element
            const std::size_t count = is256 ? 4 : 2;
            for (std::size_t i = 0; i < count; ++i) res.set_u64(i, v2.u64(0));
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x78) { // VPBROADCASTB: a byte fills every element
            const std::size_t count = is256 ? 32 : 16;
            for (std::size_t i = 0; i < count; ++i) res.b[i] = v2.b[0];
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        }
    } else if (map == 3) { // 0F 3A map (instructions have an imm8 byte)
        if (cursor >= available) return false;
        uint8_t imm = bytes[cursor++];

        if (opcode == 0x00 && is256) { // VPERMQ (256-bit only)
            for (std::size_t i = 0; i < 4; ++i) {
                unsigned idx = (imm >> (i * 2)) & 3;
                res.set_u64(i, v2.u64(idx));
            }
            StoreVec(ctx, ymmHigh, reg, res, true);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x01 && is256) { // VPERMPD (256-bit only)
            for (std::size_t i = 0; i < 4; ++i) {
                unsigned idx = (imm >> (i * 2)) & 3;
                res.set_u64(i, v2.u64(idx));
            }
            StoreVec(ctx, ymmHigh, reg, res, true);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x02) { // VPBLENDD
            std::size_t count = is256 ? 8 : 4;
            for (std::size_t i = 0; i < count; ++i) {
                uint32_t val = ((imm >> i) & 1) ? v2.u32(i) : v1.u32(i);
                res.set_u32(i, val);
            }
            StoreVec(ctx, ymmHigh, reg, res, is256);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x46 && is256) { // VPERM2I128 (256-bit only)
            for (unsigned lane = 0; lane < 2; ++lane) {
                uint8_t ctrl = (imm >> (lane * 4)) & 0x0F;
                if (ctrl & 8) {
                    std::memset(&res.b[lane * 16], 0, 16);
                } else {
                    unsigned srcLane = ctrl & 3;
                    const uint8_t* srcPtr = nullptr;
                    if (srcLane == 0) srcPtr = &v1.b[0];
                    else if (srcLane == 1) srcPtr = &v1.b[16];
                    else if (srcLane == 2) srcPtr = &v2.b[0];
                    else srcPtr = &v2.b[16];
                    std::memcpy(&res.b[lane * 16], srcPtr, 16);
                }
            }
            StoreVec(ctx, ymmHigh, reg, res, true);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x38 && is256) { // VINSERTI128
            if ((imm & 1) == 0) {
                std::memcpy(&res.b[0], &v2.b[0], 16);
                std::memcpy(&res.b[16], &v1.b[16], 16);
            } else {
                std::memcpy(&res.b[0], &v1.b[0], 16);
                std::memcpy(&res.b[16], &v2.b[0], 16);
            }
            StoreVec(ctx, ymmHigh, reg, res, true);
            ctx.Rip += cursor;
            return true;
        } else if (opcode == 0x39 && is256) { // VEXTRACTI128
            // Encoding is MR: ModRM:reg is source YMM (r), ModRM:r/m is dest XMM/m128 (w).
            Vec256 srcVec = LoadVec(ctx, ymmHigh, reg, true);
            const uint8_t* srcPtr = ((imm & 1) == 0) ? &srcVec.b[0] : &srcVec.b[16];
            if (mod == 3) {
                std::memcpy(&res.b[0], srcPtr, 16);
                StoreVec(ctx, ymmHigh, rm_reg, res, false); // 128-bit dest, zeroes upper YMM
            } else {
                std::memcpy(reinterpret_cast<void*>(memAddr), srcPtr, 16);
            }
            ctx.Rip += cursor;
            return true;
        }
    }

    return false;
}

} // namespace avx2_emu
#endif
