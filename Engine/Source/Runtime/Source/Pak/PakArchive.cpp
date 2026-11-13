#include "RuntimePCH.h"
#include "PakArchive.h"
#include <fstream>
#include "miniz.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Log/Log.h"
#include "Memory/Memcpy.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    namespace
    {
        // Bounds-checked POD read; truncated PAKs fail cleanly.
        template<typename T>
        bool ReadPOD(const uint8* Data, int64 Size, int64& Offset, T& Out)
        {
            if (Offset < 0 || Offset + static_cast<int64>(sizeof(T)) > Size)
            {
                return false;
            }
            Memory::Memcpy(&Out, Data + Offset, sizeof(T));
            Offset += static_cast<int64>(sizeof(T));
            return true;
        }

        FString TopLevelOf(FStringView Path)
        {
            if (Path.IsEmpty() || Path[0] != '/')
            {
                return FString();
            }
            const int32 Second = Path.Find('/', 1);
            return Second == Constants::kIndexNone
                ? FString(Path.GetData(), Path.Len())
                : FString(Path.GetData(), Path.GetData() + Second);
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
        std::ifstream File(*FString(NativeFilePath.GetData(), NativeFilePath.Len()), std::ios::binary | std::ios::ate);
        if (!File)
        {
            LOG_ERROR("FPakArchive: cannot open '{}'", *FString(NativeFilePath.GetData(), NativeFilePath.Len()));
            return nullptr;
        }

        const std::streamsize FileSize = File.tellg();
        if (FileSize < (std::streamsize)sizeof(FPakHeader))
        {
            LOG_ERROR("FPakArchive: '{}' is too small to be a PAK", *FString(NativeFilePath.GetData(), NativeFilePath.Len()));
            return nullptr;
        }

        TVector64<uint8> Compressed;
        Compressed.SetNum(static_cast<int64>(FileSize));

        File.seekg(0, std::ios::beg);
        if (!File.read(reinterpret_cast<char*>(Compressed.GetData()), FileSize))
        {
            LOG_ERROR("FPakArchive: read failed for '{}'", *FString(NativeFilePath.GetData(), NativeFilePath.Len()));
            return nullptr;
        }

        const uint8* Data = Compressed.GetData();
        const int64 Size = Compressed.Num();

        FPakHeader Header{};
        int64 Cursor = 0;
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
        if (Header.TocOffset >= static_cast<uint64>(Size))
        {
            LOG_ERROR("FPakArchive: TOC offset {} past end of file ({})", Header.TocOffset, Size);
            return nullptr;
        }

        Cursor = static_cast<int64>(Header.TocOffset);

        // Each entry is at least its fixed fields, so a count the table could not hold is caught before it allocates.
        constexpr int64 MinEntryBytes = sizeof(uint32) + sizeof(uint64) * 4 + sizeof(uint8) + 7;
        if (Header.EntryCount > (Size - Cursor) / MinEntryBytes)
        {
            LOG_ERROR("FPakArchive: TOC claims {} entries, more than its {} bytes can hold", Header.EntryCount, Size - Cursor);
            return nullptr;
        }

        TVector<FParsedEntry> Parsed;
        Parsed.Reserve(Header.EntryCount);

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
             || Cursor + static_cast<int64>(sizeof(Pad)) > Size)
            {
                LOG_ERROR("FPakArchive: TOC entry {} fields read failed", i);
                return nullptr;
            }
            Cursor += sizeof(Pad);

            // Compared without adding, so a huge size cannot wrap past the check.
            if (P.Offset >= (uint64)Size || P.CompressedSize > (uint64)Size - P.Offset)
            {
                LOG_ERROR("FPakArchive: entry '{}' points outside the file (offset={}, comp={}, file size={})",
                    P.Path, P.Offset, P.CompressedSize, Size);
                return nullptr;
            }

            // Deflate expands at most about 1032 to 1, so anything claiming more is corrupt rather than a large asset.
            constexpr uint64 MaxDeflateRatio = 1032;
            if (P.UncompressedSize > P.CompressedSize * MaxDeflateRatio + 64 || TotalUncompressed + P.UncompressedSize < TotalUncompressed)
            {
                LOG_ERROR("FPakArchive: entry '{}' claims {} bytes from {} compressed; PAK is corrupt",
                    P.Path, P.UncompressedSize, P.CompressedSize);
                return nullptr;
            }

            // The archive itself counts in int64, but every entry is handed out as one int32-counted buffer.
            if (P.UncompressedSize > static_cast<uint64>(TNumericLimits<int32>::Max()))
            {
                LOG_ERROR("FPakArchive: entry '{}' is {} bytes, more than one entry can hold", P.Path, P.UncompressedSize);
                return nullptr;
            }

            TotalUncompressed += P.UncompressedSize;
            Parsed.Emplace(Move(P));
        }

        TSharedPtr<FPakArchive> Archive(new FPakArchive{});
        Archive->RawData.SetNum(static_cast<int64>(TotalUncompressed));
        Archive->Entries.Reserve(Parsed.Num());

        uint64 DestCursor = 0;
        for (const FParsedEntry& P : Parsed)
        {
            const uint8* Src = Data + P.Offset;
            uint8* Dst = Archive->RawData.GetData() + DestCursor;

            if (P.Method == (uint8)EPakCompression::None)
            {
                if (P.CompressedSize != P.UncompressedSize)
                {
                    LOG_ERROR("FPakArchive: entry '{}' uncompressed but sizes differ (comp={}, uncomp={})",
                        *P.Path, P.CompressedSize, P.UncompressedSize);
                    return nullptr;
                }
                if (P.UncompressedSize > 0)
                {
                    Memory::Memcpy(Dst, Src, (size_t)P.UncompressedSize);
                }
            }
            else if (P.Method == (uint8)EPakCompression::Deflate)
            {
                mz_ulong OutLen = (mz_ulong)P.UncompressedSize;
                int Ret = mz_uncompress(Dst, &OutLen, Src, (mz_ulong)P.CompressedSize);
                if (Ret != MZ_OK || OutLen != P.UncompressedSize)
                {
                    LOG_ERROR("FPakArchive: decompress failed for '{}' (ret={}, got={}, expected={})",
                        *P.Path, Ret, (uint64)OutLen, P.UncompressedSize);
                    return nullptr;
                }
            }
            else
            {
                LOG_ERROR("FPakArchive: entry '{}' has unknown compression method {}",
                    *P.Path, (uint32)P.Method);
                return nullptr;
            }

            // A hash of 0 means not set, from a legacy or empty entry, and is accepted silently.
            if (P.ContentHash != 0 && P.UncompressedSize > 0)
            {
                const uint64 Actual = Hash::XXHash::GetHash64(Dst, (size_t)P.UncompressedSize);
                if (Actual != P.ContentHash)
                {
                    LOG_ERROR("FPakArchive: content hash mismatch for '{}' (expected 0x{:016X}, got 0x{:016X}); PAK is corrupt",
                        *P.Path, P.ContentHash, Actual);
                    return nullptr;
                }
            }

            FPakEntry Entry{};
            Entry.Offset = static_cast<int64>(DestCursor);
            Entry.Size   = static_cast<int64>(P.UncompressedSize);
            Archive->Entries.TryAdd(P.Path, Entry);

            DestCursor += P.UncompressedSize;
        }

        LOG_INFO("FPakArchive: loaded '{}' ({} entries, {} bytes on disk, {} bytes uncompressed)",
            *FString(NativeFilePath.GetData(), NativeFilePath.Len()),
            Header.EntryCount, Size, TotalUncompressed);

        return Archive;
    }

    TSpan<const uint8> FPakArchive::ReadEntry(FStringView VirtualPath) const
    {
        FFixedString Key(VirtualPath.GetData(), VirtualPath.Len());
        auto It = Entries.Find(Key);
        if (It == nullptr)
        {
            return {};
        }
        return TSpan<const uint8>(RawData.GetData() + It->Offset, static_cast<int32>(It->Size));
    }

    bool FPakArchive::HasEntry(FStringView VirtualPath) const
    {
        FFixedString Key(VirtualPath.GetData(), VirtualPath.Len());
        return Entries.Contains(Key);
    }

    int64 FPakArchive::EntrySize(FStringView VirtualPath) const
    {
        FFixedString Key(VirtualPath.GetData(), VirtualPath.Len());
        auto It = Entries.Find(Key);
        return It == nullptr ? 0 : It->Size;
    }

    TVector<FString> FPakArchive::GetTopLevelAliases() const
    {
        TVector<FString> Out;
        for (const auto& [Key, Entry] : Entries)
        {
            FString Alias = TopLevelOf(FStringView(Key.GetData(), Key.Len()));
            if (!Alias.IsEmpty() && !Algo::Contains(Out, Alias))
            {
                Out.Add(Move(Alias));
            }
        }
        return Out;
    }

    void FPakArchive::ForEachEntry(const TFunction<void(FStringView, int64)>& Func) const
    {
        for (const auto& [Key, Entry] : Entries)
        {
            Func(FStringView(Key.GetData(), Key.Len()), Entry.Size);
        }
    }

    void FPakArchive::ForEachEntryUnder(FStringView Prefix, const TFunction<void(FStringView, int64)>& Func) const
    {
        for (const auto& [Key, Entry] : Entries)
        {
            FStringView KeyView(Key.GetData(), Key.Len());
            if (KeyView == Prefix)
            {
                continue;
            }
            if (KeyView.Len() > Prefix.Len()
                && KeyView.StartsWith(Prefix)
                && KeyView[Prefix.Len()] == '/')
            {
                Func(KeyView, Entry.Size);
            }
        }
    }
}
