#include "RuntimePCH.h"

#include "FileLibrary.h"

#include "FileSystem/FileSystem.h"

namespace Lumina
{
    bool CFileLibrary::FileExists(const FString& Path)
    {
        return !Path.IsEmpty() && VFS::Exists(FStringView(*Path, Path.Len()));
    }

    void CFileLibrary::ReadFileBytes(const FString& Path, TVector<uint8>& Out)
    {
        Out.Reset();
        if (!Path.IsEmpty() && !VFS::ReadFile(Out, FStringView(*Path, Path.Len())))
        {
            Out.Reset();
        }
    }

    FString CFileLibrary::ReadFileText(const FString& Path)
    {
        FString Text;
        if (!Path.IsEmpty() && !VFS::ReadFile(Text, FStringView(*Path, Path.Len())))
        {
            Text.Reset();
        }
        return Text;
    }
}
