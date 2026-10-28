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
        // Single global mutex for the rare first-resolve GUID write; hot reads skip it.
        FMutex& ResolveMutex()
        {
            static FMutex M;
            return M;
        }
    }

    bool FSoftObjectPath::TryResolve() const
    {
        LUMINA_PROFILE_SCOPE();
        FAssetRegistry& Registry = FAssetRegistry::Get();

        if (CachedGUID.IsValid())
        {
            // A rename keeps the GUID and moves the file, so the registry is the only route back to a live path.
            const FAssetData* Data = Registry.GetAssetByGUID(CachedGUID);
            if (Data == nullptr)
            {
                // An asset deleted and made again at the same path has a new GUID, so the authored path finds it.
                if (const FAssetData* Recreated = Path.empty() ? nullptr : Registry.GetAssetByPath(FStringView(Path.c_str(), Path.size())))
                {
                    FScopeLock Lock(ResolveMutex());
                    CachedGUID = Recreated->AssetGUID;
                }
                return true;
            }

            const FStringView Current(Data->Path.c_str(), Data->Path.size());

            FScopeLock Lock(ResolveMutex());
            if (VFS::RemoveExtension(FStringView(Path.c_str(), Path.size())) != VFS::RemoveExtension(Current))
            {
                Path.assign(Current.data(), Current.size());
            }
            return true;
        }

        if (Path.empty())
        {
            return false;
        }

        FAssetData* Data = Registry.GetAssetByPath(FStringView(Path.c_str(), Path.size()));
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
        return true;
    }

    CObject* FSoftObjectPath::LoadSynchronous() const
    {
        return TryResolve() ? FAssetManager::Get().LoadSynchronous(CachedGUID) : nullptr;
    }

    void FSoftObjectPath::LoadAsync(const TFunction<void(CObject*)>& Callback) const
    {
        LUMINA_PROFILE_SCOPE();
        if (!TryResolve())
        {
            if (Callback)
            {
                Callback(nullptr);
            }
            return;
        }
        FAssetHandle Handle = FAssetManager::Get().Load(CachedGUID);
        if (Callback)
        {
            Handle.OnReady([Callback](const FAssetHandle& Loaded) { Callback(Loaded.Get()); });
        }
    }

    FArchive& operator<<(FArchive& Ar, FSoftObjectPath& Self)
    {
        // Resolved first on write, so the saver folds a current GUID in as a Soft edge for the cook graph.
        if (Ar.IsWriting())
        {
            if (!Self.Path.empty())
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
