#ifndef CODEGEN_X86_AVX2TYPES_HPP
#define CODEGEN_X86_AVX2TYPES_HPP

#include <codegen/x86/BmiTypes.hpp>

#include <cstddef>
#include <cstdint>

namespace Codegen {

// Emulation shapes. Each shape maps to a small, hand-written code sequence that
// reproduces the 256-bit result using only AVX1/SSE2, so it runs on a CPU with
// no AVX2. Shapes were chosen to cover the instruction mix the relinker
// actually finds, verified empirically on an Ivy Bridge host.
enum class Avx2Op : std::uint8_t {
    None = 0,

    // ---- shape A: dst = OP(src1, src2), elementwise, same op at 128 bits ----
    And = 1,          // VPAND   0F DB  /r
    Andn = 2,         // VPANDN  0F DB  /r
    Or = 3,           // VPOR    0F EB  /r
    Xor = 4,          // VPXOR   0F EF  /r
    AddB = 5,         // VPADDB  0F FC  /r
    AddW = 6,         // VPADDW  0F FD  /r
    AddD = 7,         // VPADDD  0F FE  /r
    AddQ = 8,         // VPADDQ  0F D4  /r   (W=1)
    SubB = 9,         // VPSUBB  0F F8  /r
    SubW = 10,        // VPSUBW  0F F9  /r
    SubD = 11,        // VPSUBD  0F FA  /r
    SubQ = 12,        // VPSUBQ  0F FB  /r   (W=1)
    CmpEqB = 13,      // VPCMPEQB 0F 74 /r
    CmpEqW = 14,      // VPCMPEQW 0F 75 /r
    CmpEqD = 15,      // VPCMPEQD 0F 76 /r
    CmpEqQ = 16,      // VPCMPEQQ 0F 76 /r   (W=1)
    CmpGtB = 17,      // VPCMPGTB 0F 64 /r
    CmpGtW = 18,      // VPCMPGTW 0F 65 /r
    CmpGtD = 19,      // VPCMPGTD 0F 66 /r
    CmpGtQ = 20,      // VPCMPGTQ 0F38 37 /r (W=1)
    MinU = 21,        // VPMINUD 0F38 3B /r
    MaxU = 22,        // VPMAXUD 0F38 3F /r
    MulUdq = 23,      // VPMULUDQ 0F F4 /r  (W=1)
    MulD = 24,        // VPMULLD 0F38 40 /r
    AbsD = 25,        // VPABSD  0F38 1E /r  (unary)
    Sadbw = 26,       // VPSADBW 0F F6 /r
    Shufb = 27,       // VPSHUFB 0F38 00 /r
    Shufd = 28,       // VPSHUFD 0F 70 /r ib
    Alignr = 29,      // VPALIGNR 0F3A 0F /r ib

    // ---- shape D: dst = SHIFT(src1, imm8) ----
    ShiftLImm = 30,   // VPSLLD/W/Q  0F 72 /6, 0F 71 /6, 0F 73 /6
    ShiftRImm = 31,   // VPSRLD/W/Q  0F 72 /2, 0F 71 /2, 0F 73 /2
    ShiftRImmArith = 32, // VPSRAD 0F 72 /4
    Blendd = 33,      // VPBLENDD 0F3A 02 /r ib (illegal at 128 bits too)

    // ---- shape C: dst = BROADCAST(src) ----
    BroadcastB = 40,  // VPBROADCASTB 0F38 78 /r
    BroadcastW = 41,  // VPBROADCASTW 0F38 79 /r
    BroadcastD = 42,  // VPBROADCASTD 0F38 7A /r
    BroadcastQ = 43,  // VPBROADCASTQ 0F38 7B /r (W=1)
    BroadcastSs = 44, // VBROADCASTSS 0F38 18 /r  (reg source only)
    BroadcastSd = 45, // VBROADCASTSD 0F38 19 /r  (reg source only)
};

// What the stub needs in order to re-emit the instruction at 128 bits, plus the
// operand facts the code sequences depend on.
struct Avx2Match {
    Avx2Op Op = Avx2Op::None;
    std::size_t Length = 0;
    bool UseYmm = false;    // VEX.L: the 256-bit form is the illegal one
    std::uint8_t Dst = 0;   // ModRM.reg (+REX.R)
    std::uint8_t Src1 = 0;  // VEX.vvvv
    std::uint8_t Src2 = 0;  // ModRM.rm, register form only
    std::uint8_t Imm8 = 0;
    bool IsMemory = false;
    BmiMemOperand Mem;      // decoded [SIB] operand, reused from the BMI decoder

    // Encoding bits needed to rebuild the same opcode at VEX.L=0.
    std::uint8_t Map = 0;   // VEX.mmmmm: 1=0F, 2=0F38, 3=0F3A
    std::uint8_t Pp = 0;    // VEX.pp
    std::uint8_t Opcode = 0;
    bool Wide = false;      // VEX.W
    std::uint8_t Digits = 0; // /digit in ModRM.reg for shift groups
};

}

#endif
