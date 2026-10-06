#ifndef ELFPATCHER_WINDOWS_BMIVEHHANDLER_HPP
#define ELFPATCHER_WINDOWS_BMIVEHHANDLER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Elfpatcher::Windows {

// Layout bookkeeping for the BMI dispatch handler returned by
// BuildBmiVehHandler. The handler searches the .bmitab table for the
// faulting site (site RVA relative to PEB->ImageBaseAddress), then - on a
// match - redirects the fault context to the short stub. The returned bytes
// carry three zero-filled rel32 fields that the caller must resolve once the
// .bmitab RVA is known (all offsets are self-relative / base-independent):
//   Io::WriteU32(handler, layout.LeaTableDisp, tableRva - (handlerRva + layout.LeaTableDisp + 4));
//   Io::WriteU32(handler, layout.JeFoundDisp, layout.JeFoundTarget - (layout.JeFoundDisp + 4));
//   Io::WriteU32(handler, layout.JnzSearchDisp, layout.JnzSearchTarget - (layout.JnzSearchDisp + 4));
// JeFoundTarget / JnzSearchTarget are absolute offsets inside the handler.
struct BmiVehHandlerLayout {
    std::size_t Size = 0;

    std::size_t LeaTableDisp = 0;

    std::size_t JeFoundDisp = 0;
    std::size_t JeFoundTarget = 0;

    std::size_t JnzSearchDisp = 0;
    std::size_t JnzSearchTarget = 0;
};

std::vector<std::uint8_t> BuildBmiVehHandler(BmiVehHandlerLayout& layout);

}

#endif