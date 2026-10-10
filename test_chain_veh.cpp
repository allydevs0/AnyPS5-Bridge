#include <windows.h>
#include <cstdio>
#include <vector>
#include <cstdint>
#include <cstring>

static int s_count = 0;

int main() {
    // Let's test a sequence of 5 synthetic #UDs in a row
    // and verify they all execute cleanly through VEH with NO stack corruption!
    uint8_t code[] = {
        0x0F, 0x0B, // ud2 (site 0)
        0x0F, 0x0B, // ud2 (site 1)
        0x0F, 0x0B, // ud2 (site 2)
        0x0F, 0x0B, // ud2 (site 3)
        0x0F, 0x0B, // ud2 (site 4)
        0xB8, 0x37, 0x13, 0x00, 0x00, // mov eax, 0x1337
        0xC3        // ret
    };

    DWORD old;
    VirtualProtect(code, sizeof(code), PAGE_EXECUTE_READWRITE, &old);

    // Let's create stubs for each of the 5 sites: each stub just does nop; ret
    uint8_t stubs[5][2];
    for (int i = 0; i < 5; ++i) {
        stubs[i][0] = 0x90; // nop
        stubs[i][1] = 0xC3; // ret
        VirtualProtect(stubs[i], 2, PAGE_EXECUTE_READWRITE, &old);
    }

    struct Record {
        uint64_t site;
        uint64_t stub;
        uint64_t next;
    };
    Record table[5];
    for (int i = 0; i < 5; ++i) {
        table[i].site = reinterpret_cast<uint64_t>(&code[i * 2]);
        table[i].stub = reinterpret_cast<uint64_t>(stubs[i]);
        table[i].next = reinterpret_cast<uint64_t>(&code[(i + 1) * 2]);
    }

    auto handler = [](PEXCEPTION_POINTERS info) -> LONG {
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
            uint64_t fault = reinterpret_cast<uint64_t>(info->ExceptionRecord->ExceptionAddress);
            auto* pTable = reinterpret_cast<Record*>(info->ExceptionRecord->ExceptionInformation[0]); // pass table
            // search
            for (int i = 0; i < 5; ++i) {
                if (fault == reinterpret_cast<uint64_t>(&pTable[i].site)) {
                    s_count++;
                    auto* ctx = info->ContextRecord;
                    uint64_t fault_rsp = ctx->Rsp;
                    uint64_t resume_rsp = fault_rsp - 8;
                    *reinterpret_cast<uint64_t*>(resume_rsp) = pTable[i].next;
                    ctx->Rsp = resume_rsp;
                    ctx->Rip = pTable[i].stub;
                    return EXCEPTION_CONTINUE_EXECUTION; // -1
                }
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    };

    // Use a wrapper that passes table
    static Record* s_pTable = table;
    auto wrapper = [](PEXCEPTION_POINTERS info) -> LONG {
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
            uint64_t fault = reinterpret_cast<uint64_t>(info->ExceptionRecord->ExceptionAddress);
            for (int i = 0; i < 5; ++i) {
                if (fault == s_pTable[i].site) {
                    s_count++;
                    auto* ctx = info->ContextRecord;
                    uint64_t fault_rsp = ctx->Rsp;
                    uint64_t resume_rsp = fault_rsp - 8;
                    *reinterpret_cast<uint64_t*>(resume_rsp) = s_pTable[i].next;
                    ctx->Rsp = resume_rsp;
                    ctx->Rip = s_pTable[i].stub;
                    return EXCEPTION_CONTINUE_EXECUTION; // -1
                }
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    };

    PVOID h = AddVectoredExceptionHandler(1, wrapper);
    auto fn = reinterpret_cast<int(*)()>(code);
    int res = fn();
    RemoveVectoredExceptionHandler(h);

    printf("Result: 0x%x (expected 0x1337), total exceptions handled: %d (expected 5)\n", res, s_count);
    return (res == 0x1337 && s_count == 5) ? 0 : 1;
}
