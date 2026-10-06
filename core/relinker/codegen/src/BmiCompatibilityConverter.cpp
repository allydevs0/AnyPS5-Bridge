#include <codegen/IBmiCompatibilityConverter.hpp>
#include <codegen/IInstructionScanner.hpp>
#include <codegen/x86/IAvx2InstructionMatcher.hpp>
#include <codegen/x86/IAvx2StubEmitter.hpp>
#include <codegen/x86/IBmiInstructionMatcher.hpp>
#include <codegen/x86/IBmiStubEmitter.hpp>

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace Codegen {

namespace {

// Most BMI1 forms raise #UD on this CPU, so the VEH catches the fault and
// redirects to the stub. lzcnt and tzcnt do not: they decode as bsr/bsf, so
// those sites silently run the legacy instruction, leaving the destination
// untouched when the input is zero and not clearing CF.
//
// The fix is to rewrite VEX.pp to 11, which turns the encoding into one that is
// not assigned (F2 0F F5 / F2 0F 38 F1) and therefore does raise #UD. Only pp
// changes, so the site keeps its length.
//
// Re-enabled 2026-10-02. The `ret`-hijack resume in BmiVehHandler was NOT broken:
// WindowsVehDispatchTests raises a real #UD at a synthetic site and passes at both
// a small (0x1000000) and a wrapping >4GB (0x140000000) PEB image base. An
// earlier attempt to replace the hijack with a CONTEXT.Rip/Rsp rewrite was wrong
// -- it re-executed the faulting site and the dispatch test caught it, so the
// hijack was restored. The handler is now covered by that test.
constexpr bool kForceTrap = true;

class BmiCompatibilityConverter : public IBmiCompatibilityConverter {
public:
    BmiCompatibilityConverter()
        : _scanner(MakeInstructionScanner()),
          _matcher(MakeBmiInstructionMatcher()),
          _emitter(MakeBmiStubEmitter()),
          _avx2Matcher(MakeAvx2InstructionMatcher()),
          _avx2Emitter(MakeAvx2StubEmitter()) {}

    BmiConvertResult Convert(
        std::vector<std::uint8_t> fileBytes,
        const std::vector<Domain::ProgramHeader>& codeSegments
    ) const override {
        BmiConvertResult result;
        result.Bytes = std::move(fileBytes);

        for (const auto& ph : codeSegments) {
            ConvertSegment(result, ph);
        }
        return result;
    }

private:
    void ConvertSegment(BmiConvertResult& out, const Domain::ProgramHeader& ph) const {
        const auto segOffset = static_cast<std::size_t>(ph.Offset);
        const auto segSize = static_cast<std::size_t>(ph.FileSize);
        if (segOffset > out.Bytes.size() || segSize > out.Bytes.size() - segOffset) {
            return;
        }

        std::vector<std::uint8_t> seg(
            out.Bytes.begin() + static_cast<std::ptrdiff_t>(segOffset),
            out.Bytes.begin() + static_cast<std::ptrdiff_t>(segOffset + segSize));

        const auto matches = _scanner->ScanCodeSection(seg, segOffset, segSize);
        for (const auto& m : matches) {
            const auto offset = static_cast<std::size_t>(m.Offset) - segOffset;            const auto length = m.Length;
            if (length == 0 || offset > seg.size() || length > seg.size() - offset) {
                continue;
            }

            const auto match = _matcher->Match(seg.data() + offset, length);

            if (!match) {
                // Not a BMI1/BMI2 form; it may still be a VEX.256 integer op
                // that this CPU cannot execute.
                const auto avx2 = _avx2Matcher->Match(seg.data() + offset, length);
                if (!avx2) {
                    const std::uint8_t p0 = seg[offset];
                    if (p0 == 0xC4 || p0 == 0xC5) {
                        const std::uint8_t map = (p0 == 0xC4) ? static_cast<std::uint8_t>(seg[offset + 1] & 0x1F) : 1;
                        const std::size_t opIdx = (p0 == 0xC4) ? 3 : 2;
                        const std::uint32_t key = (static_cast<std::uint32_t>(p0) << 24)
                                                | (static_cast<std::uint32_t>(map) << 16)
                                                | (static_cast<std::uint32_t>(seg[offset + opIdx]) << 8)
                                                | static_cast<std::uint32_t>(length & 0xFF);
                        ++out.Avx2Unmatched[key];
                        // Sample only opcodes that are genuinely part of the
                        // AVX2 set, otherwise the samples fill up with bytes
                        // from a sweep that lost sync on a data table.
                        static const std::vector<std::uint8_t> kAvx2Opcodes{0x71, 0x72, 0x73, 0x70};
                        const std::uint8_t opByte = seg[offset + opIdx];
                        const bool interesting = std::find(std::begin(kAvx2Opcodes), std::end(kAvx2Opcodes), opByte)
                            != std::end(kAvx2Opcodes);
                        if (interesting && length >= 5 && out.Avx2UnmatchedSamples.size() < 40) {
                            BmiConvertResult::Avx2Sample sample{};
                            sample.Length = static_cast<std::uint32_t>(length);
                            const std::size_t copy = std::min<std::size_t>(12, seg.size() - offset);
                            std::memcpy(sample.Bytes, seg.data() + offset, copy);
                            out.Avx2UnmatchedSamples.push_back(sample);
                        }
                    }
                    continue;
                }
                if (length < 5) {
                    // A 4-byte VEX2 reg/reg site cannot host an E9 rel32, so it
                    // cannot be dispatched by an in-place jump. Previously these
                    // were only counted and then dropped, which left them as live
                    // AVX2 in the image -- the guest then took #UD on them at boot
                    // (a plain `vpaddd`, c5 fd fe e9, was observed doing exactly
                    // that) and the emulation stopped there.
                    //
                    // They go into the same short-site table the BMI sites use:
                    // VEX.pp is rewritten to an unassigned encoding so the site
                    // keeps its length and raises #UD, and the VEH handler (which
                    // WindowsVehDispatchTests exercises against a real #UD at both
                    // a small and a wrapping >4GB image base) matches the faulting
                    // address and resumes into the stub.
                    const std::uint8_t p0 = seg[offset];
                    const std::size_t ppOffset = (p0 == 0xC4) ? offset + 2 : offset + 1;
                    if (ppOffset < seg.size()) {
                        seg[ppOffset] = static_cast<std::uint8_t>((seg[ppOffset] & 0xFC) | 0x03);
                    }

                    BmiSiteRecord shortSite;
                    shortSite.SegmentAddress = ph.MappedAddress;
                    shortSite.SegmentOffset = static_cast<std::uint32_t>(offset);
                    shortSite.Length = static_cast<std::uint32_t>(length);
                    shortSite.StubIndex = static_cast<std::uint32_t>(out.StubBlobs.size());

                    std::size_t shortRel32 = 0;
                    std::size_t shortRip = 0;
                    auto shortBlob = _avx2Emitter->EmitShort(*avx2, shortRel32, shortRip);
                    BmiStubRecord shortRecord{std::move(shortBlob), BmiStubRecord::kNoTailRel32};
                    if (shortRip != static_cast<std::size_t>(-1)) {
                        shortRecord.RipDisp32Offset = static_cast<std::uint32_t>(shortRip);
                        ++out.Avx2RipCount;
                    }
                    out.StubBlobs.push_back(std::move(shortRecord));
                    out.ShortSites.push_back(shortSite);
                    ++out.Avx2Count;
                    ++out.Avx2ShortCount;
                    continue;
                }

                std::size_t avx2Rel32 = 0;
                std::size_t avx2Rip = 0;
                auto avx2Blob = _avx2Emitter->EmitDispatch(*avx2, avx2Rel32, avx2Rip);
                BmiSiteRecord avx2Site;
                avx2Site.SegmentAddress = ph.MappedAddress;
                avx2Site.SegmentOffset = static_cast<std::uint32_t>(offset);
                avx2Site.Length = static_cast<std::uint32_t>(length);
                avx2Site.StubIndex = static_cast<std::uint32_t>(out.StubBlobs.size());
                BmiStubRecord record{std::move(avx2Blob), static_cast<std::uint32_t>(avx2Rel32)};
                if (avx2Rip != static_cast<std::size_t>(-1)) {
                    record.RipDisp32Offset = static_cast<std::uint32_t>(avx2Rip);
                    ++out.Avx2RipCount;
                }
                out.StubBlobs.push_back(std::move(record));
                out.DispatchSites.push_back(avx2Site);
                ++out.Avx2Count;

                seg[offset] = 0xE9;
                for (std::size_t i = 1; i < length; ++i) {
                    seg[offset + i] = 0x90;
                }
                continue;
            }

            BmiSiteRecord site;
            site.SegmentAddress = ph.MappedAddress;
            site.SegmentOffset = static_cast<std::uint32_t>(offset);
            site.Length = static_cast<std::uint32_t>(length);
            site.SrcReg = match->Src & 0x07;
            site.DstReg = match->Dst & 0x07;

            if (match->IsShort && length == 4) {
                site.StubIndex = static_cast<std::uint32_t>(out.StubBlobs.size());
                out.StubBlobs.push_back({_emitter->EmitShort(*match), BmiStubRecord::kNoTailRel32});
                out.ShortSites.push_back(site);
                ++out.ShortCount;
                // See kForceTrap above: this rewrite is the fix for lzcnt/tzcnt
                // not trapping, and it is gated on the VEH found path working.
                if (kForceTrap && (match->Op == BmiOp::Lzcnt || match->Op == BmiOp::Tzcnt)) {
                    const std::size_t ppOffset = (seg[offset] == 0xC4) ? offset + 2 : offset + 1;
                    seg[ppOffset] = static_cast<std::uint8_t>((seg[ppOffset] & 0xFC) | 3);
                    ++out.ForcedTrapCount;
                }
                continue;
            }

            if (match->IsShort || length < 5) {
                continue;
            }

            std::size_t rel32Offset = 0;
            auto blob = _emitter->EmitDispatch(*match, rel32Offset);
            site.StubIndex = static_cast<std::uint32_t>(out.StubBlobs.size());
            out.StubBlobs.push_back({std::move(blob), static_cast<std::uint32_t>(rel32Offset)});
            out.DispatchSites.push_back(site);
            ++out.DispatchCount;

            // Rewrite the site in place to E9 00000000 90* (same length).
            seg[offset] = 0xE9;
            for (std::size_t i = 1; i < length; ++i) {
                seg[offset + i] = 0x90;
            }
        }

        std::memcpy(out.Bytes.data() + segOffset, seg.data(), seg.size());
    }

    std::unique_ptr<IInstructionScanner> _scanner;
    std::unique_ptr<IBmiInstructionMatcher> _matcher;
    std::unique_ptr<IBmiStubEmitter> _emitter;
    std::unique_ptr<IAvx2InstructionMatcher> _avx2Matcher;
    std::unique_ptr<IAvx2StubEmitter> _avx2Emitter;
};

}

std::unique_ptr<IBmiCompatibilityConverter> MakeBmiCompatibilityConverter() {
    return std::make_unique<BmiCompatibilityConverter>();
}

}