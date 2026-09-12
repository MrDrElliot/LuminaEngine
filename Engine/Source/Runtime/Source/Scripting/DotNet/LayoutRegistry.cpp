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
        // A function-local static, so the map exists before any self-registration runs.
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

// The schema wire sends these as kind bytes, so a drift would misinterpret every property.
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

// A [Property] flag is a bit both sides must agree on, and a wrong bit is a silently wrong behaviour
// (a field that quietly stops replicating) rather than a crash, so it is checked at bootstrap like the kinds.
LUMINA_DOTNET_EXPORT(int32, PropertyFlag_Value)(const char* Name, int32 Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return -1;
    }
    for (size_t Index = 0; Index < std::size(::Lumina::PropertyFlagNames); ++Index)
    {
        const char* Candidate = ::Lumina::PropertyFlagNames[Index];
        if (std::strlen(Candidate) == (size_t)Len && std::memcmp(Candidate, Name, (size_t)Len) == 0)
        {
            return (int32)::Lumina::PropertyFlagValues[Index];
        }
    }
    return -1;
}

LUMINA_DOTNET_EXPORT(int32, PropertyFlag_Count)()
{
    return (int32)std::size(::Lumina::PropertyFlagNames);
}

LUMINA_DOTNET_EXPORT(int32, PropertyType_Count)()
{
    return (int32)::Lumina::EPropertyTypeFlags::Count;
}
