#ifndef CODEGEN_X86_IAVX2STUBEMITTER_HPP
#define CODEGEN_X86_IAVX2STUBEMITTER_HPP

#include <codegen/x86/Avx2Types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Codegen {

class IAvx2StubEmitter {
public:
    virtual ~IAvx2StubEmitter() = default;

    // Emits the emulation followed by an E9 rel32 tail whose displacement lives
    // at `rel32Offset`; the tail is patched to the instruction after the site.
    [[nodiscard]] virtual std::vector<std::uint8_t> EmitDispatch(
        const Avx2Match& match,
        std::size_t& rel32Offset,
        std::size_t& ripDisp32Offset
    ) const = 0;

    // Emits the same emulation, but ends in `ret` instead of the E9 rel32 tail,
    // for sites that cannot host a 5-byte jump and are therefore resumed through
    // the VEH. The handler enters such a stub with the guest's next address on the
    // stack, so `ret` is what continues execution; `rel32Offset` is set to
    // kNoTailRel32 because there is no displacement to patch.
    [[nodiscard]] virtual std::vector<std::uint8_t> EmitShort(
        const Avx2Match& match,
        std::size_t& rel32Offset,
        std::size_t& ripDisp32Offset
    ) const = 0;
};

[[nodiscard]] std::unique_ptr<IAvx2StubEmitter> MakeAvx2StubEmitter();

}

#endif
