#include "RuntimePCH.h"
#include "CommandBus.h"

#include "Core/Assertions/Assert.h"
#include "Core/Threading/Thread.h"
#include "Memory/Memory.h"
#include "TaskSystem/Scheduler/JobScheduler.h"

namespace Lumina::ECS
{
    namespace
    {
        constexpr size_t HeaderSize = 16;

        struct FCommandHeader
        {
            void   (*Apply)(void*, FRegistry*);
            uint32 Stride;
        };
        static_assert(sizeof(FCommandHeader) <= HeaderSize, "The command header must fit its reserved space.");

        // A flush that keeps spawning commands this many times over is a feedback loop, not a backlog.
        constexpr uint32 MaxFlushRounds = 64;

        thread_local bool GThreadDeferring = false;

        size_t AlignUp(size_t Value, size_t Alignment)
        {
            return (Value + Alignment - 1) & ~(Alignment - 1);
        }
    }

    FCommandBus::FCommandBus(FRegistry& InRegistry)
        : Registry(InRegistry)
    {
        Slots.resize(Math::Max(Jobs::GetNumThreadSlots(), 1u));
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
        return GThreadDeferring;
    }

    void FCommandBus::SetThreadDeferring(bool bDefer)
    {
        GThreadDeferring = bDefer;
    }

    bool FCommandBus::ShouldDefer()
    {
        return GThreadDeferring || !Threading::IsMainThread();
    }

    FCommandBus::FSlot& FCommandBus::LocalSlot()
    {
        const uint32 Index = Jobs::GetWorkerIndex();
        ASSERT(Index < Slots.size());
        return Slots[Index];
    }

    FEntity FCommandBus::Create()
    {
        const FEntity Entity = Registry.ReserveEntity();
        LocalSlot().Creates.push_back(Entity);
        return Entity;
    }

    void FCommandBus::Destroy(FEntity Entity)
    {
        LocalSlot().Destroys.push_back(Entity);
    }

    void* FCommandBus::AllocateCommand(FApplyFn Apply, size_t PayloadSize)
    {
        FSlot& Slot = LocalSlot();
        const size_t Stride = HeaderSize + AlignUp(PayloadSize, CommandAlignment);

        for (;;)
        {
            if (Slot.ActiveBlock < Slot.Blocks.size())
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
                    return Command + HeaderSize;
                }
                ++Slot.ActiveBlock;
                continue;
            }

            FBlock Fresh;
            Fresh.Capacity = Math::Max(BlockSize, Stride);
            Fresh.Data     = static_cast<uint8*>(Memory::Malloc(Fresh.Capacity, CommandAlignment));
            Slot.Blocks.push_back(Fresh);
        }
    }

    void FCommandBus::RunCommands(FSlot& Slot, FRegistry* Target)
    {
        // Detached first, so a command that records more lands in fresh blocks instead of the ones being walked.
        TVector<FBlock> Running;
        Running.swap(Slot.Blocks);
        Slot.ActiveBlock = 0;
        Slot.NumCommands = 0;

        for (FBlock& Block : Running)
        {
            size_t Offset = 0;
            while (Offset < Block.Used)
            {
                FCommandHeader* Header = reinterpret_cast<FCommandHeader*>(Block.Data + Offset);
                const uint32 Stride = Header->Stride;
                Header->Apply(Block.Data + Offset + HeaderSize, Target);
                Offset += Stride;
            }
            Block.Used = 0;
        }

        for (const FBlock& Block : Running)
        {
            Slot.Blocks.push_back(Block);
        }
    }

    void FCommandBus::FreeBlocks(FSlot& Slot)
    {
        for (FBlock& Block : Slot.Blocks)
        {
            void* Data = Block.Data;
            Memory::Free(Data);
        }
        Slot.Blocks.clear();
        Slot.ActiveBlock = 0;
        Slot.NumCommands = 0;
    }

    bool FCommandBus::HasPending() const
    {
        for (const FSlot& Slot : Slots)
        {
            if (Slot.NumCommands != 0 || !Slot.Creates.empty() || !Slot.Destroys.empty())
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
        TVector<FEntity> Pending;
        for (uint32 Round = 0; Round < MaxFlushRounds && HasPending(); ++Round)
        {
            for (FSlot& Slot : Slots)
            {
                Pending.clear();
                Pending.swap(Slot.Creates);
                for (FEntity Entity : Pending)
                {
                    Registry.MaterializeReserved(Entity);
                }
                Applied += static_cast<uint32>(Pending.size());
            }

            for (FSlot& Slot : Slots)
            {
                if (Slot.NumCommands != 0)
                {
                    Applied += Slot.NumCommands;
                    RunCommands(Slot, &Registry);
                }
            }

            for (FSlot& Slot : Slots)
            {
                Pending.clear();
                Pending.swap(Slot.Destroys);
                for (FEntity Entity : Pending)
                {
                    Registry.Destroy(Entity);
                }
                Applied += static_cast<uint32>(Pending.size());
            }
        }

        if (HasPending())
        {
            LOG_WARN("Deferred commands kept recording more commands for {} rounds; the rest wait for the next sync point.", MaxFlushRounds);
        }
        return Applied;
    }

    void FCommandBus::Discard()
    {
        for (FSlot& Slot : Slots)
        {
            for (FEntity Entity : Slot.Creates)
            {
                Registry.ReleaseReserved(Entity);
            }
            Slot.Creates.clear();
            Slot.Destroys.clear();
            if (Slot.NumCommands != 0)
            {
                RunCommands(Slot, nullptr);
            }
        }
    }
}
