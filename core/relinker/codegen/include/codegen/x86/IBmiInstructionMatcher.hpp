#ifndef CODEGEN_X86_IBMIINSTRUCTIONMATCHER_HPP
#define CODEGEN_X86_IBMIINSTRUCTIONMATCHER_HPP

#include <codegen/x86/BmiTypes.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace Codegen {

class IBmiInstructionMatcher {
public:
    virtual ~IBmiInstructionMatcher() = default;

    // Decodes a single BMI1/BMI2 instruction at `data` whose total length
    // must equal `length`. Returns std::nullopt when `data` does not start
    // with a matching VEX3 (C4 ...) or legacy (F3 0F BD/BC) instruction.
    [[nodiscard]] virtual std::optional<BmiMatch> Match(
        const std::uint8_t* data,
        std::size_t length
    ) const = 0;
};

std::unique_ptr<IBmiInstructionMatcher> MakeBmiInstructionMatcher();

}

#endif