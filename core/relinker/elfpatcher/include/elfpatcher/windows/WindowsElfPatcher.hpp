#ifndef ELFPATCHER_WINDOWS_WINDOWSELFPATCHER_HPP
#define ELFPATCHER_WINDOWS_WINDOWSELFPATCHER_HPP

#include <elfpatcher/general/IElfPatcher.hpp>
#include <filesystem>

namespace Codegen {
struct BmiConvertResult;
}

namespace Elfpatcher::Windows {

class WindowsPePatcher : public IElfPatcher {
public:
    explicit WindowsPePatcher(bool windowsGui = false, std::filesystem::path iconPath = {});

    std::vector<std::uint8_t> Patch(const std::vector<std::uint8_t>& sourceElf, const std::vector<Domain::ProgramHeader>& originalHeaders, const Domain::SysVDynamicSection& dynamicSection, std::uint64_t originalPltGotVaddr, const std::string& runPath, bool lazyBinding, bool dependencyDiagnostics, const std::vector<Codegen::TrampolineSite>& trampolines) override;

    std::vector<std::uint8_t> PatchWithBmi(const std::vector<std::uint8_t>& sourceElf, const std::vector<Domain::ProgramHeader>& originalHeaders, const Domain::SysVDynamicSection& dynamicSection, std::uint64_t originalPltGotVaddr, const std::string& runPath, bool lazyBinding, bool dependencyDiagnostics, const std::vector<Codegen::TrampolineSite>& trampolines, const Codegen::BmiConvertResult& bmi) const;

private:
    std::vector<std::uint8_t> patchImpl(bool bmiEnabled, const Codegen::BmiConvertResult* bmi, const std::vector<std::uint8_t>& sourceElf, const std::vector<Domain::ProgramHeader>& originalHeaders, const Domain::SysVDynamicSection& dynamicSection, std::uint64_t originalPltGotVaddr, const std::string& runPath, bool lazyBinding, bool dependencyDiagnostics, const std::vector<Codegen::TrampolineSite>& trampolines) const;

    bool _windowsGui;
    std::filesystem::path _iconPath;
};

}

#endif
