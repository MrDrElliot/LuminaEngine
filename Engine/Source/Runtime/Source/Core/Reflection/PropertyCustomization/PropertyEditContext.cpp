#include "RuntimePCH.h"
#include "PropertyEditContext.h"

namespace Lumina
{
    void FPropertyEditContext::Set(const FName& Key, void* Value)
    {
        for (FEntry& Entry : Entries)
        {
            if (Entry.Key == Key)
            {
                Entry.Value = Value;
                return;
            }
        }

        Entries.push_back(FEntry{ Key, Value });
    }

    void* FPropertyEditContext::Find(const FName& Key) const
    {
        for (const FEntry& Entry : Entries)
        {
            if (Entry.Key == Key)
            {
                return Entry.Value;
            }
        }

        return (Parent != nullptr) ? Parent->Find(Key) : nullptr;
    }

    const FPropertyEditContext& FPropertyEditContext::None()
    {
        static const FPropertyEditContext Empty;
        return Empty;
    }
}
