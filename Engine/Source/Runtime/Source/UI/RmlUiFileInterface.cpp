#include "RuntimePCH.h"
#include "RmlUiFileInterface.h"

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Font/Font.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

#include <cstdio>
#include <cstring>

namespace Lumina
{
    namespace
    {
        struct FOpenedFile
        {
            TVector<uint8> Bytes;
            size_t         Cursor = 0;
        };

        // A path naming a CFont asset is resolved through the asset system, not read raw off the VFS.
        bool TryReadFontAsset(const Rml::String& RawPath, TVector<uint8>& OutBytes)
        {
            FString Path(RawPath.c_str(), RawPath.size());

            // RmlUi strips the leading slash off absolute paths; asset paths keep it.
            if (!Path.empty() && Path[0] != '/')
            {
                Path = "/" + Path;
            }

            const FStringView PathView(Path.c_str(), Path.size());
            const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(PathView);
            if (Data == nullptr || Data->AssetClass != CFont::StaticClass()->GetName())
            {
                return false;
            }

            CFont* Font = LoadObject<CFont>(PathView);
            if (Font == nullptr || !Font->IsValid())
            {
                return false;
            }

            OutBytes = Font->GetFontData();
            return true;
        }
    }

    Rml::FileHandle FRmlUiFileInterface::Open(const Rml::String& Path)
    {
        FStringView Source(Path.c_str(), Path.size());

        TVector<uint8> Bytes;
        if (!TryReadFontAsset(Path, Bytes) && !VFS::ReadFile(Bytes, Source))
        {
            // Fall back to virtual-path resolver for absolute editor paths.
            const FFixedString Resolved = VFS::ResolveToVirtualPath(Source);
            if (!VFS::ReadFile(Bytes, FStringView(Resolved.c_str(), Resolved.size())))
            {
                LOG_WARN("[RmlUi] FileInterface::Open: '{}' not found.", Path.c_str());
                return 0;
            }
        }

        FOpenedFile* File = new FOpenedFile{};
        File->Bytes  = Move(Bytes);
        File->Cursor = 0;
        return Rml::FileHandle(File);
    }

    void FRmlUiFileInterface::Close(Rml::FileHandle Handle)
    {
        delete reinterpret_cast<FOpenedFile*>(Handle);
    }

    size_t FRmlUiFileInterface::Read(void* Buffer, size_t Size, Rml::FileHandle Handle)
    {
        FOpenedFile* File = reinterpret_cast<FOpenedFile*>(Handle);
        if (File == nullptr || Buffer == nullptr || Size == 0)
        {
            return 0;
        }
        const size_t Remaining = (File->Cursor < File->Bytes.size()) ? (File->Bytes.size() - File->Cursor) : 0;
        const size_t ToCopy = (Size < Remaining) ? Size : Remaining;
        if (ToCopy > 0)
        {
            std::memcpy(Buffer, File->Bytes.data() + File->Cursor, ToCopy);
            File->Cursor += ToCopy;
        }
        return ToCopy;
    }

    bool FRmlUiFileInterface::Seek(Rml::FileHandle Handle, long Offset, int Origin)
    {
        FOpenedFile* File = reinterpret_cast<FOpenedFile*>(Handle);
        if (File == nullptr)
        {
            return false;
        }
        size_t NewCursor = 0;
        switch (Origin)
        {
        case SEEK_SET: NewCursor = size_t(Offset); break;
        case SEEK_CUR: NewCursor = File->Cursor + size_t(Offset); break;
        case SEEK_END: NewCursor = File->Bytes.size() + size_t(Offset); break;
        default:       return false;
        }
        if (NewCursor > File->Bytes.size())
        {
            return false;
        }
        File->Cursor = NewCursor;
        return true;
    }

    size_t FRmlUiFileInterface::Tell(Rml::FileHandle Handle)
    {
        FOpenedFile* File = reinterpret_cast<FOpenedFile*>(Handle);
        return File ? File->Cursor : 0;
    }

    size_t FRmlUiFileInterface::Length(Rml::FileHandle Handle)
    {
        FOpenedFile* File = reinterpret_cast<FOpenedFile*>(Handle);
        return File ? File->Bytes.size() : 0;
    }

    bool FRmlUiFileInterface::LoadFile(const Rml::String& Path, Rml::String& OutData)
    {
        // Routes through VFS so PAK mounts work.
        FStringView Source(Path.c_str(), Path.size());
        TVector<uint8> Bytes;
        if (!TryReadFontAsset(Path, Bytes) && !VFS::ReadFile(Bytes, Source))
        {
            const FFixedString Resolved = VFS::ResolveToVirtualPath(Source);
            if (!VFS::ReadFile(Bytes, FStringView(Resolved.c_str(), Resolved.size())))
            {
                return false;
            }
        }
        OutData.assign(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
        return true;
    }
}
