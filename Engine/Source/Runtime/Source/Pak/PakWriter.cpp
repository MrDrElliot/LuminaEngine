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
        return AddEntry(VirtualPath, TSpan<const uint8>(reinterpret_cast<const uint8*>(Data.GetData()), Data.Len()));
    }

    bool FPakWriter::AddEntry(FStringView VirtualPath, TVector<uint8>&& Data, bool bPrecompressed)
    {
        FFixedString Key(VirtualPath.GetData(), VirtualPath.Len());

        if (!SeenPaths.TryAdd(Key))
        {
            // Silently de-dup; dependency walker can revisit assets via different paths.
            return false;
        }

        FPendingEntry Pending;
        Pending.VirtualPath    = Move(Key);
        Pending.Data           = Move(Data);
        Pending.bPrecompressed = bPrecompressed;
        TotalDataSize += Pending.Data.Num();
        Entries.Emplace(Move(Pending));
        return true;
    }

    void FPakWriter::GetEntryPaths(TVector<FFixedString>& OutPaths) const
    {
        OutPaths.Reset();
        OutPaths.Reserve(Entries.Num());
        for (const FPendingEntry& Entry : Entries)
        {
            OutPaths.Add(Entry.VirtualPath);
        }
    }

    bool FPakWriter::Finalize(FStringView NativeFilePath)
    {
        LUMINA_PROFILE_SCOPE();

        std::ofstream File(*FString(NativeFilePath.GetData(), NativeFilePath.Len()), std::ios::binary | std::ios::trunc);
        if (!File)
        {
            LOG_ERROR("FPakWriter: cannot open '{}' for write", *FString(NativeFilePath.GetData(), NativeFilePath.Len()));
            return false;
        }

        // Reserve header; patched with real TocOffset at end.
        FPakHeader Header{};
        Header.Magic      = PAK_MAGIC;
        Header.Version    = PAK_VERSION;
        Header.EntryCount = (uint32)Entries.Num();
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
        Records.Reserve(Entries.Num());
        int64 TotalCompressed = 0;

        // Encoded in parallel batches and written in order, so the file is deterministic and only one batch of output is held at once.
        constexpr int32 kBatchSize = 512;
        TVector<FEncoded> Batch;
        for (int32 First = 0; First < Entries.Num(); First += kBatchSize)
        {
            const int32 Count = Math::Min(kBatchSize, Entries.Num() - First);
            Batch.Reset();
            Batch.SetNum(Count);

            Jobs::ParallelFor((uint32)Count, [&](uint32 i)
            {
                const FPendingEntry& Entry = Entries[First + i];
                FEncoded& Out = Batch[i];
                const uint64 Original = (uint64)Entry.Data.Num();
                Out.ContentHash = Original > 0 ? Hash::XXHash::GetHash64(Entry.Data.GetData(), Entry.Data.Num()) : 0;

                if (Entry.bPrecompressed || static_cast<size_t>(Original) < PAK_COMPRESSION_MIN_SIZE)
                {
                    return;
                }

                mz_ulong OutLen = mz_compressBound((mz_ulong)Original);
                Out.Compressed.SetNum(IntCastChecked<int32>(OutLen));
                const int Ret = mz_compress2(Out.Compressed.GetData(), &OutLen, Entry.Data.GetData(), (mz_ulong)Original, MZ_DEFAULT_COMPRESSION);
                if (Ret == MZ_OK && OutLen < static_cast<mz_ulong>(Original))
                {
                    Out.Compressed.SetNum(IntCastChecked<int32>(OutLen));
                    Out.Method = (uint8)EPakCompression::Deflate;
                }
                else
                {
                    Out.Compressed.Reset();
                }
            }, 1);

            for (int32 i = 0; i < Count; ++i)
            {
                const FPendingEntry& Entry = Entries[First + i];
                const FEncoded& Encoded = Batch[i];
                const bool bDeflated = Encoded.Method == (uint8)EPakCompression::Deflate;

                FWriteRecord Rec{};
                Rec.Offset           = (uint64)File.tellp();
                Rec.UncompressedSize = (uint64)Entry.Data.Num();
                Rec.ContentHash      = Encoded.ContentHash;
                Rec.Method           = Encoded.Method;
                Rec.CompressedSize   = bDeflated ? (uint64)Encoded.Compressed.Num() : Rec.UncompressedSize;

                const uint8* WritePtr = bDeflated ? Encoded.Compressed.GetData() : Entry.Data.GetData();
                if (Rec.CompressedSize > 0)
                {
                    File.write(reinterpret_cast<const char*>(WritePtr), (std::streamsize)Rec.CompressedSize);
                }

                TotalCompressed += Rec.CompressedSize;
                Records.Emplace(Rec);
            }
        }

        const uint64 TocOffset = (uint64)File.tellp();
        for (int32 i = 0; i < Entries.Num(); ++i)
        {
            const FPendingEntry& Entry = Entries[i];
            const FWriteRecord& Rec = Records[i];

            const uint32 PathLen = (uint32)Entry.VirtualPath.Len();
            File.write(reinterpret_cast<const char*>(&PathLen), sizeof(PathLen));
            if (PathLen > 0)
            {
                File.write(*Entry.VirtualPath, PathLen);
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
            LOG_ERROR("FPakWriter: write failed for '{}'", *FString(NativeFilePath.GetData(), NativeFilePath.Len()));
            return false;
        }

        const double Ratio = TotalDataSize > 0
            ? (double)TotalCompressed / (double)TotalDataSize
            : 1.0;

        LOG_INFO("FPakWriter: wrote '{}' ({} entries, {} -> {} bytes, ratio {:.2f}, TOC at {})",
            *FString(NativeFilePath.GetData(), NativeFilePath.Len()),
            Entries.Num(), TotalDataSize, TotalCompressed, Ratio, TocOffset);
        return true;
    }
}
