#include "ToolsMenuRegistry.h"

namespace Lumina
{
    FToolsMenuRegistry& FToolsMenuRegistry::Get()
    {
        static FToolsMenuRegistry Instance;
        return Instance;
    }

    uint32 FToolsMenuRegistry::Register(FToolsMenuEntry Entry)
    {
        const uint32 Handle = NextHandle++;
        Entry.Handle = Handle;
        Entries.push_back(std::move(Entry));
        return Handle;
    }

    void FToolsMenuRegistry::Unregister(uint32 Handle)
    {
        if (Handle == 0)
        {
            return;
        }
        for (auto It = Entries.begin(); It != Entries.end(); ++It)
        {
            if (It->Handle == Handle)
            {
                Entries.erase(It);
                return;
            }
        }
    }
}
