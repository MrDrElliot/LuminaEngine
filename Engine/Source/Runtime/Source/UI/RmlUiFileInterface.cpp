#include "RuntimePCH.h"
#include "RmlUiFileInterface.h"

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Font/Font.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "FileSystem/FileSystem.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Log/Log.h"
#include "Memory/Memcpy.h"

#include <cstdio>

namespace Lumina
{
    namespace
    {
        struct FOpenedFile
        {
            TVector<uint8> Bytes;
            int32          Cursor = 0;
        };

        int64 WriteTimeOf(FStringView VirtualPath)
        {
            const FPathString Physical = VFS::ResolvePath(VirtualPath);
            return Physical.IsEmpty() ? 0 : Filesystem::LastWriteTime(Physical);
        }

        // A path naming a CFont asset is resolved through the asset system, not read raw off the VFS.
        bool TryReadFontAsset(const Rml::String& RawPath, TVector<uint8>& OutBytes)
        {
            FString Path(RawPath.c_str(), IntCastChecked<int32>(RawPath.size()));

            // RmlUi strips the leading slash off absolute paths; asset paths keep it.
            if (!Path.IsEmpty() && Path[0] != '/')
            {
                Path = "/" + Path;
            }

            const FStringView PathView(*Path, Path.Len());
            const FAssetDataPtr Data = FAssetRegistry::Get().GetAssetByPath(PathView);
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

        // Routes through VFS so PAK mounts work, and returns the path the bytes came from.
        bool ReadRmlFile(const Rml::String& RawPath, TVector<uint8>& OutBytes, FFixedString& OutPath)
        {
            const FStringView Source(RawPath.c_str(), IntCastChecked<int32>(RawPath.size()));
            OutPath = FFixedString(Source.GetData(), Source.Len());
            if (TryReadFontAsset(RawPath, OutBytes) || VFS::ReadFile(OutBytes, Source))
            {
                return true;
            }

            // Fall back to virtual-path resolver for absolute editor paths.
            OutPath = VFS::ResolveToVirtualPath(Source);
            return VFS::ReadFile(OutBytes, FStringView(*OutPath, OutPath.Len()));
        }
    }

    Rml::FileHandle FRmlUiFileInterface::Open(const Rml::String& Path)
    {
        TVector<uint8> Bytes;
        FFixedString ReadPath;
        if (!ReadRmlFile(Path, Bytes, ReadPath))
        {
            LOG_WARN("[RmlUi] FileInterface::Open: '{}' not found.", Path.c_str());
            return 0;
        }
        NoteStyleSheetRead(FStringView(*ReadPath, ReadPath.Len()));

        FOpenedFile* File = new FOpenedFile{};
        File->Bytes  = Move(Bytes);
        File->Cursor = 0;
        return Rml::FileHandle(File);
    }

    void FRmlUiFileInterface::NoteStyleSheetRead(FStringView VirtualPath)
    {
        constexpr FStringView Extension(".rcss");
        if (!VirtualPath.EndsWith(Extension))
        {
            return;
        }
        const int64 WriteTime = WriteTimeOf(VirtualPath);
        auto Guard = ReadStyleSheets.Lock();
        for (FReadStyleSheet& Sheet : *Guard)
        {
            if (FStringView(*Sheet.Path, Sheet.Path.Len()) == VirtualPath)
            {
                Sheet.WriteTime = WriteTime;
                return;
            }
        }
        Guard->Emplace(FString(VirtualPath.GetData(), VirtualPath.Len()), WriteTime);
    }

    bool FRmlUiFileInterface::ConsumeChangedStyleSheets()
    {
        auto Guard = ReadStyleSheets.Lock();
        bool bChanged = false;
        for (const FReadStyleSheet& Sheet : *Guard)
        {
            if (WriteTimeOf(FStringView(*Sheet.Path, Sheet.Path.Len())) != Sheet.WriteTime)
            {
                bChanged = true;
                break;
            }
        }
        // Every sheet is parsed again after a cache clear, which records its new time.
        if (bChanged)
        {
            Guard->Reset();
        }
        return bChanged;
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
        const size_t Remaining = static_cast<size_t>(File->Bytes.Num() - File->Cursor);
        const size_t ToCopy = (Size < Remaining) ? Size : Remaining;
        if (ToCopy > 0)
        {
            Memory::Memcpy(Buffer, File->Bytes.GetData() + File->Cursor, ToCopy);
            File->Cursor += static_cast<int32>(ToCopy);
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
        int64 NewCursor = 0;
        switch (Origin)
        {
        case SEEK_SET: NewCursor = Offset; break;
        case SEEK_CUR: NewCursor = File->Cursor + static_cast<int64>(Offset); break;
        case SEEK_END: NewCursor = File->Bytes.Num() + static_cast<int64>(Offset); break;
        default:       return false;
        }
        if (NewCursor < 0 || NewCursor > File->Bytes.Num())
        {
            return false;
        }
        File->Cursor = static_cast<int32>(NewCursor);
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
        return File ? File->Bytes.Num() : 0;
    }

    bool FRmlUiFileInterface::LoadFile(const Rml::String& Path, Rml::String& OutData)
    {
        TVector<uint8> Bytes;
        FFixedString ReadPath;
        if (!ReadRmlFile(Path, Bytes, ReadPath))
        {
            return false;
        }
        OutData.assign(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num());
        return true;
    }
}
