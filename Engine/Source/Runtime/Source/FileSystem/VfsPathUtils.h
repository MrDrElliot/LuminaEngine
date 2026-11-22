#pragma once

#include "FileInfo.h"
#include "Containers/String.h"

// Shared by the file system backends so they cannot drift on what a parent path is.

namespace Lumina::VFS
{
    // A single trailing slash is ignored, and the result is empty when the path has no separator.
    FORCEINLINE FStringView ParentOf(FStringView Path)
    {
        if (!Path.IsEmpty() && Path.Last() == '/')
        {
            Path = Path.Mid(0, Path.Len() - 1);
        }

        const int32 Pos = Path.FindLastOf('/');

        if (Pos == Constants::kIndexNone)
        {
            return {};
        }

        // Keep the leading slash for root-level entries.
        return Pos == 0 ? Path.Mid(0, 1) : Path.Mid(0, Pos);
    }

    // Final component of a virtual path, ignoring a single trailing slash.
    FORCEINLINE FStringView FileNameOf(FStringView Path)
    {
        if (!Path.IsEmpty() && Path.Last() == '/')
        {
            Path = Path.Mid(0, Path.Len() - 1);
        }

        const int32 Pos = Path.FindLastOf('/');
        return Pos == Constants::kIndexNone ? Path : Path.Mid(Pos + 1);
    }

    // bReadOnly is the backend's answer, since a pak is read-only by definition and a memory mount is not.
    FORCEINLINE EFileFlags FlagsForPath(FStringView Path, bool bIsDirectory, bool bReadOnly)
    {
        EFileFlags Flags = bIsDirectory ? EFileFlags::Directory : EFileFlags::File;

        if (bReadOnly)
        {
            Flags |= EFileFlags::ReadOnly;
        }

        const int32 Dot = Path.FindLastOf('.');

        if (Dot != Constants::kIndexNone && Path.Mid(Dot) == ".lasset")
        {
            Flags |= EFileFlags::LAssetFile;
        }

        return Flags;
    }
}
