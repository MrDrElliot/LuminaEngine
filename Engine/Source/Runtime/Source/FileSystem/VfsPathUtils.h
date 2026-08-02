#pragma once

#include "FileInfo.h"
#include "Containers/String.h"

// Virtual-path helpers shared by the file system backends. They were copied per backend, which
// compiles fine one translation unit at a time and collides the moment two of those files share
// one. Sharing them also means the backends cannot drift on what a parent path is.

namespace Lumina::VFS
{
    // Parent of a virtual path, with a single trailing slash ignored so "/A/B/" and "/A/B" agree.
    // Empty when the path has no separator; "/" for a root-level entry.
    FORCEINLINE FStringView ParentOf(FStringView Path)
    {
        if (!Path.empty() && Path.back() == '/')
        {
            Path = Path.substr(0, Path.size() - 1);
        }

        const size_t Pos = Path.find_last_of('/');

        if (Pos == FStringView::npos)
        {
            return {};
        }

        // Keep the leading slash for root-level entries.
        return Pos == 0 ? Path.substr(0, 1) : Path.substr(0, Pos);
    }

    // Final component of a virtual path, ignoring a single trailing slash.
    FORCEINLINE FStringView FileNameOf(FStringView Path)
    {
        if (!Path.empty() && Path.back() == '/')
        {
            Path = Path.substr(0, Path.size() - 1);
        }

        const size_t Pos = Path.find_last_of('/');
        return Pos == FStringView::npos ? Path : Path.substr(Pos + 1);
    }

    // Entry flags implied by a path. bReadOnly is the backend's answer, not the path's: a pak is
    // read-only by definition where a memory mount is not.
    FORCEINLINE EFileFlags FlagsForPath(FStringView Path, bool bIsDirectory, bool bReadOnly)
    {
        EFileFlags Flags = bIsDirectory ? EFileFlags::Directory : EFileFlags::File;

        if (bReadOnly)
        {
            Flags |= EFileFlags::ReadOnly;
        }

        const size_t Dot = Path.find_last_of('.');

        if (Dot != FStringView::npos && Path.substr(Dot) == ".lasset")
        {
            Flags |= EFileFlags::LAssetFile;
        }

        return Flags;
    }
}
