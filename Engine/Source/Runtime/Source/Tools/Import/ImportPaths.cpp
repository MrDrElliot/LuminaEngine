#include "RuntimePCH.h"
#include "ImportPaths.h"

#include "Containers/StringFormat.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Assets/AssetRegistry/AssetRegistry.h"

namespace Lumina::Import::PathReservations
{
    namespace
    {
        constexpr uint32 kMaxSuffix = 10000;

        // Neither the extension nor case tells two packages apart, so neither may tell two reservations apart.
        FFixedString KeyOf(FStringView Path)
        {
            FFixedString Key(Path.GetData(), Path.Len());
            if (VFS::HasExtension(Key, ".lasset"))
            {
                Key.SetLen(Key.Len() - FStringView(".lasset").Len());
            }
            for (char& Character : Key)
            {
                Character = (char)std::tolower((unsigned char)Character);
            }
            return Key;
        }

        bool IsOwned(const FFixedString& Path)
        {
            // What CreatePackage actually refuses on.
            if (FindObject<CPackage>(Path) != nullptr)
            {
                return true;
            }

            // An asset already on disk that nothing has loaded this session.
            FFixedString OnDisk = Path;
            CPackage::AddPackageExt(OnDisk);
            if (VFS::Exists(OnDisk))
            {
                return true;
            }

            return FAssetRegistry::Get().IsImportPathReserved(KeyOf(Path));
        }

        // Base, then Base_1, Base_2 and so on, stopping at the first Candidate Claim accepts.
        template<typename TClaim>
        FFixedString FirstClaimed(const FFixedString& Base, TClaim&& Claim)
        {
            if (Claim(Base))
            {
                return Base;
            }

            for (uint32 N = 1; N < kMaxSuffix; ++N)
            {
                FFixedString Candidate = Base;
                Candidate.Append("_").Append(*Format("{}", N));
                if (Claim(Candidate))
                {
                    return Candidate;
                }
            }
            return {};
        }
    }

    bool IsFree(FStringView Path)
    {
        if (Path.IsEmpty())
        {
            return false;
        }

        return !IsOwned(FFixedString(Path.GetData(), Path.Len()));
    }

    FFixedString FindFree(FStringView Path)
    {
        if (Path.IsEmpty())
        {
            return {};
        }

        FFixedString Base(Path.GetData(), Path.Len());
        return FirstClaimed(Base, [](const FFixedString& Candidate) { return !IsOwned(Candidate); });
    }

    FFixedString Reserve(FStringView Path)
    {
        if (Path.IsEmpty())
        {
            return {};
        }

        FAssetRegistry& Registry = FAssetRegistry::Get();
        FFixedString Base(Path.GetData(), Path.Len());
        return FirstClaimed(Base, [&Registry](const FFixedString& Candidate)
        {
            return !IsOwned(Candidate) && Registry.TryReserveImportPath(KeyOf(Candidate));
        });
    }

    void Release(FStringView Path)
    {
        if (Path.IsEmpty())
        {
            return;
        }

        FAssetRegistry::Get().ReleaseImportPath(KeyOf(FFixedString(Path.GetData(), Path.Len())));
    }

    void ReleaseAll()
    {
        FAssetRegistry::Get().ReleaseAllImportPaths();
    }

    FScopedReservation::FScopedReservation(FStringView Path)
        : Reserved(Reserve(Path))
    {
    }

    FScopedReservation::~FScopedReservation()
    {
        if (!bCommitted)
        {
            Release(Reserved);
        }
    }
}
