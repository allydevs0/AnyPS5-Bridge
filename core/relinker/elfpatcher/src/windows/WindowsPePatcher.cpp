#include <elfpatcher/windows/WindowsElfPatcher.hpp>
#include <elfpatcher/windows/BmiVehHandler.hpp>
#include <elfpatcher/windows/WindowsEntryStubBuilder.hpp>
#include <elfpatcher/windows/WindowsIconResourceBuilder.hpp>
#include <elfpatcher/windows/WindowsLoadImage.hpp>
#include <elfpatcher/windows/WindowsPeWriter.hpp>
#include <elfpatcher/windows/WindowsRelocationBuilder.hpp>
#include <elfpatcher/windows/WindowsTlsBuilder.hpp>
#include <elfpatcher/windows/WindowsTrampolineBuilder.hpp>
#include <codegen/IBmiCompatibilityConverter.hpp>
#include <io/BufferUtils.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>

namespace Elfpatcher::Windows {

namespace {

void writeDiagnosticsImports(const std::vector<PeImport>& imports) {
    const std::filesystem::path path = std::filesystem::absolute("windows-diagnostics-imports.txt");
    std::ofstream stream(path, std::ios::trunc);
    if (!stream)
        throw Domain::RelinkerException("Cannot open windows-diagnostics-imports.txt for writing");
    for (const auto& import : imports)
        stream << import.Name << '\n';
    if (!stream)
        throw Domain::RelinkerException("Cannot write windows-diagnostics-imports.txt");
    stream.close();
    std::cout << "Wrote Windows import diagnostics to " << path.string() << '\n';
}

void writeGotStub(std::vector<PeSection>& sections, const std::uint32_t targetRva, const std::uint32_t stubRva) {
    for (auto& section : sections) {
        if (section.Data.size() < 8 || targetRva < section.Rva || targetRva - section.Rva > section.Data.size() - 8)
            continue;
        Io::WriteU64(section.Data, targetRva - section.Rva, ImageBase + stubRva);
        return;
    }
    throw Domain::RelinkerException("Lazy import GOT slot is not contained in any section", targetRva);
}

void addBmiSections(std::vector<PeSection>& sections, const Codegen::BmiConvertResult& bmi, const WindowsLoadImage& image, WindowsVehInstall& veh) {
    const auto entryCodeRva = sections.back().Rva;
    auto& entryCode = sections.back().Data;
    const auto bmicodeRva = AlignRva(static_cast<std::uint64_t>(entryCodeRva) + entryCode.size());

    std::vector<std::uint8_t> bmicode;
    std::vector<std::uint32_t> stubOffsets(bmi.StubBlobs.size());
    for (std::size_t index = 0; index < bmi.StubBlobs.size(); ++index) {
        stubOffsets[index] = static_cast<std::uint32_t>(bmicode.size());
        bmicode.insert(bmicode.end(), bmi.StubBlobs[index].Bytes.begin(), bmi.StubBlobs[index].Bytes.end());
    }

    const auto handlerRva = CheckedRva(bmicodeRva + bmicode.size());
    BmiVehHandlerLayout handlerLayout;
    auto handler = BuildBmiVehHandler(handlerLayout);

    const auto tableRva = AlignRva(static_cast<std::uint64_t>(handlerRva) + handler.size());

    Io::WriteU32(handler, handlerLayout.LeaTableDisp, CheckedRva(tableRva - (handlerRva + handlerLayout.LeaTableDisp + 4)));
    Io::WriteU32(handler, handlerLayout.JeFoundDisp, CheckedRva(handlerLayout.JeFoundTarget - (handlerLayout.JeFoundDisp + 4)));
    Io::WriteU32(handler, handlerLayout.JnzSearchDisp, static_cast<std::uint32_t>(static_cast<std::int32_t>(handlerLayout.JnzSearchTarget) - (static_cast<std::int32_t>(handlerLayout.JnzSearchDisp) + 4)));

    Io::WriteU32(entryCode, veh.HandlerRvaOffset, CheckedRva(handlerRva - (entryCodeRva + veh.HandlerRvaOffset + 4)));

    for (const auto& site : bmi.DispatchSites) {
        const auto siteRva = image.GetRva(site.SegmentAddress + site.SegmentOffset, site.Length);
        const auto stubRva = CheckedRva(bmicodeRva + stubOffsets[site.StubIndex]);
        const auto& stub = bmi.StubBlobs[site.StubIndex];
        const auto tail = stub.TailRel32Offset;
        Io::WriteU32(bmicode, stubOffsets[site.StubIndex] + tail, static_cast<std::uint32_t>(static_cast<std::int32_t>(siteRva + site.Length - (stubRva + tail + 4))));

        if (stub.RipDisp32Offset != Codegen::BmiStubRecord::kNoTailRel32) {
            const std::int64_t target = static_cast<std::int64_t>(siteRva + site.Length)
                                      + static_cast<std::int32_t>(Io::ReadU32(bmicode, stubOffsets[site.StubIndex] + stub.RipDisp32Offset));
            const auto from = static_cast<std::int64_t>(stubRva) + stub.RipDisp32Offset + 4;
            Io::WriteU32(bmicode, stubOffsets[site.StubIndex] + stub.RipDisp32Offset,
                         static_cast<std::uint32_t>(static_cast<std::int32_t>(target - from)));
        }
        bool patched = false;
        for (auto& section : sections) {
            if (siteRva >= section.Rva && siteRva - section.Rva + 5 <= section.Data.size()) {
                Io::WriteU32(section.Data, siteRva - section.Rva + 1, CheckedRva(stubRva - (siteRva + 5)));
                patched = true;
                break;
            }
        }
        if (!patched)
            throw Domain::RelinkerException("BMI dispatch site is not contained in any section", siteRva);
    }

    std::vector<std::uint8_t> table;
    Io::AppendU32(table, 0x54494d42u);
    Io::AppendU32(table, static_cast<std::uint32_t>(bmi.ShortSites.size()));
    for (const auto& site : bmi.ShortSites) {
        const auto siteRva = image.GetRva(site.SegmentAddress + site.SegmentOffset, site.Length);
        const auto stubRva = CheckedRva(bmicodeRva + stubOffsets[site.StubIndex]);
        Io::AppendU32(table, siteRva);
        Io::AppendU32(table, stubRva);
        Io::AppendU32(table, CheckedRva(siteRva + site.Length));
        Io::AppendU8(table, site.SrcReg & 0x07);
        Io::AppendU8(table, site.DstReg & 0x07);
        Io::AppendU8(table, 0);
        Io::AppendU8(table, 0);
    }

    bmicode.insert(bmicode.end(), handler.begin(), handler.end());

    sections.push_back({".bmicode", bmicodeRva, SectionRead | SectionExecute | 0x20u, std::move(bmicode)});
    sections.push_back({".bmitab", tableRva, SectionRead | 0x20u, std::move(table)});
}

}

WindowsPePatcher::WindowsPePatcher(const bool windowsGui, std::filesystem::path iconPath) : _windowsGui(windowsGui), _iconPath(std::move(iconPath)) {
}

std::vector<std::uint8_t> WindowsPePatcher::patchImpl(const bool bmiEnabled, const Codegen::BmiConvertResult* bmi, const std::vector<std::uint8_t>& sourceElf, const std::vector<Domain::ProgramHeader>& originalHeaders, const Domain::SysVDynamicSection& dynamicSection, const std::uint64_t originalPltGotVaddr, const std::string& runPath, const bool lazyBinding, const bool dependencyDiagnostics, const std::vector<Codegen::TrampolineSite>& trampolines) const {
    WindowsLoadImage image(bmiEnabled ? bmi->Bytes : sourceElf, originalHeaders);
    if (originalPltGotVaddr != 0)
        image.GetRva(originalPltGotVaddr, 8);
    const WindowsRelocationBuilder relocationBuilder;
    auto relocations = relocationBuilder.Apply(image, dynamicSection);
    auto sections = image.BuildSections();
    std::array<PeDirectory, 16> directories{};
    auto nextRva = image.GetEndRva();
    bool hasProcessParameters = false;
    for (const auto& header : originalHeaders) {
        if (header.Type != 0x61000001) continue;
        if (hasProcessParameters || header.FileSize < 0x40) throw Domain::RelinkerException("Invalid process parameter segment");
        hasProcessParameters = true;
        std::vector<std::uint8_t> metadata(8);
        Io::WriteU32(metadata, 0, image.GetRva(header.MappedAddress, header.FileSize));
        Io::WriteU32(metadata, 4, CheckedRva(header.FileSize));
        sections.push_back({".procpar", nextRva, SectionRead | 0x40u, std::move(metadata)});
        nextRva = AlignRva(nextRva + sections.back().Data.size());
    }
    for (const auto& header : originalHeaders) {
        if (header.Type != 0x6474e550) continue;
        std::vector<std::uint8_t> metadata(4);
        Io::WriteU32(metadata, 0, image.GetRva(header.MappedAddress, header.FileSize));
        sections.push_back({".ehmeta", nextRva, SectionRead | 0x40u, std::move(metadata)});
        nextRva = AlignRva(nextRva + sections.back().Data.size());
    }
    directories[9] = WindowsTlsBuilder().Build(sourceElf, originalHeaders, image, sections, relocations.BaseRelocations, nextRva);
    WindowsTrampolineBuilder().Build(trampolines, image, sections, nextRva);
    const WindowsImportBuilder importBuilder;
    std::vector<std::string> extraFunctions;
    if (bmiEnabled)
        extraFunctions.push_back("AddVectoredExceptionHandler");
    auto nativeImports = importBuilder.Build(nextRva, extraFunctions);
    directories[1] = nativeImports.Directory;
    directories[12] = nativeImports.AddressTable;
    nextRva = AlignRva(nextRva + nativeImports.Section.Data.size());
    auto libraries = importBuilder.ReadLibraries(dynamicSection);
    std::vector<std::string> guestPaths;
    for (std::size_t index = 0; index < dynamicSection.GuestModules.size(); ++index) {
        const auto& module = dynamicSection.GuestModules[index];
        guestPaths.push_back(module.Path);
        for (const auto& import : module.Imports) relocations.Imports.push_back({import.Name, import.TargetRva, import.Addend, static_cast<std::int32_t>(index), import.RelocationType});
    }
    libraries.insert(libraries.begin(), guestPaths.begin(), guestPaths.end());
    if (dependencyDiagnostics)
        writeDiagnosticsImports(relocations.Imports);
    WindowsVehInstall veh;
    WindowsVehInstall* vehPtr = bmiEnabled ? &veh : nullptr;
    auto entry = WindowsEntryStubBuilder().Build(nextRva, image.GetEntryRva(), nativeImports, libraries, relocations.Imports, runPath, lazyBinding, dependencyDiagnostics, dynamicSection.GuestModules, vehPtr);
    directories[3] = entry.ExceptionDirectory;
    const auto entryRva = entry.Code.Rva;
    nextRva = AlignRva(entry.Code.Rva + entry.Code.Data.size());
    sections.push_back(std::move(nativeImports.Section));
    sections.push_back(std::move(entry.Data));
    sections.push_back(std::move(entry.Code));
    if (bmiEnabled) {
        addBmiSections(sections, *bmi, image, veh);
        nextRva = AlignRva(sections.back().Rva + sections.back().Data.size());
    }
    for (const auto& lazyStub : entry.LazyStubs) {
        writeGotStub(sections, lazyStub.TargetRva, lazyStub.StubRva);
        relocations.BaseRelocations.push_back(lazyStub.TargetRva);
    }
    auto relocationData = relocationBuilder.BuildBaseRelocations(relocations.BaseRelocations);
    if (!relocationData.empty()) {
        directories[5] = {nextRva, CheckedRva(relocationData.size())};
        sections.push_back({".reloc", nextRva, SectionRead | 0x02000040u, std::move(relocationData)});
        nextRva = AlignRva(nextRva + sections.back().Data.size());
    }
    directories[2] = WindowsIconResourceBuilder().Build(_iconPath, sections, nextRva);
    return WindowsPeWriter().Write(sections, entryRva, directories, _windowsGui);
}

std::vector<std::uint8_t> WindowsPePatcher::Patch(const std::vector<std::uint8_t>& sourceElf, const std::vector<Domain::ProgramHeader>& originalHeaders, const Domain::SysVDynamicSection& dynamicSection, const std::uint64_t originalPltGotVaddr, const std::string& runPath, const bool lazyBinding, const bool dependencyDiagnostics, const std::vector<Codegen::TrampolineSite>& trampolines) {
    return patchImpl(false, nullptr, sourceElf, originalHeaders, dynamicSection, originalPltGotVaddr, runPath, lazyBinding, dependencyDiagnostics, trampolines);
}

std::vector<std::uint8_t> WindowsPePatcher::PatchWithBmi(const std::vector<std::uint8_t>& sourceElf, const std::vector<Domain::ProgramHeader>& originalHeaders, const Domain::SysVDynamicSection& dynamicSection, const std::uint64_t originalPltGotVaddr, const std::string& runPath, const bool lazyBinding, const bool dependencyDiagnostics, const std::vector<Codegen::TrampolineSite>& trampolines, const Codegen::BmiConvertResult& bmi) const {
    return patchImpl(true, &bmi, sourceElf, originalHeaders, dynamicSection, originalPltGotVaddr, runPath, lazyBinding, dependencyDiagnostics, trampolines);
}

}
