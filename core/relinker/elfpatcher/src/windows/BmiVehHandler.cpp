#include <elfpatcher/windows/BmiVehHandler.hpp>
#include <cstdint>
#include <initializer_list>

namespace Elfpatcher::Windows {

// Builds the fixed-size VEH dispatch handler. The handler:
//   - spills the scratch registers and forms a search loop over the .bmitab
//     records (site RVA relative to PEB->ImageBaseAddress), comparing the
//     fault address from the EXCEPTION_RECORD against ImageBase + siteRVA;
//     RVAs are loaded 32-bit zero-extended so the 64-bit ImageBase survives
//     for images loaded at or above 4 GiB;
//   - on a match, resolves stub VA / next VA against the same ImageBase, writes
//     them into CONTEXT, and returns EXCEPTION_CONTINUE_EXECUTION;
//   - on a miss, restores scratch registers and returns CONTINUE_SEARCH.
//
// Resuming is done by *editing the CONTEXT*, not by returning. An earlier version
// pushed stubVA/nextVA below the fault RSP and then executed `ret`, expecting the
// return to land in the stub. That cannot work: `ret` returns to the kernel's
// exception dispatcher, which then restores the CONTEXT we never modified, so the
// guest re-executed the faulting site and died. CONTEXT.Rip (0xF8), CONTEXT.Rsp
// (0x98) and the GPRs all have to be written, and Rax set to
// EXCEPTION_CONTINUE_EXECUTION (0).
//
// Every rel32 field position is captured as the bytes are emitted, so the
// patch offsets can never drift from the byte layout.
std::vector<std::uint8_t> BuildBmiVehHandler(BmiVehHandlerLayout& layout) {
    std::vector<std::uint8_t> handler;
    std::size_t foundLabel = 0;
    const auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
        handler.insert(handler.end(), bytes.begin(), bytes.end());
    };
    const auto emitDisp = [&](std::size_t& field) {
        field = handler.size();
        handler.resize(handler.size() + 4, 0);
    };
    emit({0x48, 0x83, 0xec, 0x60});                                  // sub rsp,96
    emit({0x48, 0x89, 0x04, 0x24});                                  // mov [rsp+0],rax
    emit({0x48, 0x89, 0x54, 0x24, 0x08});                            // mov [rsp+8],rdx
    emit({0x4c, 0x89, 0x44, 0x24, 0x10});                            // mov [rsp+16],r8
    emit({0x4c, 0x89, 0x4c, 0x24, 0x18});                            // mov [rsp+24],r9
    emit({0x4c, 0x89, 0x54, 0x24, 0x20});                            // mov [rsp+32],r10
    emit({0x4c, 0x89, 0x5c, 0x24, 0x28});                            // mov [rsp+40],r11
    emit({0x48, 0x8d, 0x05});                                        // lea rax,[rip+table]
    emitDisp(layout.LeaTableDisp);
    emit({0x48, 0x8b, 0x11});                                        // mov rdx,[rcx]         (EXCEPTION_RECORD)
    emit({0x48, 0x8b, 0x52, 0x10});                                  // mov rdx,[rdx+16]      (fault address)
    emit({0x44, 0x8b, 0x40, 0x04});                                  // mov r8d,[rax+4]       (record count)
    emit({0x48, 0x8d, 0x40, 0x08});                                  // lea rax,[rax+8]       (records)
    emit({0x65, 0x4c, 0x8b, 0x1c, 0x25, 0x60, 0x00, 0x00, 0x00});    // mov r11,gs:[0x60]   (TEB->PEB)
    emit({0x4d, 0x8b, 0x5b, 0x10});                                  // mov r11,[r11+0x10]   (PEB->ImageBaseAddress)
    layout.JnzSearchTarget = handler.size();
    emit({0x44, 0x8b, 0x08});                                        // search: mov r9d,[rax]  (site RVA, zero-extended)
    emit({0x4d, 0x01, 0xd9});                                        // add r9,r11            (ImageBase + site RVA)
    emit({0x49, 0x39, 0xd1});                                        // cmp r9,rdx
    emit({0x0f, 0x84});                                              // je found
    emitDisp(layout.JeFoundDisp);
    emit({0x48, 0x83, 0xc0, 0x10});                                  // add rax,16
    emit({0x41, 0xff, 0xc8});                                        // dec r8d
    emit({0x0f, 0x85});                                              // jnz search
    emitDisp(layout.JnzSearchDisp);
    emit({0x48, 0x8b, 0x04, 0x24});                                  // mov rax,[rsp]
    emit({0x48, 0x8b, 0x54, 0x24, 0x08});                            // mov rdx,[rsp+8]
    emit({0x4c, 0x8b, 0x44, 0x24, 0x10});                            // mov r8,[rsp+16]
    emit({0x4c, 0x8b, 0x4c, 0x24, 0x18});                            // mov r9,[rsp+24]
    emit({0x4c, 0x8b, 0x54, 0x24, 0x20});                            // mov r10,[rsp+32]
    emit({0x4c, 0x8b, 0x5c, 0x24, 0x28});                            // mov r11,[rsp+40]
    emit({0x48, 0x83, 0xc4, 0x60});                                  // add rsp,96
    emit({0x31, 0xc0});                                              // xor eax,eax
    emit({0xc3});                                                    // ret                  (CONTINUE_SEARCH)
    foundLabel = handler.size();
    layout.JeFoundTarget = foundLabel;
    emit({0x48, 0x8b, 0x51, 0x08});                                  // found: mov rdx,[rcx+8] (CONTEXT)
    emit({0x44, 0x8b, 0x50, 0x04});                                  // mov r10d,[rax+4]      (stub RVA, zero-extended)
    emit({0x4d, 0x01, 0xda});                                        // add r10,r11           (stub VA = ImageBase + stub RVA)
    emit({0x44, 0x8b, 0x48, 0x08});                                  // mov r9d,[rax+8]       (next RVA, zero-extended)
    emit({0x4d, 0x01, 0xd9});                                        // add r9,r11            (next VA = ImageBase + next RVA)
    emit({0x4c, 0x8b, 0x82, 0x98, 0x00, 0x00, 0x00});                // mov r8,[rdx+0x98]     (fault RSP)
    emit({0x49, 0x83, 0xe8, 0x08});                                  // sub r8,8              (fault RSP - 8)
    emit({0x4d, 0x89, 0x08});                                        // mov [r8],r9           ([fault RSP - 8] = next VA)
    emit({0x4c, 0x89, 0x82, 0x98, 0x00, 0x00, 0x00});                // mov [rdx+0x98],r8     (CONTEXT.Rsp = fault RSP - 8)
    emit({0x4c, 0x89, 0x92, 0xf8, 0x00, 0x00, 0x00});                // mov [rdx+0xf8],r10    (CONTEXT.Rip = stub VA)
    emit({0x48, 0x83, 0xc4, 0x60});                                  // add rsp,96
    emit({0x48, 0x83, 0xc8, 0xff});                                  // or rax,-1             (EXCEPTION_CONTINUE_EXECUTION)
    emit({0xc3});                                                    // ret

    layout.Size = handler.size();
    return handler;
}

}
