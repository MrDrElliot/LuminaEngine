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
        Entries.Add(std::move(Entry));
        return Handle;
    }

    void FToolsMenuRegistry::Unregister(uint32 Handle)
    {
        if (Handle == 0)
        {
            return;
        }
        for (int32 i = 0; i < Entries.Num(); ++i)
        {
            if (Entries[i].Handle == Handle)
            {
                Entries.RemoveAt(i);
                return;
            }
        }
    }
}
