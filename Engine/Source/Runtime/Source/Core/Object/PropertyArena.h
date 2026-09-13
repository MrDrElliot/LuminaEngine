#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/LuminaMacros.h"
#include "Memory/Construct.h"
#include "Memory/Memory.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CStruct;
    class FProperty;
}

namespace Lumina
{
    /**
     * Bump allocator holding one type's whole FProperty set.
     *
     * Properties used to be an individual heap allocation each, chained through an intrusive Next pointer, so
     * every walk was a dependent load across blocks handed out at unrelated times during static registration.
     * One arena per type puts them back-to-back in declaration order, which is also the order every walker
     * reads them in.
     *
     * Pointers stay stable, which the script-minted layouts depend on: blocks are never moved or grown in
     * place, only chained.
     */
    class FPropertyArena
    {
    public:

        FPropertyArena() = default;
        RUNTIME_API ~FPropertyArena();

        LE_NO_COPYMOVE(FPropertyArena);

        /** Placement-constructs the property in the arena, which then owns its destruction. */
        template<typename T, typename... TArgs>
        requires std::is_base_of_v<FProperty, T>
        T* Emplace(TArgs&&... Args)
        {
            void* Storage = Allocate(sizeof(T), alignof(T));
            T* Property = Memory::ConstructAt(static_cast<T*>(Storage), std::forward<TArgs>(Args)...);
            Owned.push_back(Property);
            return Property;
        }

        /** Raw arena storage, for the metadata table a script-minted property builds at runtime. */
        RUNTIME_API void* AllocateBytes(size_t Size, size_t Alignment);

        /** Copies Text into the arena and returns a null-terminated copy that lives as long as the arena. */
        RUNTIME_API const char* CopyString(FStringView Text);

        /** Sizes the next block so a whole type's properties land in one allocation. */
        RUNTIME_API void Reserve(size_t Bytes);

        RUNTIME_API void Reset();

        NODISCARD bool IsEmpty() const { return Owned.empty(); }

        NODISCARD size_t Num() const { return Owned.size(); }

    private:

        RUNTIME_API void* Allocate(size_t Size, size_t Alignment);

        struct FBlock
        {
            FBlock* Next;
            size_t  Used;
            size_t  Capacity;
        };

        static constexpr size_t kDefaultBlockBytes = 4096;

        FBlock*             Head = nullptr;

        // Destruction order and the walk order are both declaration order, so one list serves each.
        TVector<FProperty*> Owned;
    };

    /**
     * Where a newly built property is placed and what it hangs off: the arena that owns its storage, plus
     * either the struct that declares it or the property that holds it as an inner.
     *
     * Both construction paths (compile-time registration and a script-minted layout) thread this, so a
     * property's storage and its attachment are decided in one place instead of by a constructor side effect.
     */
    struct FPropertyOwner
    {
        FPropertyArena* Arena  = nullptr;
        CStruct*        Struct = nullptr;
        FProperty*      Field  = nullptr;

        // A function parameter belongs to neither, so it is collected here and the function takes it from
        // there. Set in place of Struct, which is what keeps a parameter off the owning type's member list.
        TVector<FProperty*>* Collector = nullptr;

        template<typename T, typename TParams>
        T* Build(const TParams* Params) const
        {
            T* Property = Arena->Emplace<T>(Params);
            Attach(Property);
            return Property;
        }

        /** Owner for an inner of Property (array element, map key/value, enum or optional payload). */
        NODISCARD FPropertyOwner Inner(FProperty* Property) const { return FPropertyOwner{ Arena, nullptr, Property, nullptr }; }

        RUNTIME_API void Attach(FProperty* Property) const;
    };
}
