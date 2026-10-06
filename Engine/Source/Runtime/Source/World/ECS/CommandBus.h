#pragma once

#include "Registry.h"
#include "Containers/Vector.h"

namespace Lumina::ECS
{
    // Each thread records into its own slot without a lock, and a flush applies creates, then commands in record order, then destroys.
    class RUNTIME_API FCommandBus
    {
    public:

        explicit FCommandBus(FRegistry& InRegistry);
        ~FCommandBus();

        FCommandBus(const FCommandBus&) = delete;
        FCommandBus& operator = (const FCommandBus&) = delete;

        // The handle is usable as a command target at once and becomes live at the next flush.
        NODISCARD FEntity Create();

        void Destroy(FEntity Entity);

        // Adds the component, or replaces it when the entity already has one.
        template<CComponent T, typename... TArgs>
        void Emplace(FEntity Entity, TArgs&&... Args)
        {
            if constexpr (CDataComponent<T>)
            {
                Enqueue([Entity, Value = T(std::forward<TArgs>(Args)...)](FRegistry& InRegistry) mutable
                {
                    if (InRegistry.IsValid(Entity))
                    {
                        InRegistry.EmplaceOrReplace<T>(Entity, std::move(Value));
                    }
                });
            }
            else
            {
                Enqueue([Entity](FRegistry& InRegistry)
                {
                    if (InRegistry.IsValid(Entity))
                    {
                        InRegistry.EmplaceOrReplace<T>(Entity);
                    }
                });
            }
        }

        template<CComponent T>
        void Remove(FEntity Entity)
        {
            Enqueue([Entity](FRegistry& InRegistry)
            {
                if (InRegistry.IsValid(Entity))
                {
                    InRegistry.Remove<T>(Entity);
                }
            });
        }

        // Runs the mutator at the flush and fires OnUpdate, skipping an entity that no longer has the component.
        template<CDataComponent T, typename TFunc>
        void Patch(FEntity Entity, TFunc&& Mutator)
        {
            Enqueue([Entity, Mutator = std::forward<TFunc>(Mutator)](FRegistry& InRegistry) mutable
            {
                if (InRegistry.IsValid(Entity) && InRegistry.TryGet<T>(Entity) != nullptr)
                {
                    InRegistry.Patch<T>(Entity, Mutator);
                }
            });
        }

        // Func takes FRegistry& or nothing, and runs on the flushing thread in this slot's record order.
        template<typename TFunc>
        void Enqueue(TFunc&& Func)
        {
            using TCallable = std::decay_t<TFunc>;
            static_assert(alignof(TCallable) <= CommandAlignment, "A deferred command cannot be over-aligned.");

            void* Payload = AllocateCommand(&ApplyCallable<TCallable>, sizeof(TCallable));
            new (Payload) TCallable(std::forward<TFunc>(Func));
        }

        // Main thread only, while nothing else records. Commands recorded while applying run in the same flush.
        uint32 Flush();

        // Drops every pending command without running it, returning reserved handles to the registry.
        void Discard();

        NODISCARD bool HasPending() const;

        // True on a worker, or on a thread the scheduler marked because it runs a parallel system.
        NODISCARD static bool ShouldDefer();

        NODISCARD static bool IsThreadDeferring();
        static void SetThreadDeferring(bool bDefer);

        struct FDeferScope
        {
            explicit FDeferScope(bool bEnable) : bPrevious(IsThreadDeferring()) { SetThreadDeferring(bEnable || bPrevious); }
            ~FDeferScope() { SetThreadDeferring(bPrevious); }
            FDeferScope(const FDeferScope&) = delete;
            FDeferScope& operator = (const FDeferScope&) = delete;

            bool bPrevious;
        };

    private:

        static constexpr size_t CommandAlignment = 16;
        static constexpr size_t BlockSize = 64 * 1024;

        // A null registry destroys the payload without running it.
        using FApplyFn = void (*)(void* Payload, FRegistry* Target);

        template<typename TCallable>
        static void ApplyCallable(void* Payload, FRegistry* Target)
        {
            TCallable& Callable = *static_cast<TCallable*>(Payload);
            if (Target != nullptr)
            {
                if constexpr (std::is_invocable_v<TCallable&, FRegistry&>)
                {
                    Callable(*Target);
                }
                else
                {
                    Callable();
                }
            }
            Callable.~TCallable();
        }

        struct FBlock
        {
            uint8*  Data     = nullptr;
            size_t  Capacity = 0;
            size_t  Used     = 0;
        };

        struct FSlot
        {
            TVector<FBlock>  Blocks;
            uint32           ActiveBlock = 0;
            uint32           NumCommands = 0;
            TVector<FEntity> Creates;
            TVector<FEntity> Destroys;

            // Keeps neighboring slots off one cache line, since each is written by a different thread.
            uint8            Padding[64] = {};
        };

        void* AllocateCommand(FApplyFn Apply, size_t PayloadSize);
        FSlot& LocalSlot();
        static void RunCommands(FSlot& Slot, FRegistry* Target);
        static void FreeBlocks(FSlot& Slot);

        FRegistry&      Registry;
        TVector<FSlot>  Slots;
    };
}
