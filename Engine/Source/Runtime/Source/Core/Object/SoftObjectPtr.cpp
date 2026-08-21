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
        FAssetRegistry& Registry = FAssetRegistry::Get();

        if (CachedGUID.IsValid())
        {
            // A rename keeps the GUID and moves the file, so the registry is the only route back to a live path.
            const FAssetData* Data = Registry.GetAssetByGUID(CachedGUID);
            if (Data == nullptr)
            {
                // Transient objects and deleted assets have no registry entry; leave Path as authored.
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
        if (!TryResolve())
        {
            return nullptr;
        }
        // Soft paths are deep, and the load rejects an oversize path through its own bounds check.
        return FAssetManager::Get().LoadAssetSynchronous(FFixedString(Path.c_str(), Path.size()), CachedGUID);
    }

    void FSoftObjectPath::LoadAsync(const TFunction<void(CObject*)>& Callback) const
    {
        if (!TryResolve())
        {
            if (Callback)
            {
                Callback(nullptr);
            }
            return;
        }
        FAssetHandle Handle = FAssetManager::Get().LoadAssetAsync(
            FFixedString(Path.c_str(), Path.size()), CachedGUID);
        if (!Handle.IsValid())
        {
            if (Callback)
            {
                Callback(nullptr);
            }
            return;
        }
        if (Callback)
        {
            Handle.Then([Callback](CObject*& Obj) { Callback(Obj); });
        }
    }

    FArchive& operator<<(FArchive& Ar, FSoftObjectPath& Self)
    {
        // Resolved first on write, so the saver folds a current GUID in as a Soft edge for the cook graph.
        if (Ar.IsWriting())
        {
            if (!Self.Path.empty() && !Self.CachedGUID.IsValid())
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
