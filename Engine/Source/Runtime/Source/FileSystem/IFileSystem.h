#pragma once

#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/String.h"

namespace Lumina::VFS
{
    struct FFileInfo;

    class RUNTIME_API IFileSystem
    {
    public:

        virtual ~IFileSystem() = default;

        virtual bool ReadFile(TVector<uint8>& Result, FStringView Path) = 0;
        virtual bool ReadFile(FString& OutString, FStringView Path) = 0;

        // Only the native backend reads past one int32-counted buffer; the rest go through it and hand the block over.
        virtual bool ReadFile(TVector64<uint8>& Result, FStringView Path)
        {
            TVector<uint8> Bytes;
            if (!ReadFile(Bytes, Path))
            {
                return false;
            }
            Result = Move(Bytes);
            return true;
        }

        /** Read [Offset, Offset + Size) of Path. A read that runs past EOF is clamped, so Result may come
         *  back shorter than Size; only a missing/unreadable file returns false. The default pulls the whole
         *  file and slices it -- correct, but it defeats the point, so backends that can seek override it. */
        virtual bool ReadFileRange(TVector<uint8>& Result, FStringView Path, int64 Offset, int64 Size);

        virtual bool WriteFile(FStringView Path, FStringView Data) = 0;
        virtual bool WriteFile(FStringView Path, TSpan<const uint8> Data) = 0;

        // Crash-safe write (all-or-nothing); default falls back to WriteFile for backends with no atomic primitive.
        virtual bool AtomicWriteFile(FStringView Path, TSpan<const uint8> Data) { return WriteFile(Path, Data); }

        /** Crash-safe write of Prefix, then SrcSize bytes copied straight out of SrcPath at SrcOffset, then
         *  Suffix. The spliced middle is streamed in fixed chunks and never lands in a buffer of its own,
         *  which is the entire reason this exists: it is how a package rename carries a multi-megabyte bulk
         *  region into the new file without materializing a single mip.
         *
         *  Returns false when the backend cannot do it OR the copy failed, so every caller needs a path
         *  that does not depend on it. Nothing is written unless the whole thing succeeds. */
        virtual bool AtomicWriteFileSpliced(FStringView Path, TSpan<const uint8> Prefix,
                                            FStringView SrcPath, int64 SrcOffset, int64 SrcSize,
                                            TSpan<const uint8> Suffix) { return false; }

        virtual bool Exists(FStringView Path) const = 0;
        virtual bool IsDirectory(FStringView Path) const = 0;
        virtual bool IsEmpty(FStringView Path) const = 0;
        virtual int64 Size(FStringView Path) const = 0;

        virtual bool CreateDir(FStringView Path) = 0;
        virtual bool Remove(FStringView Path) = 0;
        virtual bool RemoveAll(FStringView Path) = 0;
        virtual bool Rename(FStringView Old, FStringView New) = 0;

        virtual void PlatformOpen(FStringView Path) const = 0;

        // The info is the walk's own copy, so a callback that keeps it may move from it.
        virtual void DirectoryIterator(FStringView Path, const TFunction<void(FFileInfo&)>& Callback) const = 0;
        virtual void RecursiveDirectoryIterator(FStringView Path, const TFunction<void(FFileInfo&)>& Callback) const = 0;

        // Disk path this mount would serve Path from, or empty for backends with no on-disk file.
        virtual FPathString ResolveToDiskPath(FStringView Path) const { return {}; }

        virtual FStringView GetAliasPath() const = 0;
        virtual FStringView GetBasePath() const = 0;

        virtual bool IsReadOnly() const { return false; }
    };
}
