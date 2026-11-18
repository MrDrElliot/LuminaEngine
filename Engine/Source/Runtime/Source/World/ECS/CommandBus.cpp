#include "RuntimePCH.h"
#include "CommandBus.h"

#include "Core/Assertions/Assert.h"
#include "Core/Threading/Thread.h"
#include "Memory/Memory.h"
#include "TaskSystem/Fork/ForkRuntime.h"
#include "World/Entity/Systems/SystemAccess.h"

namespace Lumina::ECS
{
    struct FCommandBus::FCommandHeader
    {
        void   (*Apply)(void*, FRegistry*);
        uint32 Stride;
        uint32 RecordOrder;
    };

    namespace
    {
        constexpr size_t HeaderSize = 16;

        // A flush that keeps spawning commands this many times over is a feedback loop, not a backlog.
        constexpr uint32 MaxFlushRounds = 64;

        thread_local FCommandBus::FThreadState GThreadState;

        size_t AlignUp(size_t Value, size_t Alignment)
        {
            return (Value + Alignment - 1) & ~(Alignment - 1);
        }
    }

    FCommandBus::FCommandBus(FRegistry& InRegistry)
        : Registry(InRegistry)
    {
        Slots.SetNum(Math::Max(Jobs::GetNumThreadSlots(), 1u));
        RunningBlocks.SetNum(Slots.Num());
        SlotLocks.Reserve(Slots.Num());
        for (int32 Index = 0; Index < Slots.Num(); ++Index)
        {
            SlotLocks.Add(MakeUnique<FMutex>());
        }
    }

    FCommandBus::~FCommandBus()
    {
        Discard();
        for (FSlot& Slot : Slots)
        {
            FreeBlocks(Slot);
        }
    }

    bool FCommandBus::IsThreadDeferring()
    {
        return GThreadState.bDeferring;
    }

    FCommandBus::FThreadState FCommandBus::ExchangeThreadState(const FThreadState& State)
    {
        const FThreadState Previous = GThreadState;
        GThreadState = State;
        return Previous;
    }

    bool FCommandBus::ShouldDefer()
    {
        return GThreadState.bDeferring || !Threading::IsMainThread();
    }

    FCommandBus::FSlot& FCommandBus::LocalSlot()
    {
        const uint32 Index = Jobs::GetThreadSlot();
        ASSERT(Index < static_cast<uint32>(Slots.Num()));
        return Slots[Index];
    }

    FMutex& FCommandBus::LocalSlotLock()
    {
        return *SlotLocks[Jobs::GetThreadSlot()];
    }

    FEntity FCommandBus::Create()
    {
        const FEntity Entity = Registry.ReserveEntity();
        FScopeLock Guard(LocalSlotLock());
        LocalSlot().Creates.Add({ Entity, GThreadState.RecordOrder });
        return Entity;
    }

    void FCommandBus::Destroy(FEntity Entity)
    {
        FScopeLock Guard(LocalSlotLock());
        LocalSlot().Destroys.Add({ Entity, GThreadState.RecordOrder });
    }

    void* FCommandBus::AllocateCommand(FApplyFn Apply, size_t PayloadSize)
    {
        static_assert(sizeof(FCommandHeader) <= HeaderSize, "The command header must fit its reserved space.");
        ValidateSystemQueuesCommands();

        FSlot& Slot = LocalSlot();
        const size_t Stride = HeaderSize + AlignUp(PayloadSize, CommandAlignment);

        for (;;)
        {
            if (Slot.ActiveBlock < static_cast<uint32>(Slot.Blocks.Num()))
            {
                FBlock& Block = Slot.Blocks[Slot.ActiveBlock];
                if (Block.Capacity - Block.Used >= Stride)
                {
                    uint8* Command = Block.Data + Block.Used;
                    Block.Used += Stride;
                    ++Slot.NumCommands;

                    FCommandHeader* Header = reinterpret_cast<FCommandHeader*>(Command);
                    Header->Apply  = Apply;
                    Header->Stride = static_cast<uint32>(Stride);
                    Header->RecordOrder = GThreadState.RecordOrder;
                    return Command + HeaderSize;
                }
                ++Slot.ActiveBlock;
                continue;
            }

            FBlock Fresh;
            Fresh.Capacity = Math::Max(BlockSize, Stride);
            Fresh.Data     = static_cast<uint8*>(Memory::Malloc(Fresh.Capacity, CommandAlignment));
            Slot.Blocks.Add(Fresh);
        }
    }

    void FCommandBus::Detach()
    {
        // All three lists of a slot leave together, so a create and a destroy recorded mid flush stay in one round and in order.
        PendingCreates.Reset();
        PendingDestroys.Reset();
        for (int32 Index = 0; Index < Slots.Num(); ++Index)
        {
            FScopeLock Guard(*SlotLocks[Index]);
            FSlot& Slot = Slots[Index];
            swap(RunningBlocks[Index], Slot.Blocks);
            Slot.ActiveBlock = 0;
            Slot.NumCommands = 0;
            PendingCreates.Append(Slot.Creates);
            PendingDestroys.Append(Slot.Destroys);
            Slot.Creates.Reset();
            Slot.Destroys.Reset();
        }

        const auto ByRecordOrder = [](const FOrderedEntity& A, const FOrderedEntity& B) { return A.RecordOrder < B.RecordOrder; };
        Algo::StableSort(PendingCreates, ByRecordOrder);
        Algo::StableSort(PendingDestroys, ByRecordOrder);
    }

    uint32 FCommandBus::RunDetachedCommands(FRegistry* Target)
    {
        RunningCommands.Reset();
        for (int32 Index = 0; Index < Slots.Num(); ++Index)
        {
            for (const FBlock& Block : RunningBlocks[Index])
            {
                for (size_t Offset = 0; Offset < Block.Used; Offset += reinterpret_cast<FCommandHeader*>(Block.Data + Offset)->Stride)
                {
                    RunningCommands.Add(reinterpret_cast<FCommandHeader*>(Block.Data + Offset));
                }
            }
        }

        Algo::StableSort(RunningCommands, [](const FCommandHeader* A, const FCommandHeader* B) { return A->RecordOrder < B->RecordOrder; });
        for (FCommandHeader* Header : RunningCommands)
        {
            Header->Apply(reinterpret_cast<uint8*>(Header) + HeaderSize, Target);
        }

        for (int32 Index = 0; Index < Slots.Num(); ++Index)
        {
            TVector<FBlock>& Running = RunningBlocks[Index];
            for (FBlock& Block : Running)
            {
                Block.Used = 0;
            }

            // Blocks the commands recorded into stay first, so the slot's active block still points at their unrun commands.
            FScopeLock Guard(*SlotLocks[Index]);
            TVector<FBlock>& Blocks = Slots[Index].Blocks;
            if (Blocks.IsEmpty())
            {
                swap(Blocks, Running);
            }
            else
            {
                Blocks.Append(Running);
                Running.Reset();
            }
        }
        return static_cast<uint32>(RunningCommands.Num());
    }

    void FCommandBus::FreeBlocks(FSlot& Slot)
    {
        for (FBlock& Block : Slot.Blocks)
        {
            void* Data = Block.Data;
            Memory::Free(Data);
        }
        Slot.Blocks.Reset();
        Slot.ActiveBlock = 0;
        Slot.NumCommands = 0;
    }

    bool FCommandBus::HasPending() const
    {
        for (int32 Index = 0; Index < Slots.Num(); ++Index)
        {
            FScopeLock Guard(*SlotLocks[Index]);
            const FSlot& Slot = Slots[Index];
            if (Slot.NumCommands != 0 || !Slot.Creates.IsEmpty() || !Slot.Destroys.IsEmpty())
            {
                return true;
            }
        }
        return false;
    }

    uint32 FCommandBus::Flush()
    {
        DEBUG_ASSERT(Threading::IsMainThread());

        uint32 Applied = 0;
        for (uint32 Round = 0; Round < MaxFlushRounds && HasPending(); ++Round)
        {
            Detach();
            for (const FOrderedEntity& Created : PendingCreates)
            {
                Registry.MaterializeReserved(Created.Entity);
            }
            Applied += static_cast<uint32>(PendingCreates.Num());
            Applied += RunDetachedCommands(&Registry);
            for (const FOrderedEntity& Destroyed : PendingDestroys)
            {
                Registry.Destroy(Destroyed.Entity);
            }
            Applied += static_cast<uint32>(PendingDestroys.Num());
        }

        if (HasPending())
        {
            LOG_WARN("Deferred commands kept recording more commands for {} rounds; the rest wait for the next sync point.", MaxFlushRounds);
        }
        return Applied;
    }

    void FCommandBus::Discard()
    {
        Detach();
        for (const FOrderedEntity& Created : PendingCreates)
        {
            Registry.ReleaseReserved(Created.Entity);
        }
        RunDetachedCommands(nullptr);
    }
}
