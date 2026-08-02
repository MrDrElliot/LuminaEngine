#include "RuntimePCH.h"

#include "LayoutRegistry.h"
#include "Core/Object/ObjectCore.h"
#include "Scripting/DotNet/DotNetExport.h"

#include <cstring>
#include <unordered_map>
#include <string>

namespace Lumina::DotNet
{
    namespace
    {
        // Function-local static so the map is constructed before any self-registration runs (avoids the
        // static-init-order problem across translation units).
        std::unordered_map<std::string, int32>& Registry()
        {
            static std::unordered_map<std::string, int32> Map;
            return Map;
        }
    }

    void RegisterLayout(const char* Key, int32 Size)
    {
        Registry()[std::string(Key)] = Size;
    }

    int32 GetLayoutSize(const char* Name, int32 Len)
    {
        if (Name == nullptr || Len <= 0)
        {
            return -1;
        }
        const std::unordered_map<std::string, int32>& Map = Registry();
        auto It = Map.find(std::string(Name, (size_t)Len));
        return It != Map.end() ? It->second : -1;
    }
}

LUMINA_DOTNET_EXPORT(int32, Layout_GetSize)(const char* Name, int32 Len)
{
    return ::Lumina::DotNet::GetLayoutSize(Name, Len);
}

// Interop validation for the reflected property-type taxonomy. The managed LuminaSharp.EPropertyType is a
// hand-written mirror of Lumina::EPropertyTypeFlags; at bootstrap it asks native for the integer value of each
// enumerator by its plain name and for the total count, aborting C# on any mismatch (the script schema wire
// sends these values as kind bytes, so a drift would silently misinterpret every property).
LUMINA_DOTNET_EXPORT(int32, PropertyType_Value)(const char* Name, int32 Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return -1;
    }
    for (int32 Index = 0; Index < (int32)::Lumina::EPropertyTypeFlags::Count; ++Index)
    {
        const char* Candidate = ::Lumina::PropertyTypePlainNames[Index];
        if (std::strlen(Candidate) == (size_t)Len && std::memcmp(Candidate, Name, (size_t)Len) == 0)
        {
            return Index;
        }
    }
    return -1;
}

LUMINA_DOTNET_EXPORT(int32, PropertyType_Count)()
{
    return (int32)::Lumina::EPropertyTypeFlags::Count;
}
