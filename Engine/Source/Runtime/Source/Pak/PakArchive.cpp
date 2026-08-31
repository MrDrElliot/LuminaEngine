#include "RuntimePCH.h"
#include "PakArchive.h"
#include <fstream>
#include "miniz.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Log/Log.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    namespace
    {
        // Bounds-checked POD read; truncated PAKs fail cleanly.
        template<typename T>
        bool ReadPOD(const uint8* Data, size_t Size, size_t& Offset, T& Out)
        {
            if (Offset + sizeof(T) > Size)
            {
                return false;
            }
            std::memcpy(&Out, Data + Offset, sizeof(T));
            Offset += sizeof(T);
            return true;
        }

        FString TopLevelOf(FStringView Path)
        {
            if (Path.empty() || Path[0] != '/')
            {
                return FString();
            }
            const size_t Second = Path.find('/', 1);
            return Second == FStringView::npos
                ? FString(Path.data(), Path.size())
                : FString(Path.data(), Path.data() + Second);
        }

        struct FParsedEntry
        {
            FFixedString Path;
            uint64 Offset;
            uint64 CompressedSize;
            uint64 UncompressedSize;
            uint64 ContentHash;     // verified on load
            uint8  Method;
        };
    }

    TSharedPtr<FPakArchive> FPakArchive::Open(FStringView NativeFilePath)
    {
        // Native IO; PAK is read before VFS mounts exist.
        std::ifstream File(FString(NativeFilePath.data(), NativeFilePath.size()).c_str(), std::ios::binary | std::ios::ate);
        if (!File)
        {
            LOG_ERROR("FPakArchive: cannot open '{}'", FString(NativeFilePath.data(), NativeFilePath.size()).c_str());
            return nullptr;
        }

        const std::streamsize FileSize = File.tellg();
        if (FileSize < (std::streamsize)sizeof(FPakHeader))
        {
            LOG_ERROR("FPakArchive: '{}' is too small to be a PAK", FString(NativeFilePath.data(), NativeFilePath.size()).c_str());
            return nullptr;
        }

        TVector<uint8> Compressed;
        Compressed.resize((size_t)FileSize);

        File.seekg(0, std::ios::beg);
        if (!File.read(reinterpret_cast<char*>(Compressed.data()), FileSize))
        {
            LOG_ERROR("FPakArchive: read failed for '{}'", FString(NativeFilePath.data(), NativeFilePath.size()).c_str());
            return nullptr;
        }

        const uint8* Data = Compressed.data();
        const size_t Size = Compressed.size();

        FPakHeader Header{};
        size_t Cursor = 0;
        if (!ReadPOD(Data, Size, Cursor, Header))
        {
            LOG_ERROR("FPakArchive: header read failed");
            return nullptr;
        }

        if (Header.Magic != PAK_MAGIC)
        {
            LOG_ERROR("FPakArchive: bad magic 0x{:08X} (expected 0x{:08X})", Header.Magic, PAK_MAGIC);
            return nullptr;
        }
        if (Header.Version != PAK_VERSION)
        {
            LOG_ERROR("FPakArchive: version {} not supported (expected {})", Header.Version, PAK_VERSION);
            return nullptr;
        }
        if (Header.TocOffset >= Size)
        {
            LOG_ERROR("FPakArchive: TOC offset {} past end of file ({})", Header.TocOffset, Size);
            return nullptr;
        }

        Cursor = (size_t)Header.TocOffset;

        TVector<FParsedEntry> Parsed;
        Parsed.reserve(Header.EntryCount);

        uint64 TotalUncompressed = 0;

        for (uint32 i = 0; i < Header.EntryCount; ++i)
        {
            uint32 PathLen = 0;
            if (!ReadPOD(Data, Size, Cursor, PathLen) || Cursor + PathLen > Size)
            {
                LOG_ERROR("FPakArchive: TOC entry {} path read failed", i);
                return nullptr;
            }

            FFixedString Path(reinterpret_cast<const char*>(Data + Cursor), PathLen);
            Cursor += PathLen;

            FParsedEntry P{};
            P.Path = Move(Path);
            uint8 Pad[7] = {};
            if (!ReadPOD(Data, Size, Cursor, P.Offset)
             || !ReadPOD(Data, Size, Cursor, P.CompressedSize)
             || !ReadPOD(Data, Size, Cursor, P.UncompressedSize)
             || !ReadPOD(Data, Size, Cursor, P.ContentHash)
             || !ReadPOD(Data, Size, Cursor, P.Method)
             || Cursor + sizeof(Pad) > Size)
            {
                LOG_ERROR("FPakArchive: TOC entry {} fields read failed", i);
                return nullptr;
            }
            Cursor += sizeof(Pad);

            // Catch truncated PAKs.
            if (P.Offset + P.CompressedSize > (uint64)Size || P.Offset >= (uint64)Size)
            {
                LOG_ERROR("FPakArchive: entry '{}' points outside the file (offset={}, comp={}, file size={})",
                    P.Path, P.Offset, P.CompressedSize, Size);
                return nullptr;
            }

            TotalUncompressed += P.UncompressedSize;
            Parsed.emplace_back(Move(P));
        }

        TSharedPtr<FPakArchive> Archive(new FPakArchive{});
        Archive->RawData.resize((size_t)TotalUncompressed);
        Archive->Entries.reserve(Parsed.size());

        uint64 DestCursor = 0;
        for (const FParsedEntry& P : Parsed)
        {
            const uint8* Src = Data + P.Offset;
            uint8* Dst = Archive->RawData.data() + DestCursor;

            if (P.Method == (uint8)EPakCompression::None)
            {
                if (P.CompressedSize != P.UncompressedSize)
                {
                    LOG_ERROR("FPakArchive: entry '{}' uncompressed but sizes differ (comp={}, uncomp={})",
                        P.Path.c_str(), P.CompressedSize, P.UncompressedSize);
                    return nullptr;
                }
                if (P.UncompressedSize > 0)
                {
                    std::memcpy(Dst, Src, (size_t)P.UncompressedSize);
                }
            }
            else if (P.Method == (uint8)EPakCompression::Deflate)
            {
                mz_ulong OutLen = (mz_ulong)P.UncompressedSize;
                int Ret = mz_uncompress(Dst, &OutLen, Src, (mz_ulong)P.CompressedSize);
                if (Ret != MZ_OK || OutLen != P.UncompressedSize)
                {
                    LOG_ERROR("FPakArchive: decompress failed for '{}' (ret={}, got={}, expected={})",
                        P.Path.c_str(), Ret, (uint64)OutLen, P.UncompressedSize);
                    return nullptr;
                }
            }
            else
            {
                LOG_ERROR("FPakArchive: entry '{}' has unknown compression method {}",
                    P.Path.c_str(), (uint32)P.Method);
                return nullptr;
            }

            // A hash of 0 means not set, from a legacy or empty entry, and is accepted silently.
            if (P.ContentHash != 0 && P.UncompressedSize > 0)
            {
                const uint64 Actual = Hash::XXHash::GetHash64(Dst, (size_t)P.UncompressedSize);
                if (Actual != P.ContentHash)
                {
                    LOG_ERROR("FPakArchive: content hash mismatch for '{}' (expected 0x{:016X}, got 0x{:016X}); PAK is corrupt",
                        P.Path.c_str(), P.ContentHash, Actual);
                    return nullptr;
                }
            }

            FPakEntry Entry{};
            Entry.Offset = DestCursor;
            Entry.Size   = P.UncompressedSize;
            Archive->Entries.emplace(P.Path, Entry);

            DestCursor += P.UncompressedSize;
        }

        LOG_INFO("FPakArchive: loaded '{}' ({} entries, {} bytes on disk, {} bytes uncompressed)",
            FString(NativeFilePath.data(), NativeFilePath.size()).c_str(),
            Header.EntryCount, Size, TotalUncompressed);

        return Archive;
    }

    TSpan<const uint8> FPakArchive::ReadEntry(FStringView VirtualPath) const
    {
        FFixedString Key(VirtualPath.data(), VirtualPath.size());
        auto It = Entries.find(Key);
        if (It == Entries.end())
        {
            return {};
        }
        return TSpan<const uint8>(RawData.data() + It->second.Offset, (size_t)It->second.Size);
    }

    bool FPakArchive::HasEntry(FStringView VirtualPath) const
    {
        FFixedString Key(VirtualPath.data(), VirtualPath.size());
        return Entries.find(Key) != Entries.end();
    }

    size_t FPakArchive::EntrySize(FStringView VirtualPath) const
    {
        FFixedString Key(VirtualPath.data(), VirtualPath.size());
        auto It = Entries.find(Key);
        return It == Entries.end() ? 0 : (size_t)It->second.Size;
    }

    TVector<FString> FPakArchive::GetTopLevelAliases() const
    {
        TVector<FString> Out;
        for (const auto& [Key, Entry] : Entries)
        {
            FString Alias = TopLevelOf(FStringView(Key.data(), Key.size()));
            if (!Alias.empty() && !Algo::Contains(Out, Alias))
            {
                Out.push_back(Move(Alias));
            }
        }
        return Out;
    }

    void FPakArchive::ForEachEntry(const TFunction<void(FStringView, size_t)>& Func) const
    {
        for (const auto& [Key, Entry] : Entries)
        {
            Func(FStringView(Key.data(), Key.size()), (size_t)Entry.Size);
        }
    }

    void FPakArchive::ForEachEntryUnder(FStringView Prefix, const TFunction<void(FStringView, size_t)>& Func) const
    {
        for (const auto& [Key, Entry] : Entries)
        {
            FStringView KeyView(Key.data(), Key.size());
            if (KeyView == Prefix)
            {
                continue;
            }
            if (KeyView.size() > Prefix.size()
                && KeyView.starts_with(Prefix)
                && KeyView[Prefix.size()] == '/')
            {
                Func(KeyView, (size_t)Entry.Size);
            }
        }
    }
}
