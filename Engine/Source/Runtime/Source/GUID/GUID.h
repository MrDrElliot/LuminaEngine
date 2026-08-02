#pragma once
#include "Containers/Array.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Templates/Optional.h"

namespace Lumina
{
    class RUNTIME_API FGuid
    {
    public:
        
        static constexpr size_t GUID_SIZE = 16;
        
        using ByteArray = TArray<uint8, GUID_SIZE>;
    
        static FGuid New();
        static FGuid NewDeterministic(FStringView seed);
        static FGuid FromString(FStringView str);
        static TOptional<FGuid> TryParse(FStringView str);
        
        static const FGuid& Empty() noexcept;
        static const FGuid& Invalid() noexcept { return Empty(); }
        
        FGuid() noexcept = default;
        explicit FGuid(const ByteArray& bytes) noexcept;
        explicit FGuid(ByteArray&& bytes) noexcept;
        
        FGuid(const FGuid&) = default;
        FGuid& operator=(const FGuid&) = default;
        FGuid(FGuid&&) noexcept = default;
        FGuid& operator=(FGuid&&) noexcept = default;
        ~FGuid() = default;

        bool operator==(const FGuid& other) const noexcept;
        bool operator!=(const FGuid& other) const noexcept;
        bool operator<(const FGuid& other) const noexcept;
        bool operator<=(const FGuid& other) const noexcept;
        bool operator>(const FGuid& other) const noexcept;
        bool operator>=(const FGuid& other) const noexcept;
        std::strong_ordering operator<=>(const FGuid& other) const noexcept = default;
    
        FString ToString(bool uppercase = true, bool includeDashes = true) const;
        FString ToShortString() const;
        
        bool IsValid() const noexcept;
        explicit operator bool() const noexcept { return IsValid(); }
        
        const ByteArray& GetBytes() const noexcept { return Bytes; }
        ByteArray& GetBytes() noexcept { return Bytes; }
        const uint8_t* Data() const noexcept { return Bytes.data(); }
        
        void Invalidate() noexcept;
        void Swap(FGuid& other) noexcept;
        
        size_t Hash() const noexcept;

        friend FArchive& operator<<(FArchive& Ar, FGuid& Guid)
        {
            Ar.Serialize(Guid.Bytes.data(), Guid.Bytes.size());
            return Ar;
        }
        
        friend RUNTIME_API std::ostream& operator<<(std::ostream& os, const FGuid& guid);
        friend RUNTIME_API std::istream& operator>>(std::istream& is, FGuid& guid);
    
    private:
        ByteArray Bytes{};

        static bool TryParseInternal(FStringView str, ByteArray& outBytes);
    };
}


namespace eastl
{
    template <>
    struct RUNTIME_API hash<Lumina::FGuid>
    {
        std::size_t operator()(const Lumina::FGuid& Guid) const noexcept
        {
            return Guid.Hash();
        }
    };
}


template <>
struct RUNTIME_API std::formatter<Lumina::FGuid>
{
    constexpr auto parse(std::format_parse_context& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(const Lumina::FGuid& guid, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "{}", guid.ToString());
    }
};
