#include "RuntimePCH.h"
#include "GUID.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <random>

#include "Core/Math/Hash/Hash.h"

#if defined(_WIN32)
#include <objbase.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CFUUID.h>
#elif defined(__linux__) || defined(__unix__)
#include <uuid/uuid.h>
#endif
    

namespace Lumina
{
    // Static empty GUID
    const FGuid& FGuid::Empty() noexcept
    {
        static const FGuid Empty;
        return Empty;
    }
    
    FGuid::FGuid(const ByteArray& bytes) noexcept
        : Bytes(bytes)
    {
    }
    
    FGuid::FGuid(ByteArray&& bytes) noexcept
        : Bytes(std::move(bytes))
    {
    }
    
    FGuid FGuid::New()
    {
    #if defined(_WIN32)
        GUID WinGuid;
        if (FAILED(CoCreateGuid(&WinGuid)))
        {
            return FGuid();
        }
        
        ByteArray bytes =
        {
            static_cast<uint8>((WinGuid.Data1 >> 24) & 0xFF),
            static_cast<uint8>((WinGuid.Data1 >> 16) & 0xFF),
            static_cast<uint8>((WinGuid.Data1 >> 8) & 0xFF),
            static_cast<uint8>(WinGuid.Data1 & 0xFF),
            
            static_cast<uint8>((WinGuid.Data2 >> 8) & 0xFF),
            static_cast<uint8>(WinGuid.Data2 & 0xFF),
            
            static_cast<uint8>((WinGuid.Data3 >> 8) & 0xFF),
            static_cast<uint8>(WinGuid.Data3 & 0xFF),
            
            WinGuid.Data4[0], WinGuid.Data4[1], WinGuid.Data4[2], WinGuid.Data4[3],
            WinGuid.Data4[4], WinGuid.Data4[5], WinGuid.Data4[6], WinGuid.Data4[7]
        };
        
        return FGuid(bytes);
        
    #elif defined(__APPLE__)
        CFUUIDRef uuid = CFUUIDCreate(kCFAllocatorDefault);
        CFUUIDBytes bytes = CFUUIDGetUUIDBytes(uuid);
        CFRelease(uuid);
        
        ByteArray guidBytes = {
            bytes.byte0, bytes.byte1, bytes.byte2, bytes.byte3,
            bytes.byte4, bytes.byte5, bytes.byte6, bytes.byte7,
            bytes.byte8, bytes.byte9, bytes.byte10, bytes.byte11,
            bytes.byte12, bytes.byte13, bytes.byte14, bytes.byte15
        };
        
        return FGuid(std::move(guidBytes));
        
    #elif defined(__linux__) || defined(__unix__)
        uuid_t uuid;
        uuid_generate(uuid);
        
        ByteArray bytes;
        std::copy(std::begin(uuid), std::end(uuid), bytes.begin());
        
        return FGuid(std::move(bytes));
        
    #else
        // Fallback to random generation (not cryptographically secure)
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<uint64_t> dis;
        
        ByteArray bytes;
        uint64_t* ptr = reinterpret_cast<uint64_t*>(bytes.data());
        ptr[0] = dis(gen);
        ptr[1] = dis(gen);
        
        // Set version (4) and variant bits
        bytes[6] = (bytes[6] & 0x0F) | 0x40;
        bytes[8] = (bytes[8] & 0x3F) | 0x80;
        
        return FGuid(std::move(bytes));
    #endif
    }
    
    FGuid FGuid::NewDeterministic(FStringView seed)
    {
        const uint64 H0 = Hash::XXHash::GetHash64(seed.data(), seed.size());

        uint64 H1 = H0 + 0x9E3779B97F4A7C15ULL;
        H1 = (H1 ^ (H1 >> 30)) * 0xBF58476D1CE4E5B9ULL;
        H1 = (H1 ^ (H1 >> 27)) * 0x94D049BB133111EBULL;
        H1 =  H1 ^ (H1 >> 31);

        ByteArray bytes{};
        for (size_t i = 0; i < 8; ++i)
        {
            bytes[i]     = static_cast<uint8>((H0 >> (i * 8)) & 0xFFu);
            bytes[i + 8] = static_cast<uint8>((H1 >> (i * 8)) & 0xFFu);
        }

        // RFC-4122-ish version (5: name-based) and variant bits.
        bytes[6] = (bytes[6] & 0x0F) | 0x50;
        bytes[8] = (bytes[8] & 0x3F) | 0x80;

        return FGuid(bytes);
    }
    
    bool FGuid::TryParseInternal(FStringView str, ByteArray& outBytes)
    {
        if (str.length() >= 2 && str[0] == '{' && str.back() == '}')
        {
            str = str.substr(1, str.length() - 2);
        }
        
        auto isHex = [](char c) {
            return (c >= '0' && c <= '9') || 
                   (c >= 'a' && c <= 'f') || 
                   (c >= 'A' && c <= 'F');
        };
        
        auto hexValue = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        
        size_t byteIdx = 0;
        size_t i = 0;
        char firstNibble = 0;
        bool hasFirstNibble = false;
        
        while (i < str.length() && byteIdx < GUID_SIZE)
        {
            char c = str[i++];
            
            if (c == '-') continue;
            
            if (!isHex(c))
            {
                return false;
            }
            
            if (!hasFirstNibble)
            {
                firstNibble = c;
                hasFirstNibble = true;
            }
            else
            {
                outBytes[byteIdx++] = (hexValue(firstNibble) << 4) | hexValue(c);
                hasFirstNibble = false;
            }
        }
        
        return byteIdx == GUID_SIZE && !hasFirstNibble;
    }
    
    FGuid FGuid::FromString(FStringView str)
    {
        ByteArray bytes{};
        if (TryParseInternal(str, bytes))
        {
            return FGuid(bytes);
        }
        return FGuid();
    }
    
    TOptional<FGuid> FGuid::TryParse(FStringView str)
    {
        ByteArray bytes{};
        if (TryParseInternal(str, bytes))
        {
            return FGuid(bytes);
        }
        return eastl::nullopt;
    }
    
    bool FGuid::operator==(const FGuid& other) const noexcept
    {
        return Bytes == other.Bytes;
    }
    
    bool FGuid::operator!=(const FGuid& other) const noexcept
    {
        return Bytes != other.Bytes;
    }
    
    bool FGuid::operator<(const FGuid& other) const noexcept
    {
        return Bytes < other.Bytes;
    }
    
    bool FGuid::operator<=(const FGuid& other) const noexcept
    {
        return Bytes <= other.Bytes;
    }
    
    bool FGuid::operator>(const FGuid& other) const noexcept
    {
        return Bytes > other.Bytes;
    }
    
    bool FGuid::operator>=(const FGuid& other) const noexcept
    {
        return Bytes >= other.Bytes;
    }
    
    FString FGuid::ToString(bool uppercase, bool includeDashes) const
    {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        
        if (uppercase)
        {
            oss << std::uppercase;
        }
        
        for (size_t i = 0; i < GUID_SIZE; ++i)
        {
            if (includeDashes && (i == 4 || i == 6 || i == 8 || i == 10))
            {
                oss << '-';
            }
            oss << std::setw(2) << static_cast<int>(Bytes[i]);
        }
        
        return oss.str().c_str();
    }
    
    FString FGuid::ToShortString() const
    {
        return ToString(false, false);
    }
    
    bool FGuid::IsValid() const noexcept
    {
        return *this != Empty();
    }
    
    void FGuid::Invalidate() noexcept
    {
        Bytes.fill(0);
    }
    
    void FGuid::Swap(FGuid& other) noexcept
    {
        Bytes.swap(other.Bytes);
    }
    
    size_t FGuid::Hash() const noexcept
    {
        return Hash::GetHash64(Bytes.data(), Bytes.size());
    }
    
    std::ostream& operator<<(std::ostream& os, const FGuid& guid)
    {
        os << guid.ToString();
        return os;
    }
    
    std::istream& operator>>(std::istream& is, FGuid& guid)
    {
        std::string str;
        is >> str;
        guid = FGuid::FromString(str.c_str());
        return is;
    }
}
