#ifndef CODEGEN_X86_BMITYPES_HPP
#define CODEGEN_X86_BMITYPES_HPP

#include <cstddef>
#include <cstdint>

namespace Codegen {

enum class BmiOp : std::uint8_t {
    Shlx = 0,
    Shrx = 1,
    Sarx = 2,
    Andn = 3,
    Bextr = 4,
    Bzhi = 5,
    Rorx = 6,
    Lzcnt = 7,
    Tzcnt = 8,
};

// Decoded [SIB] memory operand of a BMI instruction. Full register numbers
// (0..15) are kept for REX.X/B decisions; raw low-3 codes feed ModRM/SIB.
struct BmiMemOperand {
    std::uint8_t Mod = 0;      // original ModRM mod bits (0,1,2)
    bool HasSib = false;
    int Base = -1;             // decoded base register (0..15), or -1
    int BaseCode = -1;         // raw low 3 bits used by ModRM/SIB, or -1
    int Index = -1;            // decoded index register (0..15), or -1
    int IndexCode = -1;        // raw low 3 bits, or -1 (4 means "none" in SIB)
    std::uint8_t Scale = 1;    // effective index scale multiplier (1,2,4,8)
    std::int32_t Disp = 0;     // displacement
    bool Rip = false;          // RIP-relative addressing
};

struct BmiMatch {
    BmiOp Op = BmiOp::Shlx;
    std::size_t Length = 0;    // full instruction length in bytes
    bool Wide = false;         // 64-bit (REX.W / VEX.W) operand size
    bool IsShort = false;      // 4-byte legacy site that cannot host E9 dispatch
    std::uint8_t Dst = 0;      // decoded destination register (0..15)
    std::uint8_t Src = 0;      // decoded source register (register form only)
    std::uint8_t Third = 0;    // control register for shlx/shrx/sarx/bextr/bzhi/andn
    std::uint8_t Imm8 = 0;     // rorx immediate
    bool IsMemory = false;     // source is a memory operand
    BmiMemOperand Mem;
};

}

#endif