#ifndef CODEGEN_IBMICOMPATIBILITYCONVERTER_HPP
#define CODEGEN_IBMICOMPATIBILITYCONVERTER_HPP

#include <domain/Types.hpp>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace Codegen {

// A single rewritten BMI instruction inside a code segment. SegmentAddress
// is the PT_LOAD mapped vaddr of the containing segment; SegmentOffset is
// the offset of the site within that segment, so the runtime address is
// SegmentAddress + SegmentOffset (the file offset is the same relative).
struct BmiSiteRecord {
    std::uint64_t SegmentAddress = 0;
    std::uint32_t SegmentOffset = 0;
    std::uint32_t Length = 0;
    std::uint32_t StubIndex = 0;
    std::uint8_t SrcReg = 0; // decoded source register (legacy no-REX: 0..7)
    std::uint8_t DstReg = 0; // decoded destination register
};

// A fully formed emulation stub. Dispatch stubs carry an E9 rel32 tail
// (branches back to the instruction after the site); TailRel32Offset is the
// offset of its displacement field, or kNoTailRel32 when the stub ends in a
// plain C3 (the short 4-byte sites driven by the runtime VEH bridge).
struct BmiStubRecord {
    std::vector<std::uint8_t> Bytes;
    std::uint32_t TailRel32Offset = 0;

    static constexpr std::uint32_t kNoTailRel32 = std::numeric_limits<std::uint32_t>::max();

    // Offset of a RIP-relative disp32 the stub copied from the original
    // instruction, or kNoTailRel32. The stub lives at a different address than
    // the site, so the patcher has to rebase it: the original displacement was
    // measured from (site + length), this one from (stub + offset + 4).
    std::uint32_t RipDisp32Offset = kNoTailRel32;
};

struct BmiConvertResult {
    // The ELF with every dispatch site (length >= 5) replaced in place by
    // E9 00000000 90*; the 4-byte legacy sites are left untouched.
    std::vector<std::uint8_t> Bytes;
    std::size_t DispatchCount = 0;
    std::size_t ShortCount = 0;
    // Short sites whose VEX.pp was rewritten so the encoding raises #UD. These
    // are the ones that would otherwise never reach the VEH -- lzcnt and tzcnt
    // decode as bsr/bsf on a CPU without LZCNT/TZCNT -- and so would silently
    // compute a wrong result.
    std::size_t ForcedTrapCount = 0;
    // AVX2 sites converted with the same in-place E9 dispatch. AVX2
    // instructions are all 4 bytes or longer; only the >= 5 byte ones can host
    // the rel32, so the 4-byte VEX2 reg/reg forms are reported in
    // Avx2ShortCount and left for a separate strategy.
    std::size_t Avx2Count = 0;
    std::size_t Avx2ShortCount = 0;
    std::size_t Avx2RipCount = 0;
    // Diagnostic: VEX sites the matcher did not claim, keyed by
    // (prefix | map | opcode | decoded length) so it is obvious whether the
    // sweep produced a wrong length or the opcode is simply unimplemented.
    std::map<std::uint32_t, std::size_t> Avx2Unmatched;
    // First few unmatched VEX sites, raw bytes included, so the encoding can be
    // disassembled and compared against the length the sweep reported.
    struct Avx2Sample {
        std::uint32_t Length;
        std::uint8_t Bytes[12];
    };
    std::vector<Avx2Sample> Avx2UnmatchedSamples;
    // Dispatch stubs first (in DispatchSites order), then short stubs.
    std::vector<BmiStubRecord> StubBlobs;
    std::vector<BmiSiteRecord> DispatchSites;
    std::vector<BmiSiteRecord> ShortSites;
};

class IBmiCompatibilityConverter {
public:
    virtual ~IBmiCompatibilityConverter() = default;

    // Rewrites every BMI1/BMI2 instruction in the executable segments to a
    // same-length E9 dispatch (sites >= 5 bytes) and reports the 4-byte
    // legacy sites that must be handled by a runtime VEH bridge. The stub
    // blobs carry placeholder rel32s that the PE patcher resolves once the
    // output section layout (and therefore every RVA) is known.
    [[nodiscard]] virtual BmiConvertResult Convert(
        std::vector<std::uint8_t> fileBytes,
        const std::vector<Domain::ProgramHeader>& codeSegments
    ) const = 0;
};

std::unique_ptr<IBmiCompatibilityConverter> MakeBmiCompatibilityConverter();

}

#endif