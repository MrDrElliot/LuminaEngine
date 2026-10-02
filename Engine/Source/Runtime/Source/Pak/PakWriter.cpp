#include "RuntimePCH.h"
#include "PakWriter.h"
#include <fstream>
#include "miniz.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Core/Profiler/Profile.h"
#include "TaskSystem/TaskSystem.h"
#include "Log/Log.h"

namespace Lumina
{
    bool FPakWriter::AddEntry(FStringView VirtualPath, TSpan<const uint8> Data)
    {
        return AddEntry(VirtualPath, TVector<uint8>(Data.begin(), Data.end()));
    }

    bool FPakWriter::AddEntry(FStringView VirtualPath, FStringView Data)
    {
        return AddEntry(VirtualPath, TSpan<const uint8>(reinterpret_cast<const uint8*>(Data.data()), Data.size()));
    }

    bool FPakWriter::AddEntry(FStringView VirtualPath, TVector<uint8>&& Data, bool bPrecompressed)
    {
        FFixedString Key(VirtualPath.data(), VirtualPath.size());

        if (!SeenPaths.insert(Key).second)
        {
            // Silently de-dup; dependency walker can revisit assets via different paths.
            return false;
        }

        FPendingEntry Pending;
        Pending.VirtualPath    = Move(Key);
        Pending.Data           = Move(Data);
        Pending.bPrecompressed = bPrecompressed;
        TotalDataSize += Pending.Data.size();
        Entries.emplace_back(Move(Pending));
        return true;
    }

    void FPakWriter::GetEntryPaths(TVector<FFixedString>& OutPaths) const
    {
        OutPaths.clear();
        OutPaths.reserve(Entries.size());
        for (const FPendingEntry& Entry : Entries)
        {
            OutPaths.push_back(Entry.VirtualPath);
        }
    }

    bool FPakWriter::Finalize(FStringView NativeFilePath)
    {
        LUMINA_PROFILE_SCOPE();

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

        struct FEncoded
        {
            TVector<uint8> Compressed;
            uint64         ContentHash = 0;
            uint8          Method = (uint8)EPakCompression::None;
        };

        TVector<FWriteRecord> Records;
        Records.reserve(Entries.size());
        size_t TotalCompressed = 0;

        // Encoded in parallel batches and written in order, so the file is deterministic and only one batch of output is held at once.
        constexpr size_t kBatchSize = 512;
        TVector<FEncoded> Batch;
        for (size_t First = 0; First < Entries.size(); First += kBatchSize)
        {
            const size_t Count = Math::Min(kBatchSize, Entries.size() - First);
            Batch.clear();
            Batch.resize(Count);

            Task::ParallelFor((uint32)Count, [&](uint32 i)
            {
                const FPendingEntry& Entry = Entries[First + i];
                FEncoded& Out = Batch[i];
                const uint64 Original = (uint64)Entry.Data.size();
                Out.ContentHash = Original > 0 ? Hash::XXHash::GetHash64(Entry.Data.data(), Entry.Data.size()) : 0;

                if (Entry.bPrecompressed || Original < PAK_COMPRESSION_MIN_SIZE)
                {
                    return;
                }

                mz_ulong OutLen = mz_compressBound((mz_ulong)Original);
                Out.Compressed.resize((size_t)OutLen);
                const int Ret = mz_compress2(Out.Compressed.data(), &OutLen, Entry.Data.data(), (mz_ulong)Original, MZ_DEFAULT_COMPRESSION);
                if (Ret == MZ_OK && OutLen < Original)
                {
                    Out.Compressed.resize((size_t)OutLen);
                    Out.Method = (uint8)EPakCompression::Deflate;
                }
                else
                {
                    Out.Compressed.clear();
                }
            }, 1);

            for (size_t i = 0; i < Count; ++i)
            {
                const FPendingEntry& Entry = Entries[First + i];
                const FEncoded& Encoded = Batch[i];
                const bool bDeflated = Encoded.Method == (uint8)EPakCompression::Deflate;

                FWriteRecord Rec{};
                Rec.Offset           = (uint64)File.tellp();
                Rec.UncompressedSize = (uint64)Entry.Data.size();
                Rec.ContentHash      = Encoded.ContentHash;
                Rec.Method           = Encoded.Method;
                Rec.CompressedSize   = bDeflated ? (uint64)Encoded.Compressed.size() : Rec.UncompressedSize;

                const uint8* WritePtr = bDeflated ? Encoded.Compressed.data() : Entry.Data.data();
                if (Rec.CompressedSize > 0)
                {
                    File.write(reinterpret_cast<const char*>(WritePtr), (std::streamsize)Rec.CompressedSize);
                }

                TotalCompressed += (size_t)Rec.CompressedSize;
                Records.emplace_back(Rec);
            }
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
