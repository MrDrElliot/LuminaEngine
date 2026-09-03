#pragma once

#include "Containers/HashTable.h"
#include "Containers/Vector.h"

namespace Lumina::DotNet
{
    // Tracks contexts that root a managed delegate, so a generation unload can free every one of them.
    template <typename T>
    class TManagedContextRegistry
    {
    public:

        static void Add(T* Context) { Live().insert(Context); }

        static void Remove(T* Context) { Live().erase(Context); }

        // Snapshotted, since acting on a context can destroy it and mutate the set.
        template <typename TFunc>
        static void ForEachSnapshot(TFunc&& Func)
        {
            TVector<T*> Snapshot;
            Snapshot.reserve(Live().size());
            for (T* Context : Live())
            {
                Snapshot.push_back(Context);
            }

            for (T* Context : Snapshot)
            {
                Func(Context);
            }
        }

    private:

        static THashSet<T*>& Live()
        {
            static THashSet<T*> Set;
            return Set;
        }
    };
}
