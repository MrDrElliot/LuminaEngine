#pragma once

#include "EASTL/string.h"
#include "EASTL/hash_map.h"
#include "EASTL/hash_set.h"
#include "EASTL/string.h"

namespace Lumina
{
    class FNameHashMap;
    using FNameHashNode = eastl::hash_node<eastl::pair<const uint64_t, eastl::string>, false>;

    extern FNameHashMap* gNameCache;

    class FStringHash
    {
    public:
        
        // Initialize global state.
        static void Initialize();

        // Shutdown global state.
        static void Shutdown();

    public:

        FStringHash() = default;
        FStringHash(const char* Char);
        explicit FStringHash(uint64_t InID) :ID(InID) {}
        explicit FStringHash(const eastl::string& Str);
        explicit FStringHash(const eastl::string_view& Str);

        bool IsValid() const { return ID != 0; }
        bool IsNone() const;
        uint64_t GetID() const { return ID; }
        operator uint64_t() const { return ID; }
        eastl::string ToString() const;
        
        void Clear() { ID = 0; }
        const char* c_str() const;

        bool operator==(const FStringHash& Other) const { return ID == Other.ID; }
        bool operator!=(const FStringHash& Other) const { return ID != Other.ID; }
    
        
    private:

        uint64_t      ID = 0;
    };
    
}

namespace eastl
{
    template <typename T> struct hash;

    template <>
    struct hash<Lumina::FStringHash>
    {
        size_t operator()(const Lumina::FStringHash& ID) const { return (uint64_t) ID; }
    };
}
