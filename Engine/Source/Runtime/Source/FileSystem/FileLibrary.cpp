#include "RuntimePCH.h"

#include "FileLibrary.h"

#include "FileSystem/FileSystem.h"

namespace Lumina
{
    bool CFileLibrary::FileExists(const FString& Path)
    {
        return !Path.empty() && VFS::Exists(FStringView(Path.c_str(), Path.size()));
    }

    void CFileLibrary::ReadFileBytes(const FString& Path, TVector<uint8>& Out)
    {
        Out.clear();
        if (!Path.empty() && !VFS::ReadFile(Out, FStringView(Path.c_str(), Path.size())))
        {
            Out.clear();
        }
    }

    FString CFileLibrary::ReadFileText(const FString& Path)
    {
        FString Text;
        if (!Path.empty() && !VFS::ReadFile(Text, FStringView(Path.c_str(), Path.size())))
        {
            Text.clear();
        }
        return Text;
    }
}
