#include "ThumbnailCache.h"

#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"

#include "Platform/Filesystem/PlatformFilesystem.h"

namespace Lumina::ThumbnailCache
{
    namespace
    {
        // Identifies a valid sidecar file; a header mismatch means "regenerate".
        constexpr uint32 kCacheMagic   = 0x434D4854; // 'THMC'
        constexpr uint8  kCacheVersion = 2;

        FString CacheDirectory()
        {
            const FString& Install = Paths::GetEngineInstallDirectory();
            if (Install.IsEmpty())
            {
                return {};
            }
            return Install + "/Intermediates/ThumbnailCache";
        }
    }

    FString CachePath(const FGuid& GUID)
    {
        if (!GUID.IsValid())
        {
            return {};
        }
        const FString Dir = CacheDirectory();
        if (Dir.IsEmpty())
        {
            return {};
        }
        // Dashless/lowercase keeps the filename compact and filesystem-friendly.
        return Dir + "/" + GUID.ToString(false, false) + ".thumb";
    }

    bool Load(const FGuid& GUID, uint64 ExpectedContentHash, FPackageThumbnail& OutThumbnail)
    {
        const FString Path = CachePath(GUID);
        if (Path.IsEmpty())
        {
            return false;
        }

        // Probing disk directly keeps FileHelper's per-miss "failed to get file size" out of the log.
        if (!Filesystem::Exists(Path))
        {
            return false;
        }

        TVector<uint8> Bytes;
        if (!FileHelper::LoadFileToArray(Bytes, Path) || Bytes.IsEmpty())
        {
            return false;
        }

        FMemoryReader Ar(Bytes);

        uint32 Magic   = 0;
        uint8  Version  = 0;
        uint64 StoredHash = 0;
        Ar.Serialize(&Magic, sizeof(Magic));
        if (Magic != kCacheMagic)
        {
            return false;
        }
        Ar.Serialize(&Version, sizeof(Version));
        Ar.Serialize(&StoredHash, sizeof(StoredHash));

        // Content changed since this thumbnail was baked -> ignore, caller regenerates.
        if (Version != kCacheVersion || StoredHash != ExpectedContentHash)
        {
            return false;
        }

        OutThumbnail.Serialize(Ar);
        return !Ar.HasError() && !OutThumbnail.ImageData.IsEmpty();
    }

    bool Save(const FGuid& GUID, uint64 ContentHash, FPackageThumbnail& Thumbnail)
    {
        if (!GUID.IsValid() || Thumbnail.ImageData.IsEmpty())
        {
            return false;
        }

        const FString Path = CachePath(GUID);
        if (Path.IsEmpty())
        {
            return false;
        }

        TVector<uint8> Bytes;
        FMemoryWriter Ar(Bytes);

        uint32 Magic   = kCacheMagic;
        uint8  Version = kCacheVersion;
        Ar.Serialize(&Magic, sizeof(Magic));
        Ar.Serialize(&Version, sizeof(Version));
        Ar.Serialize(&ContentHash, sizeof(ContentHash));
        Thumbnail.Serialize(Ar);

        Paths::CreateDirectories(CacheDirectory());
        return FileHelper::SaveArrayToFile(Bytes, Path);
    }

    void Delete(const FGuid& GUID)
    {
        const FString Path = CachePath(GUID);
        if (Path.IsEmpty())
        {
            return;
        }

        Filesystem::RemoveFile(Path);
    }
}
