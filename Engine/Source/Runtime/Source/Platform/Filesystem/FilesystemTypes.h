#pragma once

#include "Containers/String.h"
#include "Core/LuminaMacros.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina::Filesystem
{
    inline constexpr uintptr_t kInvalidHandle = ~static_cast<uintptr_t>(0);

    enum class EAccess : uint8
    {
        Read        = BIT(0),
        Write       = BIT(1),
        ReadWrite   = Read | Write,
    };

    ENUM_CLASS_FLAGS(EAccess);

    // Delete sharing is what lets an atomic write-then-rename replace a file this process holds open.
    enum class EShare : uint8
    {
        None    = 0,
        Read    = BIT(0),
        Write   = BIT(1),
        Delete  = BIT(2),
        All     = Read | Write | Delete,
    };

    ENUM_CLASS_FLAGS(EShare);

    enum class ECreateMode : uint8
    {
        OpenExisting,
        OpenAlways,
        CreateAlways,
        CreateNew,
    };

    enum class EHint : uint8
    {
        None            = 0,
        Sequential      = BIT(0),
        Random          = BIT(1),
        WriteThrough    = BIT(2),
    };

    ENUM_CLASS_FLAGS(EHint);

    enum class ESeek : uint8
    {
        Begin,
        Current,
        End,
    };

    enum class EResult : uint8
    {
        Success,
        NotFound,
        AccessDenied,
        AlreadyExists,
        NotEmpty,
        InvalidPath,
        IoError,
        OutOfSpace,
        Unsupported,
        Unknown,
    };

    RUNTIME_API const char* ToString(EResult Result);

    enum class EAttributes : uint16
    {
        None        = 0,
        Directory   = BIT(0),
        Symlink     = BIT(1),
        Hidden      = BIT(2),
        ReadOnly    = BIT(3),
        System      = BIT(4),
    };

    ENUM_CLASS_FLAGS(EAttributes);

    // Times are nanoseconds since the Unix epoch, matching VFS::FFileInfo::LastModifyTime.
    struct FFileStat
    {
        uint64      Size            = 0;
        int64       LastModifyTime  = 0;
        int64       CreationTime    = 0;
        EAttributes Attributes      = EAttributes::None;
        bool        bValid          = false;

        NODISCARD bool IsDirectory() const  { return EnumHasAnyFlags(Attributes, EAttributes::Directory); }
        NODISCARD bool IsSymlink() const    { return EnumHasAnyFlags(Attributes, EAttributes::Symlink); }
        NODISCARD bool IsHidden() const     { return EnumHasAnyFlags(Attributes, EAttributes::Hidden); }
        NODISCARD bool IsReadOnly() const   { return EnumHasAnyFlags(Attributes, EAttributes::ReadOnly); }
        NODISCARD bool IsFile() const       { return bValid && !IsDirectory(); }
    };

    enum class EVisit : uint8
    {
        Continue,
        SkipSubtree,
        Stop,
    };

    // Both views point into the iterator's scratch buffer and dangle once the visitor returns.
    struct FDirectoryEntry
    {
        FStringView FullPath;
        FStringView Name;
        uint64      Size            = 0;
        int64       LastModifyTime  = 0;
        EAttributes Attributes      = EAttributes::None;
        uint32      Depth           = 0;

        NODISCARD bool IsDirectory() const  { return EnumHasAnyFlags(Attributes, EAttributes::Directory); }
        NODISCARD bool IsSymlink() const    { return EnumHasAnyFlags(Attributes, EAttributes::Symlink); }
        NODISCARD bool IsHidden() const     { return EnumHasAnyFlags(Attributes, EAttributes::Hidden); }
        NODISCARD bool IsReadOnly() const   { return EnumHasAnyFlags(Attributes, EAttributes::ReadOnly); }
        NODISCARD bool IsFile() const       { return !IsDirectory(); }

        NODISCARD FStringView GetExtension() const
        {
            const size_t Dot = Name.find_last_of('.');
            return Dot == FStringView::npos ? FStringView() : Name.substr(Dot);
        }
    };

    using FDirectoryVisitor = EVisit (*)(void* Context, const FDirectoryEntry& Entry);
}
