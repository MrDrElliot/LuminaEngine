#include "Core/Threading/Thread.h"
#include "RuntimePCH.h"
#include "SoftObjectPtr.h"

#include "Assets/AssetManager/AssetManager.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Serialization/Archiver.h"
#include "FileSystem/FileSystem.h"



namespace Lumina
{
    namespace
    {
        // Single global mutex guarding every read and write of the mutable Path and CachedGUID during a resolve.
        FMutex& ResolveMutex()
        {
            static FMutex M;
            return M;
        }
    }

    bool FSoftObjectPath::TryResolve() const
    {
        FGuid Unused;
        return TryResolveInternal(Unused);
    }

    bool FSoftObjectPath::TryResolveInternal(FGuid& OutGUID) const
    {
        LUMINA_PROFILE_SCOPE();
        FAssetRegistry& Registry = FAssetRegistry::Get();

        FString LocalPath;
        FGuid LocalGUID;
        {
            FScopeLock Lock(ResolveMutex());
            LocalPath = Path;
            LocalGUID = CachedGUID;
        }

        if (LocalGUID.IsValid())
        {
            // A rename keeps the GUID and moves the file, so the registry is the only route back to a live path.
            const FAssetDataPtr Data = Registry.GetAssetByGUID(LocalGUID);
            if (Data == nullptr)
            {
                // An asset deleted and made again at the same path has a new GUID, so the authored path finds it.
                if (const FAssetDataPtr Recreated = LocalPath.IsEmpty() ? nullptr : Registry.GetAssetByPath(FStringView(*LocalPath, LocalPath.Len())))
                {
                    FScopeLock Lock(ResolveMutex());
                    CachedGUID = Recreated->AssetGUID;
                    LocalGUID = CachedGUID;
                }
                OutGUID = LocalGUID;
                return true;
            }

            const FStringView Current(*Data->Path, Data->Path.Len());

            FScopeLock Lock(ResolveMutex());
            if (VFS::RemoveExtension(FStringView(*LocalPath, LocalPath.Len())) != VFS::RemoveExtension(Current))
            {
                Path = FStringView(Current.GetData(), Current.Len());
            }
            OutGUID = LocalGUID;
            return true;
        }

        if (LocalPath.IsEmpty())
        {
            return false;
        }

        FAssetDataPtr Data = Registry.GetAssetByPath(FStringView(*LocalPath, LocalPath.Len()));
        if (Data == nullptr)
        {
            return false;
        }

        // Serialize the GUID write so concurrent first-resolvers can't tear the 16-byte FGuid.
        FScopeLock Lock(ResolveMutex());
        if (!CachedGUID.IsValid())
        {
            CachedGUID = Data->AssetGUID;
        }
        OutGUID = CachedGUID;
        return true;
    }

    CObject* FSoftObjectPath::LoadSynchronous() const
    {
        FGuid GUID;
        return TryResolveInternal(GUID) ? FAssetManager::Get().LoadSynchronous(GUID) : nullptr;
    }

    FAssetHandle FSoftObjectPath::Load() const
    {
        FGuid GUID;
        return TryResolveInternal(GUID) ? FAssetManager::Get().Load(GUID) : FAssetHandle();
    }

    void FSoftObjectPath::LoadAsync(const TFunction<void(CObject*)>& Callback) const
    {
        LUMINA_PROFILE_SCOPE();
        const FAssetHandle Handle = Load();
        if (!Callback)
        {
            return;
        }
        if (!Handle.IsValid())
        {
            Callback(nullptr);
            return;
        }
        Handle.OnReady([Callback](const FAssetHandle& Loaded) { Callback(Loaded.Get()); });
    }

    FArchive& operator<<(FArchive& Ar, FSoftObjectPath& Self)
    {
        // Resolved first on write, so the saver folds a current GUID in as a Soft edge for the cook graph.
        if (Ar.IsWriting())
        {
            if (!Self.Path.IsEmpty())
            {
                (void)Self.TryResolve();
            }

            (void)Ar.RewriteSoftAssetReference(Self.Path, Self.CachedGUID);
        }

        Ar << Self.Path;
        Ar << Self.CachedGUID;

        if (Ar.IsWriting() && Self.CachedGUID.IsValid())
        {
            Ar.RegisterSoftAssetReference(Self.CachedGUID);
        }
        return Ar;
    }
}
