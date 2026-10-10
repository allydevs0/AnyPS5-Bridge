#include <windows.h>
#include <iostream>

#ifndef CONTEXT_XSTATE
#define CONTEXT_XSTATE (0x00100040)
#endif

LONG WINAPI Handler(EXCEPTION_POINTERS* ep) {
    std::cout << "ContextFlags: 0x" << std::hex << ep->ContextRecord->ContextFlags << std::endl;
    bool hasXstate = (ep->ContextRecord->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE;
    std::cout << "has XSTATE flag: " << hasXstate << std::endl;
    auto pfn = (PVOID (WINAPI *)(PCONTEXT, DWORD, PDWORD))GetProcAddress(GetModuleHandleA("kernel32.dll"), "LocateXStateFeature");
    if (pfn) {
        PVOID ptr = pfn(ep->ContextRecord, 2, NULL);
        std::cout << "LocateXStateFeature(2): " << ptr << std::endl;
    }
    ExitProcess(0);
}

int main() {
    AddVectoredExceptionHandler(1, Handler);
    __asm__ volatile("ud2");
    return 0;
}
