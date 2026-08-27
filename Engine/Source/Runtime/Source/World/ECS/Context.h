#pragma once

#include "ComponentType.h"
#include "Containers/Vector.h"
#include "Core/Assertions/Assert.h"
#include "Memory/Memory.h"

#include <new>
#include <utility>

namespace Lumina::ECS
{
    // Per-registry singletons, keyed by the same dense type id the pools use, so a lookup is an array index.
    class FRegistryContext
    {
    public:

        FRegistryContext() = default;

        ~FRegistryContext()
        {
            Reset();
        }

        FRegistryContext(const FRegistryContext&) = delete;
        FRegistryContext& operator = (const FRegistryContext&) = delete;

        FRegistryContext(FRegistryContext&& Other) noexcept
            : Entries(std::move(Other.Entries))
        {
            Other.Entries.clear();
        }

        FRegistryContext& operator = (FRegistryContext&& Other) noexcept
        {
            if (this != &Other)
            {
                Reset();
                Entries = std::move(Other.Entries);
                Other.Entries.clear();
            }
            return *this;
        }

        // Replaces any existing value on a second emplace.
        template<typename T, typename... TArgs>
        T& Emplace(TArgs&&... Args)
        {
            const FComponentTypeID TypeID = GetComponentTypeID<T>();
            FEntry& Entry = AssureEntry(TypeID);

            if (Entry.Data != nullptr)
            {
                DestroyEntry(Entry);
            }

            Entry.Info = &FComponentTypeRegistry::Get().GetInfo(TypeID);
            Entry.Data = Memory::Malloc(sizeof(T), alignof(T));
            return *new (Entry.Data) T(std::forward<TArgs>(Args)...);
        }

        template<typename T>
        NODISCARD FORCEINLINE T* Find()
        {
            const FComponentTypeID TypeID = GetComponentTypeID<T>();
            return TypeID < Entries.size() ? static_cast<T*>(Entries[TypeID].Data) : nullptr;
        }

        template<typename T>
        NODISCARD FORCEINLINE const T* Find() const
        {
            return const_cast<FRegistryContext*>(this)->Find<T>();
        }

        // Undefined unless the singleton is present, which is what TryGetSingleton is for.
        template<typename T>
        NODISCARD FORCEINLINE T& Get()
        {
            T* Value = Find<T>();
            DEBUG_ASSERT(Value != nullptr, "Get on a singleton the registry does not hold");
            return *Value;
        }

        template<typename T>
        NODISCARD FORCEINLINE const T& Get() const
        {
            return const_cast<FRegistryContext*>(this)->Get<T>();
        }

        template<typename T>
        NODISCARD FORCEINLINE bool Contains() const
        {
            return Find<T>() != nullptr;
        }

        template<typename T>
        T& GetOrEmplace()
        {
            if (T* Existing = Find<T>())
            {
                return *Existing;
            }
            return Emplace<T>();
        }

        template<typename T>
        bool Erase()
        {
            const FComponentTypeID TypeID = GetComponentTypeID<T>();
            if (TypeID >= Entries.size() || Entries[TypeID].Data == nullptr)
            {
                return false;
            }
            DestroyEntry(Entries[TypeID]);
            return true;
        }

        void Reset()
        {
            for (FEntry& Entry : Entries)
            {
                if (Entry.Data != nullptr)
                {
                    DestroyEntry(Entry);
                }
            }
            Entries.clear();
        }

        void Swap(FRegistryContext& Other) noexcept
        {
            Entries.swap(Other.Entries);
        }

    private:

        struct FEntry
        {
            void* Data = nullptr;
            const FComponentTypeInfo* Info = nullptr;
        };

        FEntry& AssureEntry(FComponentTypeID TypeID)
        {
            if (TypeID >= Entries.size())
            {
                Entries.resize(static_cast<size_t>(TypeID) + 1u);
            }
            return Entries[TypeID];
        }

        static void DestroyEntry(FEntry& Entry)
        {
            if (Entry.Info != nullptr && Entry.Info->Destruct != nullptr && !Entry.Info->bTriviallyDestructible)
            {
                Entry.Info->Destruct(Entry.Data);
            }
            void* Block = Entry.Data;
            Memory::Free(Block);
            Entry.Data = nullptr;
            Entry.Info = nullptr;
        }

        TVector<FEntry> Entries;
    };
}
