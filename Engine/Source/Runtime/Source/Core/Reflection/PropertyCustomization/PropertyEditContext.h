#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"

namespace Lumina
{
    // Data a property editor needs that no FProperty can carry, contributed by whoever owns the draw.
    class RUNTIME_API FPropertyEditContext
    {
    public:

        FPropertyEditContext() = default;
        explicit FPropertyEditContext(const FPropertyEditContext* InParent) : Parent(InParent) {}

        FPropertyEditContext(const FPropertyEditContext&) = delete;
        FPropertyEditContext& operator=(const FPropertyEditContext&) = delete;

        // Borrowed for the draw only; the provider owns the storage.
        template<typename T>
        void Provide(T* Value) { Set(T::ContextKey(), const_cast<void*>(static_cast<const void*>(Value))); }

        template<typename T>
        T* Get() const { return static_cast<T*>(Find(T::ContextKey())); }

        // FName keys, not template type ids, because Runtime and Editor are separate DLLs.
        void Set(const FName& Key, void* Value);

        // Falls through to Parent when this level does not carry the key.
        void* Find(const FName& Key) const;

        void SetParent(const FPropertyEditContext* InParent) { Parent = InParent; }

        // The shared empty context, so nothing downstream needs a null check.
        static const FPropertyEditContext& None();

    private:

        struct FEntry
        {
            FName Key;
            void* Value = nullptr;
        };

        TVector<FEntry>             Entries;
        const FPropertyEditContext* Parent = nullptr;
    };

    // Replaces the bReadOnly bool the draw path already threads down.
    struct FPropertyDrawArgs
    {
        bool bReadOnly = false;
        const FPropertyEditContext& Context = FPropertyEditContext::None();
    };
}
