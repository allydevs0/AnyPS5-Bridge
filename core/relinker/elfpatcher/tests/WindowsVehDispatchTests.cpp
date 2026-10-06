#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <elfpatcher/windows/BmiVehHandler.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Regression + machine-code test for the BMI VEH dispatch handler.
//
// The handler is the code that ran with 11 byte-level bugs during the i5-3570K
// bring-up (7 register-width coercions, 3 site/stub/next RVA loads that were
// emitted 64-bit instead of 32-bit, and a REX.B=0 on the PEB image-base load).
// It has no test coverage in the suite, so this pins:
//   1. the exact byte stream BuildBmiVehHandler emits (every fixed byte),
//   2. the resolved internal rel32 targets (je/search labels),
//   3. real dispatch: a genuine #UD raised at a synthetic site, handled by the
//      real handler installed as a VEH, resuming into the short stub's `C3`
//      chain and returning 0xCAFE through the marker,
//      exercised against a small PEB image base (0x01000000) and against a
//      >4GB base (0x140000000) - image bases that high zero-extend a plain
//      32-bit `add r9d,[rax]`, which is why the handler instead zero-extends
//      each RVA and adds the 64-bit base separately;
//   4. the miss path: a faulting site absent from the table returns
//      CONTINUE_SEARCH and never hijacks the context (asserted in a child).
//
// Exit tip: the live-stack hijack abandons KiUserExceptionDispatcher's frames,
// so the CRT `return`/`exit()` teardown can fault afterwards. main() therefore
// ends with ExitProcess() after flushing, keeping the PASS/FAIL exit code
// truthful.

namespace {

using Elfpatcher::Windows::BuildBmiVehHandler;
using Elfpatcher::Windows::BmiVehHandlerLayout;

constexpr unsigned int kImageBase = 0x1000000;
constexpr std::uint64_t kHugeBase = 0x140000000ull; // low 32 bits = 0x40000000
constexpr std::size_t kImageSize = 0x3000;
constexpr std::size_t kTableOff = 0x200;
constexpr std::size_t kStubOff = 0x300;
constexpr std::size_t kSiteOff = 0x1000;
constexpr unsigned int kMarker = 0xCAFE;
constexpr unsigned int kGoldenSize = 187;

std::uint64_t pebImageBaseAddress() {
    return __readgsqword(0x60) + 0x10;
}

void wrU32(std::uint8_t* p, std::size_t off, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i)
        p[off + i] = static_cast<std::uint8_t>(v >> (i * 8));
}

std::uint32_t rdU32(const std::uint8_t* p, std::size_t off) {
    return static_cast<std::uint32_t>(p[off]) | (static_cast<std::uint32_t>(p[off + 1]) << 8)
         | (static_cast<std::uint32_t>(p[off + 2]) << 16) | (static_cast<std::uint32_t>(p[off + 3]) << 24);
}

struct Record {
    std::uint32_t SiteRva;
    std::uint32_t StubRva;
    std::uint32_t NextRva;
};

// Layout of the image the test executes: handler at +0x0, table at +0x200,
// short stub (single C3) at +0x300, synthetic site (ud2 + marker) at +0x1000.
// Resolves all three handler rel32s exactly like WindowsPePatcher: the table
// lea (self-relative) and the internal je / jnz search branches.
void materializeImage(std::uint8_t* base, const std::vector<std::uint8_t>& handler, const BmiVehHandlerLayout& layout, const std::vector<Record>& records) {
    std::memset(base, 0, kImageSize);
    std::memcpy(base, handler.data(), handler.size());
    wrU32(base, layout.LeaTableDisp, static_cast<std::uint32_t>(kTableOff - (layout.LeaTableDisp + 4)));
    wrU32(base, layout.JeFoundDisp, static_cast<std::uint32_t>(static_cast<std::int32_t>(layout.JeFoundTarget) - (static_cast<std::int32_t>(layout.JeFoundDisp) + 4)));
    wrU32(base, layout.JnzSearchDisp, static_cast<std::uint32_t>(static_cast<std::int32_t>(layout.JnzSearchTarget) - (static_cast<std::int32_t>(layout.JnzSearchDisp) + 4)));
    wrU32(base, kTableOff, 0x54494d42u);
    wrU32(base, kTableOff + 4, static_cast<std::uint32_t>(records.size()));
    for (std::size_t i = 0; i < records.size(); ++i) {
        const std::size_t rec = kTableOff + 8 + i * 16;
        wrU32(base, rec + 0, records[i].SiteRva);
        wrU32(base, rec + 4, records[i].StubRva);
        wrU32(base, rec + 8, records[i].NextRva);
    }
    base[kStubOff] = 0xC3;
    base[kSiteOff] = 0x0F;
    base[kSiteOff + 1] = 0x0B;                                   // ud2
    base[kSiteOff + 2] = 0xB8;                                   // mov eax,0xCAFE
    wrU32(base, kSiteOff + 3, kMarker);
    base[kSiteOff + 7] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), base, kImageSize);
}

int runDispatch(std::uint64_t imageBase, const std::vector<std::uint8_t>& handler, const BmiVehHandlerLayout& layout) {
    const std::uint64_t physical = imageBase;
    LPVOID buffer = VirtualAlloc(reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(physical)), kImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!buffer) {
        std::fprintf(stderr, "runDispatch: VirtualAlloc(0x%llX) failed err=%lu\n", static_cast<unsigned long long>(physical), GetLastError());
        return -1;
    }
    if (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(buffer)) != physical) {
        std::fprintf(stderr, "runDispatch: VirtualAlloc did not honour base 0x%llX\n", static_cast<unsigned long long>(physical));
        return -1;
    }
    std::uint8_t* const base = static_cast<std::uint8_t*>(buffer);
    materializeImage(base, handler, layout, {
        {0x2000, 0x300, 0x300},       // miss: no site at base+0x2000
        {0x0000, 0x300, 0x300},       // miss: no site at base+0x0000
        {kSiteOff, 0x300, kSiteOff + 2}, // hit: ud2 at base+0x1000
    });

    const std::uint64_t location = pebImageBaseAddress();
    const std::uint64_t original = *reinterpret_cast<const std::uint64_t*>(location);
    *reinterpret_cast<std::uint64_t*>(location) = physical;

    const PVOID handle = AddVectoredExceptionHandler(1, reinterpret_cast<PVECTORED_EXCEPTION_HANDLER>(base));
    int result = 0;
    if (handle) {
        result = reinterpret_cast<int (*)()>(base + kSiteOff)();
        RemoveVectoredExceptionHandler(handle);
    }
    *reinterpret_cast<std::uint64_t*>(location) = original;

    VirtualFree(buffer, 0, MEM_RELEASE);
    return result;
}

// Child scenario for the miss path: the faulting site is absent from the
// table, so the handler must return CONTINUE_SEARCH and let the VEH chain
// reach the sentinel (which exits with 0x5B). If the handler ever wrongfully
// hijacks, the marker runs, the call returns, and the process exits 0 instead.
LONG WINAPI missSentinel(PEXCEPTION_POINTERS) {
    std::fflush(stderr);
    ExitProcess(0x5B);
    return 0;
}

int childMissSearch() {
    LPVOID buffer = VirtualAlloc(reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(kImageBase)), kImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!buffer)
        return 1;
    std::uint8_t* const base = static_cast<std::uint8_t*>(buffer);
    BmiVehHandlerLayout layout;
    const std::vector<std::uint8_t> handler = BuildBmiVehHandler(layout);
    materializeImage(base, handler, layout, {
        {0x2000, 0x300, 0x300},       // the only record; no site at base+0x2000
    });

    const std::uint64_t location = pebImageBaseAddress();
    const std::uint64_t original = *reinterpret_cast<const std::uint64_t*>(location);
    *reinterpret_cast<std::uint64_t*>(location) = kImageBase;

    AddVectoredExceptionHandler(1, missSentinel); // added first -> runs after
    AddVectoredExceptionHandler(1, reinterpret_cast<PVECTORED_EXCEPTION_HANDLER>(base));

    const int result = reinterpret_cast<int (*)()>(base + kSiteOff)();
    *reinterpret_cast<std::uint64_t*>(location) = original;
    std::fprintf(stderr, "childMissSearch: sentinel not reached, site returned %08X\n", static_cast<unsigned int>(result));
    return 1;
}

bool expectBytes(const std::uint8_t* p, std::size_t off, const std::vector<std::uint8_t>& want, const char* what) {
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (p[off + i] != want[i]) {
            std::fprintf(stderr, "byte regression: %s at +%zX: expected", what, off);
            for (std::uint8_t b : want) std::fprintf(stderr, " %02X", b);
            std::fprintf(stderr, " got");
            for (std::size_t j = 0; j < want.size(); ++j) std::fprintf(stderr, " %02X", p[off + j]);
            std::fprintf(stderr, "\n");
            return false;
        }
    }
    return true;
}

bool verifyHandlerRegression() {
    BmiVehHandlerLayout layout;
    const std::vector<std::uint8_t> handler = BuildBmiVehHandler(layout);

    bool ok = layout.Size == kGoldenSize;
    if (!ok)
        std::fprintf(stderr, "handler size: got %zu, expected %u\n", layout.Size, kGoldenSize);
    // The rel32 field offsets and absolute search/found labels are part of the
    // encoding contract; if the handler is ever re-laid-out these are the
    // constants to update alongside the byte expectations below.
    ok = ok && layout.LeaTableDisp == 0x24
      && layout.JeFoundDisp == 0x4f && layout.JeFoundTarget == 0x84
      && layout.JnzSearchDisp == 0x5c && layout.JnzSearchTarget == 0x44;
    // (labels above are unchanged: the found path grew, but the search loop before
    //  it did not, so the entry points kept their offsets.)
    if (!ok)
        std::fprintf(stderr, "layout: leaDisp=%zu jeDisp=%zu jeTarget=%zu jnzDisp=%zu jnzTarget=%zu\n",
                     layout.LeaTableDisp, layout.JeFoundDisp, layout.JeFoundTarget, layout.JnzSearchDisp, layout.JnzSearchTarget);

    ok = expectBytes(handler.data(), 0x21, {0x48, 0x8D, 0x05}, "lea") && ok;
    ok = expectBytes(handler.data(), 0x24, {0, 0, 0, 0}, "lea rel32 zeroed") && ok;
    ok = expectBytes(handler.data(), 0x2f, {0x44, 0x8B, 0x40, 0x04}, "record count") && ok;
    ok = expectBytes(handler.data(), 0x37, {0x65, 0x4C, 0x8B, 0x1C, 0x25, 0x60, 0x00, 0x00, 0x00}, "TEB->PEB") && ok;
    ok = expectBytes(handler.data(), 0x40, {0x4D, 0x8B, 0x5B, 0x10}, "PEB->ImageBaseAddress") && ok;
    ok = expectBytes(handler.data(), 0x44, {0x44, 0x8B, 0x08}, "mov r9d,[rax]") && ok;
    ok = expectBytes(handler.data(), 0x47, {0x4D, 0x01, 0xD9}, "add r9,r11") && ok;
    ok = expectBytes(handler.data(), 0x4a, {0x49, 0x39, 0xD1}, "cmp r9,rdx") && ok;
    ok = expectBytes(handler.data(), 0x84, {0x48, 0x8B, 0x51, 0x08}, "mov rdx,[rcx+8]") && ok;
    ok = expectBytes(handler.data(), 0x88, {0x44, 0x8B, 0x50, 0x04}, "mov r10d,[rax+4]") && ok;
    ok = expectBytes(handler.data(), 0x8c, {0x4D, 0x01, 0xDA}, "add r10,r11") && ok;
    ok = expectBytes(handler.data(), 0x8f, {0x44, 0x8B, 0x48, 0x08}, "mov r9d,[rax+8]") && ok;
    ok = expectBytes(handler.data(), 0x93, {0x4D, 0x01, 0xD9}, "add r9,r11") && ok;
    ok = expectBytes(handler.data(), 0x96, {0x4C, 0x8B, 0x82, 0x98, 0x00, 0x00, 0x00}, "mov r8,[rdx+0x98]") && ok;
    ok = expectBytes(handler.data(), 0x9d, {0x49, 0x83, 0xE8, 0x08}, "sub r8,8") && ok;
    ok = expectBytes(handler.data(), 0xa1, {0x4D, 0x89, 0x08}, "mov [r8],r9") && ok;
    ok = expectBytes(handler.data(), 0xa4, {0x4C, 0x89, 0x82, 0x98, 0x00, 0x00, 0x00}, "mov [rdx+0x98],r8") && ok;
    ok = expectBytes(handler.data(), 0xab, {0x4C, 0x89, 0x92, 0xF8, 0x00, 0x00, 0x00}, "mov [rdx+0xf8],r10") && ok;
    ok = expectBytes(handler.data(), 0xb2, {0x48, 0x83, 0xC4, 0x60}, "add rsp,96") && ok;
    ok = expectBytes(handler.data(), 0xb6, {0x48, 0x83, 0xC8, 0xFF}, "or rax,-1") && ok;

    ok = ok && handler.back() == 0xC3;

    // Resolve the internal rel32s exactly like WindowsPePatcher does and check
    // the absolute landing offsets are the labels the layout promised.
    if (ok) {
        std::vector<std::uint8_t> resolved = handler;
        wrU32(resolved.data(), 0x4f, static_cast<std::uint32_t>(static_cast<std::int32_t>(layout.JeFoundTarget) - (static_cast<std::int32_t>(layout.JeFoundDisp) + 4)));
        wrU32(resolved.data(), 0x5c, static_cast<std::uint32_t>(static_cast<std::int32_t>(layout.JnzSearchTarget) - (static_cast<std::int32_t>(layout.JnzSearchDisp) + 4)));
        ok = rdU32(resolved.data(), 0x4f) == 0x31 && rdU32(resolved.data(), 0x5c) == 0xFFFFFFE4u;
        if (!ok)
            std::fprintf(stderr, "resolved branch targets do not land on the labels\n");
    }
    return ok;
}

bool runChild(const std::string& childArguments) {
    char executable[32768];
    const DWORD size = GetModuleFileNameA(nullptr, executable, static_cast<DWORD>(sizeof(executable)));
    if (size == 0 || size >= sizeof(executable))
        return false;
    std::string command = executable;
    command += childArguments;
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 20000);
    DWORD exitCode = 0;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0)
        std::fprintf(stderr, "child miss test timed out\n");
    std::printf("  miss path child exit: 0x%08X\n", static_cast<unsigned int>(exitCode));
    if (!(wait == WAIT_OBJECT_0 && exitCode == 0x5B))
        std::fprintf(stderr, "  expected 0x5B (handler returned CONTINUE_SEARCH, sentinel reached)\n");
    return wait == WAIT_OBJECT_0 && exitCode == 0x5B;
}

}

static LONG __stdcall LogUnhandled(struct _EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    std::fprintf(stderr, "UNHANDLED exception code=0x%08lX at 0x%llX PEB-base=0x%llX\n",
                 static_cast<unsigned long>(rec->ExceptionCode),
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress)),
                 static_cast<unsigned long long>(*reinterpret_cast<const std::uint64_t*>(pebImageBaseAddress())));
    return EXCEPTION_CONTINUE_SEARCH;
}

void finish(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    ExitProcess(static_cast<UINT>(code));
}

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(&LogUnhandled);
    if (argc >= 2 && std::strcmp(argv[1], "--child-miss-search") == 0)
        finish(childMissSearch() ? 1 : 0);

    bool ok = verifyHandlerRegression();
    std::printf("handler byte/layout regression: %s\n", ok ? "PASS" : "FAIL");

    // Child first: it spawns a second process, and the loader's first-time
    // delay-load unwind is not robust against the abandoned dispatcher frames
    // left behind by an earlier hijack (AV inside LdrResolveDelayLoadedAPI
    // seen under Release/-O3). Nothing about the miss assertion depends on
    // ordering, so exercise it before any real dispatch runs.
    const bool miss = runChild(" --child-miss-search");
    std::printf("miss path continues search: %s\n", miss ? "PASS" : "FAIL");

    BmiVehHandlerLayout layout;
    const std::vector<std::uint8_t> handler = BuildBmiVehHandler(layout);

    int result = runDispatch(kImageBase, handler, layout);
    const bool smallBase = result == static_cast<int>(kMarker);
    std::printf("dispatch at small PEB base 0x%X: %s\n", kImageBase, smallBase ? "PASS (0xCAFE)" : "FAIL");

    const bool hugeBase = runDispatch(kHugeBase, handler, layout) == static_cast<int>(kMarker);
    std::printf("dispatch at wrapping >4GB PEB base 0x%llX: %s\n", static_cast<unsigned long long>(kHugeBase), hugeBase ? "PASS (0xCAFE)" : "FAIL");

    if (ok && miss && smallBase && hugeBase) {
        std::printf("Windows VEH dispatch machine-code tests passed\n");
        finish(0);
    }
    std::fprintf(stderr, "Windows VEH dispatch tests FAILED\n");
    finish(1);
}