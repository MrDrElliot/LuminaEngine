#include "RuntimePCH.h"
#ifdef _WIN32

#include "PlatformFilesystem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Lumina::Filesystem
{
    namespace
    {
        thread_local EResult GLastResult = EResult::Success;

        constexpr int64 kUnixEpochIn100ns   = 116444736000000000LL;
        constexpr uint32 kMaxIoChunk        = 32u * 1024u * 1024u;
        constexpr int32 kLongPathThreshold  = 248;

        EResult TranslateError(DWORD Error)
        {
            switch (Error)
            {
            case ERROR_SUCCESS:              return EResult::Success;
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
            case ERROR_INVALID_DRIVE:        return EResult::NotFound;
            case ERROR_ACCESS_DENIED:
            case ERROR_SHARING_VIOLATION:
            case ERROR_LOCK_VIOLATION:       return EResult::AccessDenied;
            case ERROR_FILE_EXISTS:
            case ERROR_ALREADY_EXISTS:       return EResult::AlreadyExists;
            case ERROR_DIR_NOT_EMPTY:        return EResult::NotEmpty;
            case ERROR_INVALID_NAME:
            case ERROR_BAD_PATHNAME:
            case ERROR_FILENAME_EXCED_RANGE: return EResult::InvalidPath;
            case ERROR_DISK_FULL:
            case ERROR_HANDLE_DISK_FULL:     return EResult::OutOfSpace;
            case ERROR_NOT_SUPPORTED:        return EResult::Unsupported;
            default:                         return EResult::Unknown;
            }
        }

        EResult RecordLastError()
        {
            GLastResult = TranslateError(::GetLastError());
            return GLastResult;
        }

        void RecordSuccess()
        {
            GLastResult = EResult::Success;
        }

        int64 FileTimeToUnixNanos(const FILETIME& FileTime)
        {
            ULARGE_INTEGER Value;
            Value.LowPart  = FileTime.dwLowDateTime;
            Value.HighPart = FileTime.dwHighDateTime;

            if (Value.QuadPart == 0)
            {
                return 0;
            }

            return (static_cast<int64>(Value.QuadPart) - kUnixEpochIn100ns) * 100;
        }

        EAttributes TranslateAttributes(DWORD Attributes, FStringView Name)
        {
            EAttributes Result = EAttributes::None;

            if (Attributes & FILE_ATTRIBUTE_DIRECTORY)       { Result |= EAttributes::Directory; }
            if (Attributes & FILE_ATTRIBUTE_REPARSE_POINT)   { Result |= EAttributes::Symlink; }
            if (Attributes & FILE_ATTRIBUTE_HIDDEN)          { Result |= EAttributes::Hidden; }
            if (Attributes & FILE_ATTRIBUTE_READONLY)        { Result |= EAttributes::ReadOnly; }
            if (Attributes & FILE_ATTRIBUTE_SYSTEM)          { Result |= EAttributes::System; }

            if (!Name.empty() && Name.front() == '.')
            {
                Result |= EAttributes::Hidden;
            }

            return Result;
        }

        bool IsFullyQualified(const wchar_t* Path, int32 Length)
        {
            if (Length >= 2 && Path[1] == L':')
            {
                return Length >= 3 && (Path[2] == L'\\' || Path[2] == L'/');
            }

            return Length >= 2 && (Path[0] == L'\\' || Path[0] == L'/') && (Path[1] == L'\\' || Path[1] == L'/');
        }

        // The extended prefix also disables '.' and '..' collapsing, so it is only applied where legal.
        class FWidePath
        {
        public:

            FWidePath() = default;

            explicit FWidePath(FStringView Path)
            {
                Assign(Path);
            }

            ~FWidePath()
            {
                if (Data != Inline)
                {
                    void* Mem = Data;
                    Memory::Free(Mem);
                }
            }

            FWidePath(const FWidePath&) = delete;
            FWidePath& operator=(const FWidePath&) = delete;

            void Assign(FStringView Path)
            {
                if (Path.empty())
                {
                    Length = 0;
                    Start  = Data + kPrefixChars;
                    Start[0] = L'\0';
                    bValid = false;
                    return;
                }

                const int32 SourceLength = static_cast<int32>(Path.size());
                const int32 NeededChars  = Platform::GetConvertedLength_UTF8ToWide(Path.data(), SourceLength);

                Start = Data + kPrefixChars;
                Reserve(NeededChars + kPrefixChars + 1);

                wchar_t* Target = Data + kPrefixChars;
                const int32 Written = Platform::Convert_UTF8ToWide(Target, NeededChars, Path.data(), SourceLength);
                Target[Written] = L'\0';

                for (int32 Index = 0; Index < Written; ++Index)
                {
                    if (Target[Index] == L'/')
                    {
                        Target[Index] = L'\\';
                    }
                }

                const bool bAlreadyExtended = Written >= 4 && Target[0] == L'\\' && Target[1] == L'\\' && Target[2] == L'?';

                if (Written >= kLongPathThreshold && !bAlreadyExtended && IsFullyQualified(Target, Written))
                {
                    Data[0] = L'\\';
                    Data[1] = L'\\';
                    Data[2] = L'?';
                    Data[3] = L'\\';
                    Start   = Data;
                    Length  = Written + kPrefixChars;
                }
                else
                {
                    Start  = Target;
                    Length = Written;
                }

                bValid = true;
            }

            void Append(const wchar_t* Suffix, int32 SuffixLength)
            {
                const int32 Offset = static_cast<int32>(Start - Data);
                Reserve(Offset + Length + SuffixLength + 1);
                ::memcpy(Start + Length, Suffix, static_cast<size_t>(SuffixLength) * sizeof(wchar_t));
                Length += SuffixLength;
                Start[Length] = L'\0';
            }

            void Truncate(int32 NewLength)
            {
                Length = NewLength;
                Start[Length] = L'\0';
            }

            NODISCARD const wchar_t* Get() const    { return Start; }
            NODISCARD int32 Size() const            { return Length; }
            NODISCARD bool IsValid() const          { return bValid; }

        private:

            static constexpr int32 kPrefixChars  = 4;
            static constexpr int32 kInlineChars  = 512;

            void Reserve(int32 Chars)
            {
                if (Chars <= Capacity)
                {
                    return;
                }

                const int32 NewCapacity = Chars < Capacity * 2 ? Capacity * 2 : Chars;
                wchar_t* NewData = static_cast<wchar_t*>(Memory::Malloc(static_cast<size_t>(NewCapacity) * sizeof(wchar_t), alignof(wchar_t)));

                const int32 StartOffset = static_cast<int32>(Start - Data);
                ::memcpy(NewData, Data, static_cast<size_t>(Capacity) * sizeof(wchar_t));

                if (Data != Inline)
                {
                    void* Mem = Data;
                    Memory::Free(Mem);
                }

                Data     = NewData;
                Start    = NewData + StartOffset;
                Capacity = NewCapacity;
            }

            wchar_t  Inline[kInlineChars] = {};
            wchar_t* Data                 = Inline;
            wchar_t* Start                = Inline;
            int32    Capacity             = kInlineChars;
            int32    Length               = 0;
            bool     bValid               = false;
        };

        DWORD ToDesiredAccess(EAccess Access)
        {
            DWORD Result = 0;
            if (EnumHasAnyFlags(Access, EAccess::Read))  { Result |= GENERIC_READ; }
            if (EnumHasAnyFlags(Access, EAccess::Write)) { Result |= GENERIC_WRITE; }
            return Result;
        }

        DWORD ToShareMode(EShare Share)
        {
            DWORD Result = 0;
            if (EnumHasAnyFlags(Share, EShare::Read))   { Result |= FILE_SHARE_READ; }
            if (EnumHasAnyFlags(Share, EShare::Write))  { Result |= FILE_SHARE_WRITE; }
            if (EnumHasAnyFlags(Share, EShare::Delete)) { Result |= FILE_SHARE_DELETE; }
            return Result;
        }

        DWORD ToCreationDisposition(ECreateMode Mode)
        {
            switch (Mode)
            {
            case ECreateMode::OpenExisting: return OPEN_EXISTING;
            case ECreateMode::OpenAlways:   return OPEN_ALWAYS;
            case ECreateMode::CreateAlways: return CREATE_ALWAYS;
            case ECreateMode::CreateNew:    return CREATE_NEW;
            }
            return OPEN_EXISTING;
        }

        // Only POSIX-semantics rename can replace a target another handle still holds open.
        bool RenameByHandle(const wchar_t* From, const wchar_t* To, int32 ToLength)
        {
            const HANDLE Source = ::CreateFileW(From, DELETE | SYNCHRONIZE,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (Source == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            const size_t NameBytes = static_cast<size_t>(ToLength) * sizeof(wchar_t);
            const size_t InfoBytes = sizeof(FILE_RENAME_INFO) + NameBytes;

            FILE_RENAME_INFO* Info = static_cast<FILE_RENAME_INFO*>(Memory::Malloc(InfoBytes, alignof(FILE_RENAME_INFO)));
            ::memset(Info, 0, sizeof(FILE_RENAME_INFO));
            Info->Flags          = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
            Info->RootDirectory  = nullptr;
            Info->FileNameLength = static_cast<DWORD>(NameBytes);
            ::memcpy(Info->FileName, To, NameBytes);
            Info->FileName[ToLength] = L'\0';

            BOOL bRenamed = ::SetFileInformationByHandle(Source, FileRenameInfoEx, Info, static_cast<DWORD>(InfoBytes));

            if (!bRenamed)
            {
                Info->ReplaceIfExists = TRUE;
                bRenamed = ::SetFileInformationByHandle(Source, FileRenameInfo, Info, static_cast<DWORD>(InfoBytes));
            }

            void* Mem = Info;
            Memory::Free(Mem);
            ::CloseHandle(Source);

            return bRenamed != FALSE;
        }

        DWORD ToFlags(EHint Hint)
        {
            DWORD Result = FILE_ATTRIBUTE_NORMAL;
            if (EnumHasAnyFlags(Hint, EHint::Sequential))    { Result |= FILE_FLAG_SEQUENTIAL_SCAN; }
            if (EnumHasAnyFlags(Hint, EHint::Random))        { Result |= FILE_FLAG_RANDOM_ACCESS; }
            if (EnumHasAnyFlags(Hint, EHint::WriteThrough))  { Result |= FILE_FLAG_WRITE_THROUGH; }
            return Result;
        }
    }

    void FFileHandle::Close()
    {
        if (Handle != kInvalidHandle)
        {
            ::CloseHandle(reinterpret_cast<HANDLE>(Handle));
            Handle = kInvalidHandle;
        }
    }

    uint64 FFileHandle::Size() const
    {
        LARGE_INTEGER Result;
        if (Handle == kInvalidHandle || !::GetFileSizeEx(reinterpret_cast<HANDLE>(Handle), &Result))
        {
            return 0;
        }

        return static_cast<uint64>(Result.QuadPart);
    }

    uint64 FFileHandle::Tell() const
    {
        LARGE_INTEGER Zero;
        Zero.QuadPart = 0;

        LARGE_INTEGER Result;
        if (Handle == kInvalidHandle || !::SetFilePointerEx(reinterpret_cast<HANDLE>(Handle), Zero, &Result, FILE_CURRENT))
        {
            return 0;
        }

        return static_cast<uint64>(Result.QuadPart);
    }

    bool FFileHandle::Seek(int64 Offset, ESeek Origin)
    {
        if (Handle == kInvalidHandle)
        {
            return false;
        }

        DWORD Method = FILE_BEGIN;
        switch (Origin)
        {
        case ESeek::Begin:   Method = FILE_BEGIN;   break;
        case ESeek::Current: Method = FILE_CURRENT; break;
        case ESeek::End:     Method = FILE_END;     break;
        }

        LARGE_INTEGER Distance;
        Distance.QuadPart = Offset;
        return ::SetFilePointerEx(reinterpret_cast<HANDLE>(Handle), Distance, nullptr, Method) != FALSE;
    }

    uint64 FFileHandle::Read(void* Dest, uint64 Bytes)
    {
        if (Handle == kInvalidHandle || Dest == nullptr)
        {
            return 0;
        }

        uint8* Cursor    = static_cast<uint8*>(Dest);
        uint64 Remaining = Bytes;

        while (Remaining > 0)
        {
            const DWORD Want = static_cast<DWORD>(Remaining < kMaxIoChunk ? Remaining : kMaxIoChunk);

            DWORD Got = 0;
            if (!::ReadFile(reinterpret_cast<HANDLE>(Handle), Cursor, Want, &Got, nullptr))
            {
                RecordLastError();
                break;
            }

            if (Got == 0)
            {
                break;
            }

            Cursor    += Got;
            Remaining -= Got;
        }

        return Bytes - Remaining;
    }

    uint64 FFileHandle::Write(const void* Src, uint64 Bytes)
    {
        if (Handle == kInvalidHandle || Src == nullptr)
        {
            return 0;
        }

        const uint8* Cursor = static_cast<const uint8*>(Src);
        uint64 Remaining    = Bytes;

        while (Remaining > 0)
        {
            const DWORD Want = static_cast<DWORD>(Remaining < kMaxIoChunk ? Remaining : kMaxIoChunk);

            DWORD Put = 0;
            if (!::WriteFile(reinterpret_cast<HANDLE>(Handle), Cursor, Want, &Put, nullptr) || Put == 0)
            {
                RecordLastError();
                break;
            }

            Cursor    += Put;
            Remaining -= Put;
        }

        return Bytes - Remaining;
    }

    uint64 FFileHandle::ReadAt(void* Dest, uint64 Bytes, uint64 Offset) const
    {
        if (Handle == kInvalidHandle || Dest == nullptr)
        {
            return 0;
        }

        uint8* Cursor      = static_cast<uint8*>(Dest);
        uint64 Remaining   = Bytes;
        uint64 FilePointer = Offset;

        while (Remaining > 0)
        {
            const DWORD Want = static_cast<DWORD>(Remaining < kMaxIoChunk ? Remaining : kMaxIoChunk);

            OVERLAPPED Overlapped = {};
            Overlapped.Offset     = static_cast<DWORD>(FilePointer & 0xFFFFFFFFull);
            Overlapped.OffsetHigh = static_cast<DWORD>(FilePointer >> 32);

            DWORD Got = 0;
            if (!::ReadFile(reinterpret_cast<HANDLE>(Handle), Cursor, Want, &Got, &Overlapped))
            {
                if (::GetLastError() != ERROR_HANDLE_EOF)
                {
                    RecordLastError();
                }
                break;
            }

            if (Got == 0)
            {
                break;
            }

            Cursor      += Got;
            FilePointer += Got;
            Remaining   -= Got;
        }

        return Bytes - Remaining;
    }

    uint64 FFileHandle::WriteAt(const void* Src, uint64 Bytes, uint64 Offset) const
    {
        if (Handle == kInvalidHandle || Src == nullptr)
        {
            return 0;
        }

        const uint8* Cursor = static_cast<const uint8*>(Src);
        uint64 Remaining    = Bytes;
        uint64 FilePointer  = Offset;

        while (Remaining > 0)
        {
            const DWORD Want = static_cast<DWORD>(Remaining < kMaxIoChunk ? Remaining : kMaxIoChunk);

            OVERLAPPED Overlapped = {};
            Overlapped.Offset     = static_cast<DWORD>(FilePointer & 0xFFFFFFFFull);
            Overlapped.OffsetHigh = static_cast<DWORD>(FilePointer >> 32);

            DWORD Put = 0;
            if (!::WriteFile(reinterpret_cast<HANDLE>(Handle), Cursor, Want, &Put, &Overlapped) || Put == 0)
            {
                RecordLastError();
                break;
            }

            Cursor      += Put;
            FilePointer += Put;
            Remaining   -= Put;
        }

        return Bytes - Remaining;
    }

    bool FFileHandle::Flush()
    {
        return Handle != kInvalidHandle && ::FlushFileBuffers(reinterpret_cast<HANDLE>(Handle)) != FALSE;
    }

    bool FFileHandle::Truncate(uint64 NewSize)
    {
        if (!Seek(static_cast<int64>(NewSize), ESeek::Begin))
        {
            return false;
        }

        return ::SetEndOfFile(reinterpret_cast<HANDLE>(Handle)) != FALSE;
    }

    FMappedFile::FMappedFile(FMappedFile&& Other) noexcept
        : Base(Other.Base)
        , Length(Other.Length)
        , Mapping(Other.Mapping)
        , File(Other.File)
    {
        Other.Base    = nullptr;
        Other.Length  = 0;
        Other.Mapping = kInvalidHandle;
        Other.File    = kInvalidHandle;
    }

    FMappedFile& FMappedFile::operator=(FMappedFile&& Other) noexcept
    {
        if (this != &Other)
        {
            Close();
            Base    = Other.Base;
            Length  = Other.Length;
            Mapping = Other.Mapping;
            File    = Other.File;

            Other.Base    = nullptr;
            Other.Length  = 0;
            Other.Mapping = kInvalidHandle;
            Other.File    = kInvalidHandle;
        }
        return *this;
    }

    void FMappedFile::Close()
    {
        if (Base != nullptr)
        {
            ::UnmapViewOfFile(Base);
            Base = nullptr;
        }

        if (Mapping != kInvalidHandle)
        {
            ::CloseHandle(reinterpret_cast<HANDLE>(Mapping));
            Mapping = kInvalidHandle;
        }

        if (File != kInvalidHandle)
        {
            ::CloseHandle(reinterpret_cast<HANDLE>(File));
            File = kInvalidHandle;
        }

        Length = 0;
    }

    FMappedFile FMappedFile::OpenRead(FStringView Path)
    {
        FMappedFile Result;

        FFileHandle Handle = Open(Path, EAccess::Read, ECreateMode::OpenExisting, EShare::Read, EHint::Random);
        if (!Handle.IsValid())
        {
            return Result;
        }

        const uint64 FileLength = Handle.Size();
        if (FileLength == 0)
        {
            return Result;
        }

        const HANDLE Native  = reinterpret_cast<HANDLE>(Handle.GetNativeHandle());
        const HANDLE Mapping = ::CreateFileMappingW(Native, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (Mapping == nullptr)
        {
            RecordLastError();
            return Result;
        }

        const void* View = ::MapViewOfFile(Mapping, FILE_MAP_READ, 0, 0, 0);
        if (View == nullptr)
        {
            RecordLastError();
            ::CloseHandle(Mapping);
            return Result;
        }

        // The section object keeps the file alive, so the handle closes with Handle at scope exit.
        Result.Base    = static_cast<const uint8*>(View);
        Result.Length  = FileLength;
        Result.Mapping = reinterpret_cast<uintptr_t>(Mapping);

        RecordSuccess();
        return Result;
    }

    EResult GetLastResult()
    {
        return GLastResult;
    }

    FFileHandle Open(FStringView Path, EAccess Access, ECreateMode Mode, EShare Share, EHint Hint)
    {
        const FWidePath Wide(Path);
        if (!Wide.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return FFileHandle();
        }

        const HANDLE Result = ::CreateFileW(Wide.Get(), ToDesiredAccess(Access), ToShareMode(Share), nullptr,
                                            ToCreationDisposition(Mode), ToFlags(Hint), nullptr);

        if (Result == INVALID_HANDLE_VALUE)
        {
            RecordLastError();
            return FFileHandle();
        }

        RecordSuccess();
        return FFileHandle(reinterpret_cast<uintptr_t>(Result));
    }

    FFileStat Stat(FStringView Path)
    {
        FFileStat Result;

        const FWidePath Wide(Path);
        if (!Wide.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return Result;
        }

        WIN32_FILE_ATTRIBUTE_DATA Attributes;
        if (!::GetFileAttributesExW(Wide.Get(), GetFileExInfoStandard, &Attributes))
        {
            RecordLastError();
            return Result;
        }

        ULARGE_INTEGER Size;
        Size.LowPart  = Attributes.nFileSizeLow;
        Size.HighPart = Attributes.nFileSizeHigh;

        Result.Size           = Size.QuadPart;
        Result.LastModifyTime = FileTimeToUnixNanos(Attributes.ftLastWriteTime);
        Result.CreationTime   = FileTimeToUnixNanos(Attributes.ftCreationTime);
        Result.Attributes     = TranslateAttributes(Attributes.dwFileAttributes, {});
        Result.bValid         = true;

        RecordSuccess();
        return Result;
    }

    bool Exists(FStringView Path)
    {
        const FWidePath Wide(Path);
        if (!Wide.IsValid())
        {
            return false;
        }

        return ::GetFileAttributesW(Wide.Get()) != INVALID_FILE_ATTRIBUTES;
    }

    bool IsDirectory(FStringView Path)
    {
        const FWidePath Wide(Path);
        if (!Wide.IsValid())
        {
            return false;
        }

        const DWORD Attributes = ::GetFileAttributesW(Wide.Get());
        return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    bool IsFile(FStringView Path)
    {
        const FWidePath Wide(Path);
        if (!Wide.IsValid())
        {
            return false;
        }

        const DWORD Attributes = ::GetFileAttributesW(Wide.Get());
        return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    uint64 FileSize(FStringView Path)
    {
        const FFileStat Info = Stat(Path);
        return Info.bValid ? Info.Size : 0;
    }

    int64 LastWriteTime(FStringView Path)
    {
        const FFileStat Info = Stat(Path);
        return Info.bValid ? Info.LastModifyTime : 0;
    }

    bool MakeDirectory(FStringView Path)
    {
        const FWidePath Wide(Path);
        if (!Wide.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        if (::CreateDirectoryW(Wide.Get(), nullptr))
        {
            RecordSuccess();
            return true;
        }

        return RecordLastError() == EResult::AlreadyExists;
    }

    bool RemoveFile(FStringView Path)
    {
        const FWidePath Wide(Path);
        if (!Wide.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        if (::DeleteFileW(Wide.Get()))
        {
            RecordSuccess();
            return true;
        }

        if (::GetLastError() == ERROR_ACCESS_DENIED)
        {
            const DWORD Attributes = ::GetFileAttributesW(Wide.Get());
            if (Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_READONLY))
            {
                ::SetFileAttributesW(Wide.Get(), Attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
                if (::DeleteFileW(Wide.Get()))
                {
                    RecordSuccess();
                    return true;
                }
            }
        }

        return RecordLastError() == EResult::NotFound;
    }

    bool DeleteDirectory(FStringView Path)
    {
        const FWidePath Wide(Path);
        if (!Wide.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        if (::RemoveDirectoryW(Wide.Get()))
        {
            RecordSuccess();
            return true;
        }

        return RecordLastError() == EResult::NotFound;
    }

    bool Move(FStringView From, FStringView To, bool bReplaceExisting)
    {
        const FWidePath WideFrom(From);
        const FWidePath WideTo(To);

        if (!WideFrom.IsValid() || !WideTo.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        DWORD Flags = MOVEFILE_COPY_ALLOWED;
        if (bReplaceExisting)
        {
            Flags |= MOVEFILE_REPLACE_EXISTING;
        }

        if (::MoveFileExW(WideFrom.Get(), WideTo.Get(), Flags))
        {
            RecordSuccess();
            return true;
        }

        if (bReplaceExisting && RenameByHandle(WideFrom.Get(), WideTo.Get(), WideTo.Size()))
        {
            RecordSuccess();
            return true;
        }

        RecordLastError();
        return false;
    }

    bool Copy(FStringView From, FStringView To, bool bReplaceExisting)
    {
        const FWidePath WideFrom(From);
        const FWidePath WideTo(To);

        if (!WideFrom.IsValid() || !WideTo.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        if (::CopyFileW(WideFrom.Get(), WideTo.Get(), bReplaceExisting ? FALSE : TRUE))
        {
            RecordSuccess();
            return true;
        }

        RecordLastError();
        return false;
    }

    bool SetReadOnly(FStringView Path, bool bReadOnly)
    {
        const FWidePath Wide(Path);
        if (!Wide.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        const DWORD Attributes = ::GetFileAttributesW(Wide.Get());
        if (Attributes == INVALID_FILE_ATTRIBUTES)
        {
            RecordLastError();
            return false;
        }

        const DWORD Updated = bReadOnly
            ? (Attributes | FILE_ATTRIBUTE_READONLY)
            : (Attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));

        if (Updated == Attributes)
        {
            RecordSuccess();
            return true;
        }

        if (::SetFileAttributesW(Wide.Get(), Updated))
        {
            RecordSuccess();
            return true;
        }

        RecordLastError();
        return false;
    }

    bool CopyPermissions(FStringView From, FStringView To)
    {
        const FFileStat Source = Stat(From);
        return Source.bValid && SetReadOnly(To, Source.IsReadOnly());
    }

    FString GetWorkingDirectory()
    {
        wchar_t Buffer[1024];
        const DWORD Length = ::GetCurrentDirectoryW(static_cast<DWORD>(std::size(Buffer)), Buffer);
        if (Length == 0 || Length >= std::size(Buffer))
        {
            RecordLastError();
            return {};
        }

        const auto Narrow = StringCast<ANSICHAR>(Buffer, static_cast<int32>(Length));
        return FString(Narrow.Get(), Narrow.Length());
    }

    bool SetWorkingDirectory(FStringView Path)
    {
        const FWidePath Wide(Path);
        if (!Wide.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        if (::SetCurrentDirectoryW(Wide.Get()))
        {
            RecordSuccess();
            return true;
        }

        RecordLastError();
        return false;
    }

    FString GetTempDirectory()
    {
        wchar_t Buffer[MAX_PATH + 1];
        const DWORD Length = ::GetTempPathW(static_cast<DWORD>(std::size(Buffer)), Buffer);
        if (Length == 0)
        {
            RecordLastError();
            return {};
        }

        const auto Narrow = StringCast<ANSICHAR>(Buffer, static_cast<int32>(Length));
        return FString(Narrow.Get(), Narrow.Length());
    }

    FString MakeAbsolute(FStringView Path)
    {
        const FWidePath Wide(Path);
        if (!Wide.IsValid())
        {
            return {};
        }

        wchar_t Buffer[1024];
        const DWORD Length = ::GetFullPathNameW(Wide.Get(), static_cast<DWORD>(std::size(Buffer)), Buffer, nullptr);
        if (Length == 0 || Length >= std::size(Buffer))
        {
            RecordLastError();
            return {};
        }

        const auto Narrow = StringCast<ANSICHAR>(Buffer, static_cast<int32>(Length));
        FString Result(Narrow.Get(), Narrow.Length());

        for (char& Character : Result)
        {
            if (Character == '\\')
            {
                Character = '/';
            }
        }

        return Result;
    }

    bool IsDirectoryEmpty(FStringView Path)
    {
        FWidePath Pattern(Path);
        if (!Pattern.IsValid())
        {
            return false;
        }

        Pattern.Append(L"\\*", 2);

        WIN32_FIND_DATAW FindData;
        const HANDLE Search = ::FindFirstFileExW(Pattern.Get(), FindExInfoBasic, &FindData,
                                                 FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (Search == INVALID_HANDLE_VALUE)
        {
            RecordLastError();
            return false;
        }

        bool bEmpty = true;
        do
        {
            const wchar_t* Name = FindData.cFileName;
            const bool bDotEntry = Name[0] == L'.' && (Name[1] == L'\0' || (Name[1] == L'.' && Name[2] == L'\0'));
            if (!bDotEntry)
            {
                bEmpty = false;
                break;
            }
        }
        while (::FindNextFileW(Search, &FindData));

        ::FindClose(Search);
        RecordSuccess();
        return bEmpty;
    }

    namespace
    {
        struct FIterationState
        {
            FWidePath           Wide;
            FString             Utf8;
            FDirectoryVisitor   Visitor     = nullptr;
            void*               Context     = nullptr;
            bool                bRecursive  = false;
        };

        EVisit IterateLevel(FIterationState& State, uint32 Depth)
        {
            const int32 WideBase  = State.Wide.Size();
            const size_t Utf8Base = State.Utf8.size();

            State.Wide.Append(L"\\*", 2);

            WIN32_FIND_DATAW FindData;
            const HANDLE Search = ::FindFirstFileExW(State.Wide.Get(), FindExInfoBasic, &FindData,
                                                     FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);

            State.Wide.Truncate(WideBase);

            if (Search == INVALID_HANDLE_VALUE)
            {
                RecordLastError();
                return EVisit::Continue;
            }

            EVisit Outcome = EVisit::Continue;

            do
            {
                const wchar_t* Name = FindData.cFileName;
                if (Name[0] == L'.' && (Name[1] == L'\0' || (Name[1] == L'.' && Name[2] == L'\0')))
                {
                    continue;
                }

                const int32 NameLength = static_cast<int32>(::wcslen(Name));

                State.Wide.Append(L"\\", 1);
                State.Wide.Append(Name, NameLength);

                State.Utf8.push_back('/');

                const size_t NameOffset  = State.Utf8.size();
                const int32 NarrowLength = Platform::GetConvertedLength_WideToUTF8(Name, NameLength);
                State.Utf8.resize(NameOffset + static_cast<size_t>(NarrowLength));
                Platform::Convert_WideToUTF8(State.Utf8.data() + NameOffset, NarrowLength, Name, NameLength);

                ULARGE_INTEGER EntrySize;
                EntrySize.LowPart  = FindData.nFileSizeLow;
                EntrySize.HighPart = FindData.nFileSizeHigh;

                FDirectoryEntry Entry;
                Entry.FullPath       = FStringView(State.Utf8.data(), State.Utf8.size());
                Entry.Name           = Entry.FullPath.substr(NameOffset);
                Entry.Size           = EntrySize.QuadPart;
                Entry.LastModifyTime = FileTimeToUnixNanos(FindData.ftLastWriteTime);
                Entry.Attributes     = TranslateAttributes(FindData.dwFileAttributes, Entry.Name);
                Entry.Depth          = Depth;

                const EVisit Decision = State.Visitor(State.Context, Entry);

                const bool bDescend = State.bRecursive
                    && Decision == EVisit::Continue
                    && (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                    && (FindData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;

                const bool bStop = Decision == EVisit::Stop
                    || (bDescend && IterateLevel(State, Depth + 1) == EVisit::Stop);

                State.Wide.Truncate(WideBase);
                State.Utf8.resize(Utf8Base);

                if (bStop)
                {
                    Outcome = EVisit::Stop;
                    break;
                }
            }
            while (::FindNextFileW(Search, &FindData));

            ::FindClose(Search);
            return Outcome;
        }
    }

    bool IterateDirectoryRaw(FStringView Path, bool bRecursive, FDirectoryVisitor Visitor, void* Context)
    {
        if (Visitor == nullptr || Path.empty())
        {
            return false;
        }

        FStringView Trimmed = Path;
        while (Trimmed.size() > 1 && (Trimmed.back() == '/' || Trimmed.back() == '\\'))
        {
            Trimmed.remove_suffix(1);
        }

        if (!IsDirectory(Trimmed))
        {
            GLastResult = EResult::NotFound;
            return false;
        }

        FIterationState State;
        State.Wide.Assign(Trimmed);
        if (!State.Wide.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        State.Utf8.reserve(1024);
        State.Utf8.assign(Trimmed.data(), Trimmed.size());
        State.Visitor    = Visitor;
        State.Context    = Context;
        State.bRecursive = bRecursive;

        RecordSuccess();
        IterateLevel(State, 0);
        return true;
    }
}

#endif
