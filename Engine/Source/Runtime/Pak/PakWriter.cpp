#include "pch.h"
#include "PakWriter.h"
#include <fstream>
#include "miniz.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Log/Log.h"

namespace Lumina
{
    bool FPakWriter::AddEntry(FStringView VirtualPath, TSpan<const uint8> Data)
    {
        FFixedString Key(VirtualPath.data(), VirtualPath.size());

        if (!SeenPaths.insert(Key).second)
        {
            // Silently de-dup; dependency walker can revisit assets via different paths.
            return false;
        }

        FPendingEntry Pending;
        Pending.VirtualPath = Move(Key);
        Pending.Data.assign(Data.begin(), Data.end());
        TotalDataSize += Pending.Data.size();
        Entries.emplace_back(Move(Pending));
        return true;
    }

    bool FPakWriter::AddEntry(FStringView VirtualPath, FStringView Data)
    {
        return AddEntry(VirtualPath, TSpan<const uint8>(reinterpret_cast<const uint8*>(Data.data()), Data.size()));
    }

    bool FPakWriter::Finalize(FStringView NativeFilePath)
    {
        std::ofstream File(FString(NativeFilePath.data(), NativeFilePath.size()).c_str(), std::ios::binary | std::ios::trunc);
        if (!File)
        {
            LOG_ERROR("FPakWriter: cannot open '{}' for write", FString(NativeFilePath.data(), NativeFilePath.size()).c_str());
            return false;
        }

        // Reserve header; patched with real TocOffset at end.
        FPakHeader Header{};
        Header.Magic      = PAK_MAGIC;
        Header.Version    = PAK_VERSION;
        Header.EntryCount = (uint32)Entries.size();
        Header.TocOffset  = 0;
        Header._Pad       = 0;

        File.write(reinterpret_cast<const char*>(&Header), sizeof(Header));

        struct FWriteRecord
        {
            uint64 Offset;
            uint64 CompressedSize;
            uint64 UncompressedSize;
            uint64 ContentHash;     // xxh64 of UNCOMPRESSED bytes; v3 integrity check.
            uint8  Method;
        };

        TVector<FWriteRecord> Records;
        Records.reserve(Entries.size());

        TVector<uint8> Scratch;
        size_t TotalCompressed = 0;

        for (const FPendingEntry& Entry : Entries)
        {
            const uint64 Offset = (uint64)File.tellp();
            const uint64 Original = (uint64)Entry.Data.size();

            FWriteRecord Rec{};
            Rec.Offset = Offset;
            Rec.UncompressedSize = Original;
            Rec.ContentHash = Original > 0
                ? Hash::XXHash::GetHash64(Entry.Data.data(), Entry.Data.size())
                : 0;

            const uint8* WritePtr = Entry.Data.data();
            uint64 WriteSize = Original;
            uint8 Method = (uint8)EPakCompression::None;

            if (Original >= PAK_COMPRESSION_MIN_SIZE)
            {
                mz_ulong Bound = mz_compressBound((mz_ulong)Original);
                Scratch.resize((size_t)Bound);

                mz_ulong OutLen = Bound;
                int Ret = mz_compress2(Scratch.data(), &OutLen, Entry.Data.data(), (mz_ulong)Original, MZ_DEFAULT_COMPRESSION);

                if (Ret == MZ_OK && OutLen < Original)
                {
                    WritePtr = Scratch.data();
                    WriteSize = (uint64)OutLen;
                    Method = (uint8)EPakCompression::Deflate;
                }
            }

            if (WriteSize > 0)
            {
                File.write(reinterpret_cast<const char*>(WritePtr), (std::streamsize)WriteSize);
            }

            Rec.CompressedSize = WriteSize;
            Rec.Method = Method;
            TotalCompressed += (size_t)WriteSize;
            Records.emplace_back(Rec);
        }

        const uint64 TocOffset = (uint64)File.tellp();
        for (size_t i = 0; i < Entries.size(); ++i)
        {
            const FPendingEntry& Entry = Entries[i];
            const FWriteRecord& Rec = Records[i];

            const uint32 PathLen = (uint32)Entry.VirtualPath.size();
            File.write(reinterpret_cast<const char*>(&PathLen), sizeof(PathLen));
            if (PathLen > 0)
            {
                File.write(Entry.VirtualPath.c_str(), PathLen);
            }

            File.write(reinterpret_cast<const char*>(&Rec.Offset),           sizeof(Rec.Offset));
            File.write(reinterpret_cast<const char*>(&Rec.CompressedSize),   sizeof(Rec.CompressedSize));
            File.write(reinterpret_cast<const char*>(&Rec.UncompressedSize), sizeof(Rec.UncompressedSize));
            File.write(reinterpret_cast<const char*>(&Rec.ContentHash),      sizeof(Rec.ContentHash));
            File.write(reinterpret_cast<const char*>(&Rec.Method),           sizeof(Rec.Method));

            const uint8 Pad[7] = {};
            File.write(reinterpret_cast<const char*>(Pad), sizeof(Pad));
        }

        // Patch header with TocOffset.
        Header.TocOffset = TocOffset;
        File.seekp(0);
        File.write(reinterpret_cast<const char*>(&Header), sizeof(Header));

        if (!File.good())
        {
            LOG_ERROR("FPakWriter: write failed for '{}'", FString(NativeFilePath.data(), NativeFilePath.size()).c_str());
            return false;
        }

        const double Ratio = TotalDataSize > 0
            ? (double)TotalCompressed / (double)TotalDataSize
            : 1.0;

        LOG_INFO("FPakWriter: wrote '{}' ({} entries, {} -> {} bytes, ratio {:.2f}, TOC at {})",
            FString(NativeFilePath.data(), NativeFilePath.size()).c_str(),
            (uint32)Entries.size(), TotalDataSize, TotalCompressed, Ratio, TocOffset);
        return true;
    }
}
