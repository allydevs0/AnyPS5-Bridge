#pragma once
// Emulation of MOVBE on Intel CPUs lacking MOVBE (e.g. Intel Core i5-3570K / Ivy Bridge).
//
// The PS5's Zen 2 CPU has MOVBE (Move Data After Swapping Bytes), but Ivy Bridge raises
// STATUS_ILLEGAL_INSTRUCTION (0xc000001d). The vectored exception handler intercepts it,
// swaps the bytes, updates the CONTEXT, and resumes execution.
#ifdef _WIN32
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace movbe_emu {

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

inline void SetGpr(CONTEXT& ctx, unsigned reg, uint64_t val, int sizeInBytes) {
    auto update = [&](DWORD64& r) {
        if (sizeInBytes == 8) {
            r = val;
        } else if (sizeInBytes == 4) {
            r = static_cast<uint32_t>(val); // Zero-extends 32-bit result to 64 bits
        } else if (sizeInBytes == 2) {
            r = (r & ~0xffffULL) | (val & 0xffffULL); // Preserves upper 48 bits
        }
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

inline uint16_t Swap16(uint16_t x) {
    return static_cast<uint16_t>((x << 8) | (x >> 8));
}

inline uint32_t Swap32(uint32_t x) {
    return __builtin_bswap32(x);
}

inline uint64_t Swap64(uint64_t x) {
    return __builtin_bswap64(x);
}

inline bool Emulate(const uint8_t* bytes, std::size_t available, CONTEXT& ctx) {
    if (available < 3) return false;
    std::size_t cursor = 0;

    bool has66 = false;
    bool has67 = false;
    uint8_t rex = 0;

    // Decode legacy prefixes & REX
    while (cursor < available) {
        uint8_t b = bytes[cursor];
        if (b == 0x66) {
            has66 = true;
            rex = 0;
            cursor++;
        } else if (b == 0x67) {
            has67 = true;
            rex = 0;
            cursor++;
        } else if (b >= 0x40 && b <= 0x4F) {
            rex = b;
            cursor++;
        } else {
            break;
        }
    }

    if (cursor + 3 > available) return false;

    // Check opcode: 0F 38 F0 or 0F 38 F1
    if (bytes[cursor] != 0x0F || bytes[cursor + 1] != 0x38) return false;
    uint8_t opcode = bytes[cursor + 2];
    if (opcode != 0xF0 && opcode != 0xF1) return false;
    cursor += 3;

    if (cursor >= available) return false;
    uint8_t modrm = bytes[cursor++];
    uint8_t mod = modrm >> 6;
    uint8_t reg_field = (modrm >> 3) & 7;
    uint8_t rm = modrm & 7;

    if (mod == 3) {
        // MOVBE does not support register-to-register forms
        return false;
    }

    uint8_t rex_w = (rex >> 3) & 1;
    uint8_t rex_r = (rex >> 2) & 1;
    uint8_t rex_x = (rex >> 1) & 1;
    uint8_t rex_b = rex & 1;

    unsigned reg = reg_field | (rex_r << 3);
    unsigned rm_reg = rm | (rex_b << 3);

    int opSize = 4;
    if (rex_w) opSize = 8;
    else if (has66) opSize = 2;

    uint64_t addr = 0;
    if (rm == 4) { // SIB byte present
        if (cursor >= available) return false;
        uint8_t sib = bytes[cursor++];
        uint8_t scale_shift = sib >> 6;
        uint8_t index_reg = ((sib >> 3) & 7) | (rex_x << 3);
        uint8_t base_reg = (sib & 7) | (rex_b << 3);

        if (mod == 0 && (sib & 7) == 5) {
            if (cursor + 4 > available) return false;
            int32_t disp = 0;
            std::memcpy(&disp, &bytes[cursor], 4);
            cursor += 4;
            addr = static_cast<uint64_t>(static_cast<int64_t>(disp));
        } else {
            addr = GetGpr(ctx, base_reg);
        }

        if (((sib >> 3) & 7) != 4 || rex_x != 0) {
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
    } else if (mod == 0 && rm == 5) { // RIP-relative
        if (cursor + 4 > available) return false;
        int32_t disp = 0;
        std::memcpy(&disp, &bytes[cursor], 4);
        cursor += 4;
        addr = ctx.Rip + cursor + disp;
    } else {
        addr = GetGpr(ctx, rm_reg);
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
    }

    if (has67) {
        addr &= 0xFFFFFFFFULL;
    }

    if (opcode == 0xF0) { // MOVBE reg, mem (Load)
        if (opSize == 8) {
            uint64_t val = 0;
            std::memcpy(&val, reinterpret_cast<const void*>(addr), 8);
            SetGpr(ctx, reg, Swap64(val), 8);
        } else if (opSize == 4) {
            uint32_t val = 0;
            std::memcpy(&val, reinterpret_cast<const void*>(addr), 4);
            SetGpr(ctx, reg, Swap32(val), 4);
        } else { // 2
            uint16_t val = 0;
            std::memcpy(&val, reinterpret_cast<const void*>(addr), 2);
            SetGpr(ctx, reg, Swap16(val), 2);
        }
    } else { // 0xF1: MOVBE mem, reg (Store)
        uint64_t val = GetGpr(ctx, reg);
        if (opSize == 8) {
            uint64_t sw = Swap64(val);
            std::memcpy(reinterpret_cast<void*>(addr), &sw, 8);
        } else if (opSize == 4) {
            uint32_t sw = Swap32(static_cast<uint32_t>(val));
            std::memcpy(reinterpret_cast<void*>(addr), &sw, 4);
        } else { // 2
            uint16_t sw = Swap16(static_cast<uint16_t>(val));
            std::memcpy(reinterpret_cast<void*>(addr), &sw, 2);
        }
    }

    ctx.Rip += cursor;
    return true;
}

} // namespace movbe_emu
#endif
