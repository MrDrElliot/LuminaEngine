#pragma once

#include "FilesystemTypes.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/String.h"

namespace Lumina::Filesystem
{
    class RUNTIME_API FFileHandle
    {
    public:

        FFileHandle() = default;
        explicit FFileHandle(uintptr_t InHandle) : Handle(InHandle) {}
        ~FFileHandle() { Close(); }

        FFileHandle(const FFileHandle&) = delete;
        FFileHandle& operator=(const FFileHandle&) = delete;

        FFileHandle(FFileHandle&& Other) noexcept : Handle(Other.Handle) { Other.Handle = kInvalidHandle; }

        FFileHandle& operator=(FFileHandle&& Other) noexcept
        {
            if (this != &Other)
            {
                Close();
                Handle = Other.Handle;
                Other.Handle = kInvalidHandle;
            }
            return *this;
        }

        NODISCARD bool IsValid() const          { return Handle != kInvalidHandle; }
        NODISCARD explicit operator bool() const { return IsValid(); }
        NODISCARD uintptr_t GetNativeHandle() const { return Handle; }

        void Close();

        NODISCARD int64 Size() const;
        NODISCARD int64 Tell() const;
        bool Seek(int64 Offset, ESeek Origin = ESeek::Begin);

        // Sizes and offsets are int64 as in Unreal's IFileHandle; a count of zero or less moves nothing.
        int64 Read(void* Dest, int64 Bytes);
        int64 Write(const void* Src, int64 Bytes);

        // Positional, needs no Seek, and is safe to call concurrently on one handle, but it voids Tell().
        int64 ReadAt(void* Dest, int64 Bytes, int64 Offset) const;
        int64 WriteAt(const void* Src, int64 Bytes, int64 Offset) const;

        bool Flush();
        bool Truncate(int64 NewSize);

    private:

        uintptr_t Handle = kInvalidHandle;
    };

    class RUNTIME_API FMappedFile
    {
    public:

        FMappedFile() = default;
        ~FMappedFile() { Close(); }

        FMappedFile(const FMappedFile&) = delete;
        FMappedFile& operator=(const FMappedFile&) = delete;

        FMappedFile(FMappedFile&& Other) noexcept;
        FMappedFile& operator=(FMappedFile&& Other) noexcept;

        static FMappedFile OpenRead(FStringView Path);

        void Close();

        NODISCARD bool IsValid() const              { return Base != nullptr; }
        NODISCARD explicit operator bool() const    { return IsValid(); }
        NODISCARD const uint8* Data() const         { return Base; }
        NODISCARD int64 Size() const                { return Length; }
        NODISCARD TSpan64<const uint8> AsSpan() const { return TSpan64<const uint8>(Base, Length); }

    private:

        const uint8*    Base    = nullptr;
        int64           Length  = 0;
        uintptr_t       Mapping = kInvalidHandle;
        uintptr_t       File    = kInvalidHandle;
    };

    RUNTIME_API FFileHandle Open(FStringView Path, EAccess Access, ECreateMode Mode = ECreateMode::OpenExisting,
                                 EShare Share = EShare::Read, EHint Hint = EHint::None);

    RUNTIME_API EResult GetLastResult();

    NODISCARD RUNTIME_API bool Exists(FStringView Path);
    NODISCARD RUNTIME_API bool IsDirectory(FStringView Path);
    NODISCARD RUNTIME_API bool IsFile(FStringView Path);
    NODISCARD RUNTIME_API FFileStat Stat(FStringView Path);
    NODISCARD RUNTIME_API int64 FileSize(FStringView Path);
    NODISCARD RUNTIME_API int64 LastWriteTime(FStringView Path);
    NODISCARD RUNTIME_API bool IsDirectoryEmpty(FStringView Path);

    RUNTIME_API bool MakeDirectory(FStringView Path);
    RUNTIME_API bool MakeDirectoryTree(FStringView Path);
    RUNTIME_API bool MakeParentDirectoryTree(FStringView Path);

    RUNTIME_API bool RemoveFile(FStringView Path);
    RUNTIME_API bool DeleteDirectory(FStringView Path);
    RUNTIME_API bool RemoveTree(FStringView Path);

    RUNTIME_API bool Move(FStringView From, FStringView To, bool bReplaceExisting = true);
    RUNTIME_API bool Copy(FStringView From, FStringView To, bool bReplaceExisting = true);

    // Mirrors the whole subtree; returns false if any single entry failed, having copied the rest.
    RUNTIME_API bool CopyTree(FStringView From, FStringView To, uint32* OutFilesCopied = nullptr);

    RUNTIME_API bool SetReadOnly(FStringView Path, bool bReadOnly);

    // Carries the source mode across, which on POSIX is what keeps a generated .sh executable.
    RUNTIME_API bool CopyPermissions(FStringView From, FStringView To);

    NODISCARD RUNTIME_API FString GetWorkingDirectory();
    RUNTIME_API bool SetWorkingDirectory(FStringView Path);
    NODISCARD RUNTIME_API FString GetTempDirectory();
    NODISCARD RUNTIME_API FString MakeAbsolute(FStringView Path);

    RUNTIME_API bool ReadFile(TVector<uint8>& OutData, FStringView Path);
    RUNTIME_API bool ReadFile(TVector64<uint8>& OutData, FStringView Path);
    RUNTIME_API bool ReadFile(FString& OutText, FStringView Path);
    RUNTIME_API bool ReadFileRange(TVector<uint8>& OutData, FStringView Path, int64 Offset, int64 Size);
    RUNTIME_API bool WriteFile(FStringView Path, TSpan64<const uint8> Data);
    RUNTIME_API bool AppendFile(FStringView Path, TSpan64<const uint8> Data);

    // Writes to a sibling temp file and renames over the destination, so a crash never leaves a torn file.
    RUNTIME_API bool AtomicWriteFile(FStringView Path, TSpan64<const uint8> Data);

    // Streams the middle section straight from SrcPath in fixed windows rather than buffering it.
    RUNTIME_API bool AtomicWriteFileSpliced(FStringView Path, TSpan<const uint8> Prefix,
                                            FStringView SrcPath, int64 SrcOffset, int64 SrcSize,
                                            TSpan<const uint8> Suffix);

    // Entry paths are Path verbatim plus '/'-joined names, so pass a normalized root to get one back.
    RUNTIME_API bool IterateDirectoryRaw(FStringView Path, bool bRecursive, FDirectoryVisitor Visitor, void* Context);

    namespace Detail
    {
        template <typename TCallable>
        EVisit Trampoline(void* Context, const FDirectoryEntry& Entry)
        {
            TCallable& Callable = *static_cast<TCallable*>(Context);

            if constexpr (std::is_same_v<decltype(Callable(Entry)), EVisit>)
            {
                return Callable(Entry);
            }
            else if constexpr (std::is_same_v<decltype(Callable(Entry)), bool>)
            {
                return Callable(Entry) ? EVisit::Continue : EVisit::Stop;
            }
            else
            {
                Callable(Entry);
                return EVisit::Continue;
            }
        }
    }

    template <typename TCallable>
    bool IterateDirectory(FStringView Path, TCallable&& Callable)
    {
        using CallableType = std::remove_reference_t<TCallable>;
        return IterateDirectoryRaw(Path, false, &Detail::Trampoline<CallableType>, (void*)&Callable);
    }

    template <typename TCallable>
    bool IterateDirectoryRecursive(FStringView Path, TCallable&& Callable)
    {
        using CallableType = std::remove_reference_t<TCallable>;
        return IterateDirectoryRaw(Path, true, &Detail::Trampoline<CallableType>, (void*)&Callable);
    }
}
