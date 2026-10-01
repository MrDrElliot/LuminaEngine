#include "RuntimePCH.h"
#include "Profile.h"

#if defined(TRACY_ENABLE)

#include "Containers/HashTable.h"
#include "Memory/Memory.h"

namespace Lumina::Profiler
{
    const tracy::SourceLocationData* NamedLocation(const char* Name)
    {
        // Per thread so the lookup takes no lock. Tracy keeps every location it is handed, so none is ever freed.
        thread_local THashMap<const char*, const tracy::SourceLocationData*> Locations;

        const auto Found = Locations.find(Name);
        if (Found != Locations.end())
        {
            return Found->second;
        }

        const tracy::SourceLocationData* Location = Memory::New<tracy::SourceLocationData>(tracy::SourceLocationData{ Name, Name, "", 0, 0 });
        Locations.emplace(Name, Location);
        return Location;
    }
}

#endif
