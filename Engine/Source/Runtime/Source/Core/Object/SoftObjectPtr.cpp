#include "RuntimePCH.h"
#include "SoftObjectPtr.h"

#include "Assets/AssetManager/AssetManager.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Serialization/Archiver.h"

#include <mutex>


namespace Lumina
{
    namespace
    {
        // Single global mutex for the rare first-resolve GUID write; hot reads skip it.
        std::mutex& ResolveMutex()
        {
            static std::mutex M;
            return M;
        }
    }

    bool FSoftObjectPath::TryResolve() const
    {
        // Lock-free fast path: once any thread has populated CachedGUID,
        // every subsequent caller observes it without touching the mutex.
        if (CachedGUID.IsValid())
        {
            return true;
        }
        if (Path.empty())
        {
            return false;
        }

        FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(FStringView(Path.c_str(), Path.size()));
        if (Data == nullptr)
        {
            return false;
        }

        // Serialize the GUID write so concurrent first-resolvers can't tear the 16-byte FGuid.
        std::scoped_lock Lock(ResolveMutex());
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
        // Soft paths are FString (deep package paths); narrow to FFixedString here,
        // the load rejects oversize paths via its own bounds check.
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
        // On write, resolve first so the persisted GUID is current and the saver can fold
        // it into the ImportTable as a Soft edge for the cook graph.
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
