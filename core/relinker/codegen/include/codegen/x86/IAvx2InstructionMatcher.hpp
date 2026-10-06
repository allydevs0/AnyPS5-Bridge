#ifndef CODEGEN_X86_IAVX2INSTRUCTIONMATCHER_HPP
#define CODEGEN_X86_IAVX2INSTRUCTIONMATCHER_HPP

#include <codegen/x86/Avx2Types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace Codegen {

class IAvx2InstructionMatcher {
public:
    virtual ~IAvx2InstructionMatcher() = default;

    // Recognises exactly the VEX-encoded forms an Ivy Bridge cannot execute.
    // `length` is the full instruction length reported by the scanner, so the
    // match is only accepted when the encoding is consumed exactly.
    [[nodiscard]] virtual std::optional<Avx2Match> Match(
        const std::uint8_t* data,
        std::size_t length
    ) const = 0;
};

[[nodiscard]] std::unique_ptr<IAvx2InstructionMatcher> MakeAvx2InstructionMatcher();

}

#endif
