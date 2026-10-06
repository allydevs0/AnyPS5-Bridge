#ifndef CODEGEN_X86_IBMI_STUBEMITTER_HPP
#define CODEGEN_X86_IBMI_STUBEMITTER_HPP

#include <codegen/x86/BmiTypes.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace Codegen {

class IBmiStubEmitter {
public:
    virtual ~IBmiStubEmitter() = default;

    // Emits the self-contained native emulation template for `match`
    // (balanced push/pop, correct flag semantics), identical to the
    // templates validated at 1163/1163 in the native harness.
    [[nodiscard]] virtual std::vector<std::uint8_t> EmitTemplate(const BmiMatch& match) const = 0;

    // EmitTemplate followed by an E9 rel32 branch back to the instruction
    // that follows the dispatch site. `rel32Offset` receives the offset of
    // the 32-bit displacement field (bytes.size() - 4), which the PE
    // patcher fills with the final target RVA.
    [[nodiscard]] virtual std::vector<std::uint8_t> EmitDispatch(
        const BmiMatch& match,
        std::size_t& rel32Offset
    ) const = 0;

    // EmitTemplate terminated with C3 ret, for the 4-byte legacy sites
    // driven by the runtime VEH bridge (handler pushes the resume address).
    [[nodiscard]] virtual std::vector<std::uint8_t> EmitShort(const BmiMatch& match) const = 0;
};

std::unique_ptr<IBmiStubEmitter> MakeBmiStubEmitter();

}

#endif