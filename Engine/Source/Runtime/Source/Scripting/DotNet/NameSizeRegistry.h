#pragma once

#include "Platform/GenericPlatform.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace Lumina::DotNet
{
    enum class ENameSizeRegistry : uint8
    {
        Layout,
        ExportSignature,
        Num,
    };

    namespace NameSizeRegistry
    {
        struct FTransparentHash
        {
            using is_transparent = void;
            size_t operator()(std::string_view Name) const noexcept { return std::hash<std::string_view>{}(Name); }
        };

        using FMap = std::unordered_map<std::string, int32, FTransparentHash, std::equal_to<>>;

        // A std map in a function-local static, so it exists before any self-registration runs and needs no engine allocator.
        inline FMap& Get(ENameSizeRegistry Registry)
        {
            static FMap Maps[(size_t)ENameSizeRegistry::Num];
            return Maps[(size_t)Registry];
        }

        inline void Register(ENameSizeRegistry Registry, const char* Name, int32 Size)
        {
            Get(Registry).insert_or_assign(std::string(Name), Size);
        }

        inline int32 Find(ENameSizeRegistry Registry, const char* Name, int32 Len)
        {
            if (Name == nullptr || Len <= 0)
            {
                return -1;
            }

            const FMap& Map = Get(Registry);
            auto It = Map.find(std::string_view(Name, (size_t)Len));
            return It != Map.end() ? It->second : -1;
        }
    }
}
