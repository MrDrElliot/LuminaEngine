#pragma once

#include "Registry.h"
#include "Containers/Vector.h"
#include "Core/Threading/Thread.h"

namespace Lumina::ECS
{
    // Each thread records into its own slot under that slot's lock, and a flush applies creates, then commands, then destroys, each by record order then record sequence.
    class RUNTIME_API FCommandBus
    {
    public:

        explicit FCommandBus(FRegistry& InRegistry);
        ~FCommandBus();

        FCommandBus(const FCommandBus&) = delete;
        FCommandBus& operator = (const FCommandBus&) = delete;
        FCommandBus(FCommandBus&&) = delete;
        FCommandBus& operator=(FCommandBus&&) = delete;

        // The handle is usable as a command target at once and becomes live at the next flush.
        NODISCARD FEntity Create();

        void Destroy(FEntity Entity);

        // Adds the component, or replaces it when the entity already has one.
        template<CComponent T, typename... TArgs>
        void Emplace(FEntity Entity, TArgs&&... Args)
        {
            if constexpr (CDataComponent<T>)
            {
                Enqueue([Entity, Value = T(Forward<TArgs>(Args)...)](FRegistry& InRegistry) mutable
                {
                    if (InRegistry.IsValid(Entity))
                    {
                        InRegistry.EmplaceOrReplace<T>(Entity, Move(Value));
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

        template<CComponent... Ts> requires (sizeof...(Ts) > 0)
        void Remove(FEntity Entity)
        {
            Enqueue([Entity](FRegistry& InRegistry)
            {
                if (InRegistry.IsValid(Entity))
                {
                    InRegistry.Remove<Ts...>(Entity);
                }
            });
        }

        // Runs the mutator at the flush and fires OnUpdate, skipping an entity that no longer has the component.
        template<CDataComponent T, typename TFunc>
        void Patch(FEntity Entity, TFunc&& Mutator)
        {
            Enqueue([Entity, Mutator = Forward<TFunc>(Mutator)](FRegistry& InRegistry) mutable
            {
                if (InRegistry.IsValid(Entity) && InRegistry.TryGet<T>(Entity) != nullptr)
                {
                    InRegistry.Patch<T>(Entity, Mutator);
                }
            });
        }

        // Func takes FRegistry& or nothing, and runs on the flushing thread in record order, then in this thread's record sequence.
        template<typename TFunc>
        void Enqueue(TFunc&& Func)
        {
            using TCallable = std::decay_t<TFunc>;
            static_assert(alignof(TCallable) <= CommandAlignment, "A deferred command cannot be over-aligned.");

            // A thread outside the scheduler records at any moment, so the flush must never detach a half written command.
            FScopeLock Guard(LocalSlotLock());
            void* Payload = AllocateCommand(&ApplyCallable<TCallable>, sizeof(TCallable));
            new (Payload) TCallable(Forward<TFunc>(Func));
        }

        // Main thread only. Commands recorded while applying run in the same flush, and other threads may keep recording.
        uint32 Flush();

        // Drops every pending command without running it, returning reserved handles to the registry.
        void Discard();

        NODISCARD bool HasPending() const;

        // True on a worker, or on a thread the scheduler marked because it runs a parallel system.
        NODISCARD static bool ShouldDefer();

        NODISCARD static bool IsThreadDeferring();

        struct FThreadState
        {
            bool   bDeferring  = false;

            // A system's schedule index, so systems sharing a segment apply in schedule order whichever worker ran them.
            uint32 RecordOrder = 0;
        };

        static FThreadState ExchangeThreadState(const FThreadState& State);

        struct FDeferScope
        {
            explicit FDeferScope(bool bEnable, uint32 RecordOrder = 0) : Previous(ExchangeThreadState({ bEnable || IsThreadDeferring(), RecordOrder })) {}
            ~FDeferScope() { ExchangeThreadState(Previous); }
            FDeferScope(const FDeferScope&) = delete;
            FDeferScope& operator = (const FDeferScope&) = delete;
            FDeferScope(FDeferScope&&) = delete;
            FDeferScope& operator=(FDeferScope&&) = delete;

            FThreadState Previous;
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

        struct FOrderedEntity
        {
            FEntity Entity;
            uint32  RecordOrder = 0;
        };

        struct FSlot
        {
            TVector<FBlock>         Blocks;
            uint32                  ActiveBlock = 0;
            uint32                  NumCommands = 0;
            TVector<FOrderedEntity> Creates;
            TVector<FOrderedEntity> Destroys;

            // Keeps neighboring slots off one cache line, since each is written by a different thread.
            uint8            Padding[64] = {};
        };

        struct FCommandHeader;

        void* AllocateCommand(FApplyFn Apply, size_t PayloadSize);
        FSlot& LocalSlot();
        FMutex& LocalSlotLock();
        void Detach();
        uint32 RunDetachedCommands(FRegistry* Target);

        static void FreeBlocks(FSlot& Slot);

        FRegistry&      Registry;
        TVector<FSlot>  Slots;

        // One per slot, beside it rather than in it because a mutex cannot move with the vector.
        TVector<TUniquePtr<FMutex>> SlotLocks;

        // Flush scratch, kept so a steady-state flush allocates nothing.
        TVector<TVector<FBlock>> RunningBlocks;
        TVector<FCommandHeader*> RunningCommands;
        TVector<FOrderedEntity>  PendingCreates;
        TVector<FOrderedEntity>  PendingDestroys;
    };
}
