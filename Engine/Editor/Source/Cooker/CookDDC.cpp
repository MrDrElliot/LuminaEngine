#include "CookDDC.h"

#include "Core/Math/Hash/Hash.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include "Platform/Filesystem/PlatformFilesystem.h"


namespace Lumina
{
    namespace
    {
        std::atomic<size_t> GHits{0};
        std::atomic<size_t> GMisses{0};
        std::atomic<size_t> GWrittenBytes{0};

        // On-disk header; bump kFileVersion if the wire format changes shape (independent of kCookStamp, which keys the payload contents).
        struct FDDCFileHeader
        {
            char   Magic[4];        // 'LDDC'
            uint32 FileVersion;     // see kFileVersion
            uint64 PayloadHash;     // xxh64 of payload bytes (no header)
        };
        static_assert(sizeof(FDDCFileHeader) == 16, "DDC header must stay packed");

        constexpr char     kMagic[4]      = { 'L', 'D', 'D', 'C' };
        constexpr uint32   kFileVersion   = 1;

        FString DDCBaseDir()
        {
            FString Out = Paths::GetEngineInstallDirectory();
            if (Out.empty()) return {};
            Out += "/Intermediates/DDC";
            return Out;
        }

        FString DDCPathFor(const FCookInputHash& Key)
        {
            char Hex[17];
            std::snprintf(Hex, sizeof(Hex), "%016llx",
                static_cast<unsigned long long>(Key.Hash));

            FString Out = DDCBaseDir();
            if (Out.empty()) return {};
            Out += '/';
            Out += Hex[0];
            Out += Hex[1];
            Out += '/';
            Out += Hex;
            Out += ".ddc";
            return Out;
        }
    }


    FCookInputHash FCookDDC::ComputeKey(uint64 SourceContentHash)
    {
        if (SourceContentHash == 0)
        {
            return {};
        }
        // Mix the cook stamp via a large odd multiplier so a stamp bump shuffles every key off its prior bucket (not cryptographic; source hash dominates collision risk).
        static constexpr uint64 kMixer = 0x9E3779B97F4A7C15ull;
        const uint64 H = SourceContentHash ^ (static_cast<uint64>(kCookStamp) * kMixer);
        return { H == 0 ? 1ull : H };
    }

    bool FCookDDC::TryGet(const FCookInputHash& Key, TVector<uint8>& OutBytes)
    {
        if (!Key.IsValid())
        {
            return false;
        }

        const FString Path = DDCPathFor(Key);
        if (Path.empty())
        {
            LOG_WARN("[CookDDC] TryGet: engine install dir not set; cache disabled");
            return false;
        }

        if (!Filesystem::Exists(Path))
        {
            GMisses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        TVector<uint8> Raw;
        if (!FileHelper::LoadFileToArray(Raw, Path))
        {
            GMisses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // A short file, wrong magic, version mismatch or payload hash miss all mean a torn or stale entry.
        auto DropCorrupt = [&](const char* Why)
        {
            LOG_WARN("[CookDDC] Discarding corrupt entry {}: {}", Path, Why);
            Filesystem::RemoveFile(Path);
            GMisses.fetch_add(1, std::memory_order_relaxed);
        };

        if (Raw.size() < sizeof(FDDCFileHeader))
        {
            DropCorrupt("short file");
            return false;
        }

        FDDCFileHeader Header{};
        std::memcpy(&Header, Raw.data(), sizeof(Header));
        if (std::memcmp(Header.Magic, kMagic, sizeof(kMagic)) != 0)
        {
            DropCorrupt("bad magic");
            return false;
        }
        if (Header.FileVersion != kFileVersion)
        {
            DropCorrupt("file-format version mismatch");
            return false;
        }

        const size_t PayloadSize = Raw.size() - sizeof(FDDCFileHeader);
        const uint8* PayloadPtr  = Raw.data() + sizeof(FDDCFileHeader);
        const uint64 PayloadHash = PayloadSize > 0
            ? Hash::XXHash::GetHash64(PayloadPtr, PayloadSize)
            : 0ull;
        if (PayloadHash != Header.PayloadHash)
        {
            DropCorrupt("payload hash mismatch");
            return false;
        }

        OutBytes.assign(PayloadPtr, PayloadPtr + PayloadSize);

        GHits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool FCookDDC::Put(const FCookInputHash& Key, const TVector<uint8>& Bytes)
    {
        if (!Key.IsValid() || Bytes.empty())
        {
            return false;
        }

        const FString Path = DDCPathFor(Key);
        if (Path.empty())
        {
            LOG_WARN("[CookDDC] Put: engine install dir not set; cache disabled");
            return false;
        }

        if (!Filesystem::MakeParentDirectoryTree(Path))
        {
            LOG_WARN("[CookDDC] could not create the parent directory of {}", Path);
            return false;
        }

        FDDCFileHeader Header{};
        std::memcpy(Header.Magic, kMagic, sizeof(kMagic));
        Header.FileVersion = kFileVersion;
        Header.PayloadHash = Hash::XXHash::GetHash64(Bytes.data(), Bytes.size());

        TVector<uint8> Framed;
        Framed.reserve(sizeof(Header) + Bytes.size());
        Framed.resize(sizeof(Header));
        std::memcpy(Framed.data(), &Header, sizeof(Header));
        Framed.insert(Framed.end(), Bytes.begin(), Bytes.end());

        // A kill mid-write would otherwise leave a short file that TryGet accepts as truth.
        if (!Filesystem::AtomicWriteFile(Path, TSpan<const uint8>(Framed.data(), Framed.size())))
        {
            LOG_WARN("[CookDDC] Put: failed to publish {}", Path);
            return false;
        }

        GWrittenBytes.fetch_add(Bytes.size(), std::memory_order_relaxed);
        return true;
    }

    void   FCookDDC::Reset()        { GHits = 0; GMisses = 0; GWrittenBytes = 0; }
    size_t FCookDDC::Hits()         { return GHits.load(std::memory_order_relaxed); }
    size_t FCookDDC::Misses()       { return GMisses.load(std::memory_order_relaxed); }
    size_t FCookDDC::WrittenBytes() { return GWrittenBytes.load(std::memory_order_relaxed); }
}
