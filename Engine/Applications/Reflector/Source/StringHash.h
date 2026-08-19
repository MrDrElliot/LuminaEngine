#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace Lumina
{
    class FNameHashMap;

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
        explicit FStringHash(const std::string& Str);
        explicit FStringHash(const std::string_view& Str);

        bool IsValid() const { return ID != 0; }
        bool IsNone() const;
        uint64_t GetID() const { return ID; }
        operator uint64_t() const { return ID; }
        std::string ToString() const;
        
        void Clear() { ID = 0; }
        const char* c_str() const;

        bool operator==(const FStringHash& Other) const { return ID == Other.ID; }
        bool operator!=(const FStringHash& Other) const { return ID != Other.ID; }
    
        
    private:

        uint64_t      ID = 0;
    };
    
}

namespace std
{
    template <typename T> struct hash;

    template <>
    struct hash<Lumina::FStringHash>
    {
        size_t operator()(const Lumina::FStringHash& ID) const { return (uint64_t) ID; }
    };
}
