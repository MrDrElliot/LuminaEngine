#pragma once

#include "Containers/StringFormat.h"
#include "GUID/GUID.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/NativeFileSystem.h"
#include "Platform/Filesystem/PlatformFilesystem.h"

namespace Lumina::TestUtil
{
    // A writable mount over a guid-named temp folder, so parallel test runs never share or delete each other's files.
    struct FScopedTempMount
    {
        FFixedString Alias;
        FString      Dir;

        FScopedTempMount(FStringView InAlias, const char* DirPrefix)
            : Alias(InAlias)
        {
            Dir = Format("{}/{}_{}", Filesystem::GetTempDirectory(), DirPrefix, FGuid::New().ToString());
            Filesystem::MakeDirectoryTree(Dir);
            VFS::Mount<VFS::FNativeFileSystem>(Alias, FStringView(*Dir, Dir.Len()));
        }

        ~FScopedTempMount()
        {
            VFS::Unmount(Alias);
            Filesystem::RemoveTree(Dir);
        }
    };
}
