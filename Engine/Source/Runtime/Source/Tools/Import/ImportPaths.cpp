#include "RuntimePCH.h"
#include "ImportPaths.h"

#include "Containers/HashTable.h"
#include "Containers/StringFormat.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Core/Threading/Thread.h"
#include "Core/Threading/Sync.h"

namespace Lumina::Import::Paths
{
    namespace
    {
        // Import runs on workers, and the content browser reserves from the main thread.
        FMutex               GMutex;
        THashSet<FFixedString> GReserved;

        constexpr uint32 kMaxSuffix = 10000;

        bool IsOwnedUnlocked(const FFixedString& Path)
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

            return GReserved.find(Path) != GReserved.end();
        }
    }

    bool IsFree(FStringView Path)
    {
        if (Path.empty())
        {
            return false;
        }

        FScopeLock Lock(GMutex);
        return !IsOwnedUnlocked(FFixedString(Path.data(), Path.size()));
    }

    FFixedString Reserve(FStringView Path)
    {
        if (Path.empty())
        {
            return {};
        }

        const FFixedString Base(Path.data(), Path.size());

        FScopeLock Lock(GMutex);
        if (!IsOwnedUnlocked(Base))
        {
            GReserved.insert(Base);
            return Base;
        }

        for (uint32 N = 1; N < kMaxSuffix; ++N)
        {
            FFixedString Candidate = Base;
            Candidate.append("_").append(Format("{}", N).c_str());
            if (!IsOwnedUnlocked(Candidate))
            {
                GReserved.insert(Candidate);
                return Candidate;
            }
        }

        return {};
    }

    void Release(FStringView Path)
    {
        if (Path.empty())
        {
            return;
        }

        FScopeLock Lock(GMutex);
        GReserved.erase(FFixedString(Path.data(), Path.size()));
    }

    void ReleaseAll()
    {
        FScopeLock Lock(GMutex);
        GReserved.clear();
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
