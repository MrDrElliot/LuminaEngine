#include "RuntimePCH.h"
#ifndef _WIN32

#include "PlatformFilesystem.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace Lumina::Filesystem
{
    namespace
    {
        thread_local EResult GLastResult = EResult::Success;

        constexpr size_t kMaxIoChunk = 32ull * 1024ull * 1024ull;

        EResult TranslateError(int Error)
        {
            switch (Error)
            {
            case 0:             return EResult::Success;
            case ENOENT:
            case ENOTDIR:       return EResult::NotFound;
            case EACCES:
            case EPERM:
            case EROFS:         return EResult::AccessDenied;
            case EEXIST:        return EResult::AlreadyExists;
            case ENOTEMPTY:     return EResult::NotEmpty;
            case ENAMETOOLONG:
            case EINVAL:        return EResult::InvalidPath;
            case ENOSPC:
            case EDQUOT:        return EResult::OutOfSpace;
            case ENOSYS:
            case EOPNOTSUPP:    return EResult::Unsupported;
            default:            return EResult::Unknown;
            }
        }

        EResult RecordLastError()
        {
            GLastResult = TranslateError(errno);
            return GLastResult;
        }

        void RecordSuccess()
        {
            GLastResult = EResult::Success;
        }

        int64 TimeSpecToUnixNanos(const timespec& Time)
        {
            return static_cast<int64>(Time.tv_sec) * 1000000000ll + static_cast<int64>(Time.tv_nsec);
        }

        int ToFileDescriptor(uintptr_t Handle)
        {
            return static_cast<int>(static_cast<intptr_t>(Handle));
        }

        uintptr_t ToHandle(int FileDescriptor)
        {
            return static_cast<uintptr_t>(static_cast<intptr_t>(FileDescriptor));
        }

        EAttributes TranslateMode(mode_t Mode, FStringView Name)
        {
            EAttributes Result = EAttributes::None;

            if (S_ISDIR(Mode))  { Result |= EAttributes::Directory; }
            if (S_ISLNK(Mode))  { Result |= EAttributes::Symlink; }

            if ((Mode & S_IWUSR) == 0)
            {
                Result |= EAttributes::ReadOnly;
            }

            if (!Name.empty() && Name.front() == '.')
            {
                Result |= EAttributes::Hidden;
            }

            return Result;
        }

        // POSIX needs a null-terminated path, and FStringView carries no guarantee of one.
        class FNativePath
        {
        public:

            explicit FNativePath(FStringView Path)
            {
                const size_t Length = Path.size();

                if (Length == 0)
                {
                    Inline[0] = '\0';
                    return;
                }

                if (Length + 1 > kInlineChars)
                {
                    Data = static_cast<char*>(Memory::Malloc(Length + 1, alignof(char)));
                }

                ::memcpy(Data, Path.data(), Length);
                Data[Length] = '\0';
                bValid = true;
            }

            ~FNativePath()
            {
                if (Data != Inline)
                {
                    void* Mem = Data;
                    Memory::Free(Mem);
                }
            }

            FNativePath(const FNativePath&) = delete;
            FNativePath& operator=(const FNativePath&) = delete;

            NODISCARD const char* Get() const   { return Data; }
            NODISCARD bool IsValid() const      { return bValid; }

        private:

            static constexpr size_t kInlineChars = 512;

            char  Inline[kInlineChars] = {};
            char* Data                 = Inline;
            bool  bValid               = false;
        };

        int ToOpenFlags(EAccess Access, ECreateMode Mode)
        {
            int Flags = 0;

            const bool bRead  = EnumHasAnyFlags(Access, EAccess::Read);
            const bool bWrite = EnumHasAnyFlags(Access, EAccess::Write);

            if (bRead && bWrite)    { Flags |= O_RDWR; }
            else if (bWrite)        { Flags |= O_WRONLY; }
            else                    { Flags |= O_RDONLY; }

            switch (Mode)
            {
            case ECreateMode::OpenExisting:                             break;
            case ECreateMode::OpenAlways:   Flags |= O_CREAT;           break;
            case ECreateMode::CreateAlways: Flags |= O_CREAT | O_TRUNC; break;
            case ECreateMode::CreateNew:    Flags |= O_CREAT | O_EXCL;  break;
            }

            return Flags;
        }
    }

    void FFileHandle::Close()
    {
        if (Handle != kInvalidHandle)
        {
            ::close(ToFileDescriptor(Handle));
            Handle = kInvalidHandle;
        }
    }

    uint64 FFileHandle::Size() const
    {
        struct stat Info;
        if (Handle == kInvalidHandle || ::fstat(ToFileDescriptor(Handle), &Info) != 0)
        {
            return 0;
        }

        return static_cast<uint64>(Info.st_size);
    }

    uint64 FFileHandle::Tell() const
    {
        if (Handle == kInvalidHandle)
        {
            return 0;
        }

        const off_t Position = ::lseek(ToFileDescriptor(Handle), 0, SEEK_CUR);
        return Position < 0 ? 0 : static_cast<uint64>(Position);
    }

    bool FFileHandle::Seek(int64 Offset, ESeek Origin)
    {
        if (Handle == kInvalidHandle)
        {
            return false;
        }

        int Whence = SEEK_SET;
        switch (Origin)
        {
        case ESeek::Begin:   Whence = SEEK_SET; break;
        case ESeek::Current: Whence = SEEK_CUR; break;
        case ESeek::End:     Whence = SEEK_END; break;
        }

        return ::lseek(ToFileDescriptor(Handle), static_cast<off_t>(Offset), Whence) >= 0;
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
            const size_t Want = static_cast<size_t>(Remaining < kMaxIoChunk ? Remaining : kMaxIoChunk);
            const ssize_t Got = ::read(ToFileDescriptor(Handle), Cursor, Want);

            if (Got < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                RecordLastError();
                break;
            }

            if (Got == 0)
            {
                break;
            }

            Cursor    += Got;
            Remaining -= static_cast<uint64>(Got);
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
            const size_t Want = static_cast<size_t>(Remaining < kMaxIoChunk ? Remaining : kMaxIoChunk);
            const ssize_t Put = ::write(ToFileDescriptor(Handle), Cursor, Want);

            if (Put < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                RecordLastError();
                break;
            }

            if (Put == 0)
            {
                break;
            }

            Cursor    += Put;
            Remaining -= static_cast<uint64>(Put);
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
            const size_t Want = static_cast<size_t>(Remaining < kMaxIoChunk ? Remaining : kMaxIoChunk);
            const ssize_t Got = ::pread(ToFileDescriptor(Handle), Cursor, Want, static_cast<off_t>(FilePointer));

            if (Got < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                RecordLastError();
                break;
            }

            if (Got == 0)
            {
                break;
            }

            Cursor      += Got;
            FilePointer += static_cast<uint64>(Got);
            Remaining   -= static_cast<uint64>(Got);
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
            const size_t Want = static_cast<size_t>(Remaining < kMaxIoChunk ? Remaining : kMaxIoChunk);
            const ssize_t Put = ::pwrite(ToFileDescriptor(Handle), Cursor, Want, static_cast<off_t>(FilePointer));

            if (Put < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                RecordLastError();
                break;
            }

            if (Put == 0)
            {
                break;
            }

            Cursor      += Put;
            FilePointer += static_cast<uint64>(Put);
            Remaining   -= static_cast<uint64>(Put);
        }

        return Bytes - Remaining;
    }

    bool FFileHandle::Flush()
    {
        return Handle != kInvalidHandle && ::fsync(ToFileDescriptor(Handle)) == 0;
    }

    bool FFileHandle::Truncate(uint64 NewSize)
    {
        return Handle != kInvalidHandle && ::ftruncate(ToFileDescriptor(Handle), static_cast<off_t>(NewSize)) == 0;
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
            ::munmap(const_cast<uint8*>(Base), static_cast<size_t>(Length));
            Base = nullptr;
        }

        Mapping = kInvalidHandle;
        File    = kInvalidHandle;
        Length  = 0;
    }

    FMappedFile FMappedFile::OpenRead(FStringView Path)
    {
        FMappedFile Result;

        FFileHandle Handle = Open(Path, EAccess::Read, ECreateMode::OpenExisting, EShare::All, EHint::Random);
        if (!Handle.IsValid())
        {
            return Result;
        }

        const uint64 FileLength = Handle.Size();
        if (FileLength == 0)
        {
            return Result;
        }

        void* View = ::mmap(nullptr, static_cast<size_t>(FileLength), PROT_READ, MAP_PRIVATE,
                            ToFileDescriptor(Handle.GetNativeHandle()), 0);

        if (View == MAP_FAILED)
        {
            RecordLastError();
            return Result;
        }

        Result.Base   = static_cast<const uint8*>(View);
        Result.Length = FileLength;

        RecordSuccess();
        return Result;
    }

    EResult GetLastResult()
    {
        return GLastResult;
    }

    FFileHandle Open(FStringView Path, EAccess Access, ECreateMode Mode, EShare Share, EHint Hint)
    {
        const FNativePath Native(Path);
        if (!Native.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return FFileHandle();
        }

        const int FileDescriptor = ::open(Native.Get(), ToOpenFlags(Access, Mode), 0666);
        if (FileDescriptor < 0)
        {
            RecordLastError();
            return FFileHandle();
        }

#if defined(POSIX_FADV_SEQUENTIAL)
        if (EnumHasAnyFlags(Hint, EHint::Sequential))
        {
            ::posix_fadvise(FileDescriptor, 0, 0, POSIX_FADV_SEQUENTIAL);
        }
        else if (EnumHasAnyFlags(Hint, EHint::Random))
        {
            ::posix_fadvise(FileDescriptor, 0, 0, POSIX_FADV_RANDOM);
        }
#endif

        RecordSuccess();
        return FFileHandle(ToHandle(FileDescriptor));
    }

    FFileStat Stat(FStringView Path)
    {
        FFileStat Result;

        const FNativePath Native(Path);
        if (!Native.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return Result;
        }

        struct stat Info;
        if (::stat(Native.Get(), &Info) != 0)
        {
            RecordLastError();
            return Result;
        }

        Result.Size           = static_cast<uint64>(Info.st_size);
        Result.LastModifyTime = TimeSpecToUnixNanos(Info.st_mtim);
        Result.CreationTime   = TimeSpecToUnixNanos(Info.st_ctim);
        Result.Attributes     = TranslateMode(Info.st_mode, {});
        Result.bValid         = true;

        RecordSuccess();
        return Result;
    }

    bool Exists(FStringView Path)
    {
        const FNativePath Native(Path);
        return Native.IsValid() && ::access(Native.Get(), F_OK) == 0;
    }

    bool IsDirectory(FStringView Path)
    {
        const FNativePath Native(Path);
        if (!Native.IsValid())
        {
            return false;
        }

        struct stat Info;
        return ::stat(Native.Get(), &Info) == 0 && S_ISDIR(Info.st_mode);
    }

    bool IsFile(FStringView Path)
    {
        const FNativePath Native(Path);
        if (!Native.IsValid())
        {
            return false;
        }

        struct stat Info;
        return ::stat(Native.Get(), &Info) == 0 && S_ISREG(Info.st_mode);
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
        const FNativePath Native(Path);
        if (!Native.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        if (::mkdir(Native.Get(), 0777) == 0)
        {
            RecordSuccess();
            return true;
        }

        return RecordLastError() == EResult::AlreadyExists;
    }

    bool RemoveFile(FStringView Path)
    {
        const FNativePath Native(Path);
        if (!Native.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        if (::unlink(Native.Get()) == 0)
        {
            RecordSuccess();
            return true;
        }

        return RecordLastError() == EResult::NotFound;
    }

    bool DeleteDirectory(FStringView Path)
    {
        const FNativePath Native(Path);
        if (!Native.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        if (::rmdir(Native.Get()) == 0)
        {
            RecordSuccess();
            return true;
        }

        return RecordLastError() == EResult::NotFound;
    }

    bool Move(FStringView From, FStringView To, bool bReplaceExisting)
    {
        const FNativePath NativeFrom(From);
        const FNativePath NativeTo(To);

        if (!NativeFrom.IsValid() || !NativeTo.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        if (!bReplaceExisting && ::access(NativeTo.Get(), F_OK) == 0)
        {
            GLastResult = EResult::AlreadyExists;
            return false;
        }

        if (::rename(NativeFrom.Get(), NativeTo.Get()) == 0)
        {
            RecordSuccess();
            return true;
        }

        RecordLastError();
        return false;
    }

    bool Copy(FStringView From, FStringView To, bool bReplaceExisting)
    {
        FFileHandle Source = Open(From, EAccess::Read, ECreateMode::OpenExisting, EShare::All, EHint::Sequential);
        if (!Source.IsValid())
        {
            return false;
        }

        const ECreateMode Mode = bReplaceExisting ? ECreateMode::CreateAlways : ECreateMode::CreateNew;
        FFileHandle Target = Open(To, EAccess::Write, Mode, EShare::All, EHint::Sequential);
        if (!Target.IsValid())
        {
            return false;
        }

        constexpr size_t kCopyChunk = 1ull << 20;
        TVector<uint8> Buffer;
        Buffer.resize(kCopyChunk);

        for (;;)
        {
            const uint64 Got = Source.Read(Buffer.data(), kCopyChunk);
            if (Got == 0)
            {
                break;
            }

            if (Target.Write(Buffer.data(), Got) != Got)
            {
                return false;
            }
        }

        RecordSuccess();
        return true;
    }

    bool SetReadOnly(FStringView Path, bool bReadOnly)
    {
        const FNativePath Native(Path);
        if (!Native.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        struct stat Info;
        if (::stat(Native.Get(), &Info) != 0)
        {
            RecordLastError();
            return false;
        }

        const mode_t WriteBits = S_IWUSR | S_IWGRP | S_IWOTH;
        const mode_t Updated   = bReadOnly ? (Info.st_mode & ~WriteBits) : (Info.st_mode | S_IWUSR);

        if (::chmod(Native.Get(), Updated) != 0)
        {
            RecordLastError();
            return false;
        }

        RecordSuccess();
        return true;
    }

    bool CopyPermissions(FStringView From, FStringView To)
    {
        const FNativePath NativeFrom(From);
        const FNativePath NativeTo(To);

        if (!NativeFrom.IsValid() || !NativeTo.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        struct stat Info;
        if (::stat(NativeFrom.Get(), &Info) != 0 || ::chmod(NativeTo.Get(), Info.st_mode & 07777) != 0)
        {
            RecordLastError();
            return false;
        }

        RecordSuccess();
        return true;
    }

    FString GetWorkingDirectory()
    {
        char Buffer[PATH_MAX];
        if (::getcwd(Buffer, sizeof(Buffer)) == nullptr)
        {
            RecordLastError();
            return {};
        }

        return FString(Buffer);
    }

    bool SetWorkingDirectory(FStringView Path)
    {
        const FNativePath Native(Path);
        if (!Native.IsValid())
        {
            GLastResult = EResult::InvalidPath;
            return false;
        }

        if (::chdir(Native.Get()) == 0)
        {
            RecordSuccess();
            return true;
        }

        RecordLastError();
        return false;
    }

    FString GetTempDirectory()
    {
        if (const char* Override = ::getenv("TMPDIR"))
        {
            return FString(Override);
        }

        return FString("/tmp");
    }

    FString MakeAbsolute(FStringView Path)
    {
        const FNativePath Native(Path);
        if (!Native.IsValid())
        {
            return {};
        }

        char Buffer[PATH_MAX];
        if (::realpath(Native.Get(), Buffer) == nullptr)
        {
            RecordLastError();
            return {};
        }

        return FString(Buffer);
    }

    bool IsDirectoryEmpty(FStringView Path)
    {
        const FNativePath Native(Path);
        if (!Native.IsValid())
        {
            return false;
        }

        DIR* Directory = ::opendir(Native.Get());
        if (Directory == nullptr)
        {
            RecordLastError();
            return false;
        }

        bool bEmpty = true;
        while (const dirent* Entry = ::readdir(Directory))
        {
            const char* Name = Entry->d_name;
            const bool bDotEntry = Name[0] == '.' && (Name[1] == '\0' || (Name[1] == '.' && Name[2] == '\0'));
            if (!bDotEntry)
            {
                bEmpty = false;
                break;
            }
        }

        ::closedir(Directory);
        RecordSuccess();
        return bEmpty;
    }

    namespace
    {
        struct FIterationState
        {
            FString             Utf8;
            FDirectoryVisitor   Visitor     = nullptr;
            void*               Context     = nullptr;
            bool                bRecursive  = false;
        };

        EVisit IterateLevel(FIterationState& State, uint32 Depth)
        {
            const size_t Base = State.Utf8.size();

            DIR* Directory = ::opendir(State.Utf8.c_str());
            if (Directory == nullptr)
            {
                RecordLastError();
                return EVisit::Continue;
            }

            const int DirectoryDescriptor = ::dirfd(Directory);
            EVisit Outcome = EVisit::Continue;

            while (const dirent* Found = ::readdir(Directory))
            {
                const char* Name = Found->d_name;
                if (Name[0] == '.' && (Name[1] == '\0' || (Name[1] == '.' && Name[2] == '\0')))
                {
                    continue;
                }

                const size_t NameLength = ::strlen(Name);

                State.Utf8.push_back('/');
                const size_t NameOffset = State.Utf8.size();
                State.Utf8.append(Name, NameLength);

                struct stat Info;
                const bool bStatted = ::fstatat(DirectoryDescriptor, Name, &Info, AT_SYMLINK_NOFOLLOW) == 0;

                FDirectoryEntry Entry;
                Entry.FullPath       = FStringView(State.Utf8.data(), State.Utf8.size());
                Entry.Name           = Entry.FullPath.substr(NameOffset);
                Entry.Size           = bStatted ? static_cast<uint64>(Info.st_size) : 0;
                Entry.LastModifyTime = bStatted ? TimeSpecToUnixNanos(Info.st_mtim) : 0;
                Entry.Attributes     = bStatted ? TranslateMode(Info.st_mode, Entry.Name) : EAttributes::None;
                Entry.Depth          = Depth;

                const EVisit Decision = State.Visitor(State.Context, Entry);

                const bool bDescend = State.bRecursive
                    && Decision == EVisit::Continue
                    && bStatted
                    && S_ISDIR(Info.st_mode);

                const bool bStop = Decision == EVisit::Stop
                    || (bDescend && IterateLevel(State, Depth + 1) == EVisit::Stop);

                State.Utf8.resize(Base);

                if (bStop)
                {
                    Outcome = EVisit::Stop;
                    break;
                }
            }

            ::closedir(Directory);
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
        while (Trimmed.size() > 1 && Trimmed.back() == '/')
        {
            Trimmed.remove_suffix(1);
        }

        if (!IsDirectory(Trimmed))
        {
            GLastResult = EResult::NotFound;
            return false;
        }

        FIterationState State;
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
