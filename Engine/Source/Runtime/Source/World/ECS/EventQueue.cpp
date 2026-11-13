#include "RuntimePCH.h"
#include "EventQueue.h"

#include "Core/Assertions/Assert.h"
#include "Core/Math/Math.h"
#include "Log/Log.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "Memory/Memcpy.h"
#include "Memory/Memory.h"
#include "TaskSystem/Fork/ForkRuntime.h"

namespace Lumina::ECS
{
    namespace
    {
        // A fan-out that keeps sending into the queue it was handed would otherwise never let the sync point finish.
        constexpr int32 MaxCommitRounds = 64;
    }

    FComponentTypeID FindEventType(FName Name)
    {
        FComponentTypeRegistry& Types = FComponentTypeRegistry::Get();
        if (CStruct* Struct = ResolveStructByName(Name))
        {
            const FComponentTypeID TypeID = Types.FindByStruct(Struct);
            if (TypeID != InvalidComponentTypeID)
            {
                return TypeID;
            }
        }
        return Types.FindByName(Name);
    }

    FComponentTypeID AcquireScriptEventType(FName Name, uint32 Size, uint32 Alignment)
    {
        FComponentTypeInfo Info;
        Info.Name                   = Name;
        Info.Size                   = Size;
        Info.Alignment              = Math::Max(Alignment, 1u);
        Info.bTriviallyRelocatable  = true;
        Info.bTriviallyDestructible = true;
        Info.bTriviallyCopyable     = true;
        Info.bRuntimeLayout         = true;

        FComponentTypeRegistry& Types = FComponentTypeRegistry::Get();
        const FComponentTypeID TypeID = Types.Acquire(Info);
        const FComponentTypeInfo& Existing = Types.GetInfo(TypeID);
        if (Existing.Size != Info.Size || Existing.Alignment != Info.Alignment)
        {
            Types.Relayout(TypeID, Info, nullptr);
        }
        return TypeID;
    }

    FEventQueue::FEventQueue(FComponentTypeID InTypeID)
        : Info(FComponentTypeRegistry::Get().GetInfo(InTypeID))
        , TypeID(InTypeID)
        , Stride(Math::Max(Info.Size, 1u))
    {
        Staging.SetNum(Math::Max(Jobs::GetNumThreadSlots(), 1u));
    }

    FEventQueue::~FEventQueue()
    {
        Clear();
        FreeBuffer(Buffers[0]);
        FreeBuffer(Buffers[1]);
        for (FBuffer& Slot : Staging)
        {
            FreeBuffer(Slot);
        }
    }

    void FEventQueue::Reserve(FBuffer& Buffer, uint32 Wanted)
    {
        if (Wanted <= Buffer.Capacity)
        {
            return;
        }

        const uint32 NewCapacity = Math::Max(Wanted, Math::Max(Buffer.Capacity * 2u, 16u));
        uint8* NewData = static_cast<uint8*>(Memory::Malloc((size_t)NewCapacity * Stride, Math::Max(Info.Alignment, 1u)));

        if (Buffer.Count != 0)
        {
            if (Info.bTriviallyRelocatable)
            {
                Memory::Memcpy(NewData, Buffer.Data, (size_t)Buffer.Count * Stride);
            }
            else
            {
                for (uint32 Index = 0; Index < Buffer.Count; ++Index)
                {
                    Info.Relocate(Info, NewData + (size_t)Index * Stride, At(Buffer, Index));
                }
            }
        }

        if (Buffer.Data != nullptr)
        {
            void* Old = Buffer.Data;
            Memory::Free(Old);
        }
        Buffer.Data     = NewData;
        Buffer.Capacity = NewCapacity;
    }

    void FEventQueue::CopyInto(FBuffer& Buffer, const uint8* Source, uint32 Count)
    {
        Reserve(Buffer, Buffer.Count + Count);
        if (Info.bTriviallyCopyable)
        {
            Memory::Memcpy(At(Buffer, Buffer.Count), Source, (size_t)Count * Stride);
        }
        else
        {
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                Info.CopyConstruct(Info, At(Buffer, Buffer.Count + Index), Source + (size_t)Index * Stride);
            }
        }
        Buffer.Count += Count;
    }

    void FEventQueue::RelocateInto(FBuffer& Buffer, FBuffer& Source)
    {
        Reserve(Buffer, Buffer.Count + Source.Count);
        if (Info.bTriviallyRelocatable)
        {
            Memory::Memcpy(At(Buffer, Buffer.Count), Source.Data, (size_t)Source.Count * Stride);
        }
        else
        {
            for (uint32 Index = 0; Index < Source.Count; ++Index)
            {
                Info.Relocate(Info, At(Buffer, Buffer.Count + Index), At(Source, Index));
            }
        }
        Buffer.Count += Source.Count;
        Source.Count = 0;
    }

    void FEventQueue::DestroyAll(FBuffer& Buffer)
    {
        if (!Info.bTriviallyDestructible && Info.Destruct != nullptr)
        {
            for (uint32 Index = 0; Index < Buffer.Count; ++Index)
            {
                Info.Destruct(Info, At(Buffer, Index));
            }
        }
        Buffer.Count = 0;
    }

    void FEventQueue::FreeBuffer(FBuffer& Buffer)
    {
        DestroyAll(Buffer);
        if (Buffer.Data != nullptr)
        {
            void* Old = Buffer.Data;
            Memory::Free(Old);
        }
        Buffer = FBuffer{};
    }

    void FEventQueue::Send(const void* Event)
    {
        SendMany(Event, 1);
    }

    void FEventQueue::SendMany(const void* Events, uint32 Count)
    {
        if (Count == 0)
        {
            return;
        }

        // A reload resized the event since the last Advance, so the caller's buffer no longer matches the stride.
        if (Math::Max(Info.Size, 1u) != Stride)
        {
            LOG_WARN("Dropped {} event(s) of type {} sent while its layout changed; the queue resumes next frame.", Count, Info.Name);
            return;
        }

        const uint32 Slot = Jobs::GetThreadSlot();
        ASSERT(Slot < static_cast<uint32>(Staging.Num()));
        CopyInto(Staging[Slot], static_cast<const uint8*>(Events), Count);
        bStaged.store(true, std::memory_order_release);
    }

    FEventRuns FEventQueue::Read(uint64& Cursor) const
    {
        FEventRuns Runs;
        Runs.Stride = Stride;
        if (Math::Max(Info.Size, 1u) != Stride)
        {
            return Runs;
        }

        auto Take = [&](const FBuffer& Buffer, FEventRun& Out)
        {
            const uint64 Start = Math::Max(Cursor, Buffer.FirstIndex);
            const uint64 End   = Buffer.FirstIndex + Buffer.Count;
            if (Start < End)
            {
                Out.Data  = At(Buffer, (uint32)(Start - Buffer.FirstIndex));
                Out.Count = (uint32)(End - Start);
            }
        };

        Take(Buffers[1 - Newer], Runs.First);
        Take(Buffers[Newer], Runs.First.Count == 0 ? Runs.First : Runs.Second);

        Cursor = NextIndex;
        return Runs;
    }

    uint32 FEventQueue::NumUnread(uint64 Cursor) const
    {
        if (Math::Max(Info.Size, 1u) != Stride)
        {
            return 0;
        }

        const uint64 Oldest = Buffers[1 - Newer].FirstIndex;
        return (uint32)(NextIndex - Math::Max(Cursor, Oldest));
    }

    bool FEventQueue::Commit(FRegistry& Registry)
    {
        if (!bStaged.exchange(false, std::memory_order_acq_rel))
        {
            return false;
        }

        FBuffer& Target = Buffers[Newer];
        const uint32 Before = Target.Count;
        for (FBuffer& Slot : Staging)
        {
            if (Slot.Count != 0)
            {
                RelocateInto(Target, Slot);
            }
        }

        const uint32 Added = Target.Count - Before;
        if (Added == 0)
        {
            return false;
        }
        NextIndex += Added;

        // Sends a handler makes land in staging, so the run handed over here cannot move under it.
        if (Info.EventFanOut != nullptr)
        {
            Info.EventFanOut(Registry, At(Target, Before), Added);
        }
        return true;
    }

    void FEventQueue::RefreshLayout()
    {
        const uint32 Wanted = Math::Max(Info.Size, 1u);
        if (Wanted == Stride)
        {
            return;
        }

        FreeBuffer(Buffers[0]);
        FreeBuffer(Buffers[1]);
        for (FBuffer& Slot : Staging)
        {
            FreeBuffer(Slot);
        }
        Buffers[0].FirstIndex = NextIndex;
        Buffers[1].FirstIndex = NextIndex;
        Stride = Wanted;
    }

    void FEventQueue::Advance()
    {
        RefreshLayout();

        FBuffer& Older = Buffers[1 - Newer];
        DestroyAll(Older);
        Older.FirstIndex = NextIndex;
        Newer = 1 - Newer;
    }

    void FEventQueue::Clear()
    {
        DestroyAll(Buffers[0]);
        DestroyAll(Buffers[1]);
        for (FBuffer& Slot : Staging)
        {
            DestroyAll(Slot);
        }
        Buffers[0].FirstIndex = NextIndex;
        Buffers[1].FirstIndex = NextIndex;
        bStaged.store(false, std::memory_order_release);
    }

    FEventQueues::FEventQueues(FRegistry& InRegistry)
        : Registry(InRegistry)
    {
    }

    FEventQueues::~FEventQueues() = default;

    FEventQueue& FEventQueues::Get(FComponentTypeID TypeID)
    {
        ASSERT(TypeID < MaxComponentTypes);
        if (FEventQueue* Queue = ByTypeID[TypeID].load(std::memory_order_acquire))
        {
            return *Queue;
        }

        FScopeLock Lock(CreateLock);
        if (FEventQueue* Queue = ByTypeID[TypeID].load(std::memory_order_acquire))
        {
            return *Queue;
        }

        FEventQueue* Created = Owned.EmplaceGetRef(MakeUnique<FEventQueue>(TypeID)).get();
        ByTypeID[TypeID].store(Created, std::memory_order_release);
        return *Created;
    }

    FEventQueue* FEventQueues::Find(FComponentTypeID TypeID) const
    {
        return TypeID < MaxComponentTypes ? ByTypeID[TypeID].load(std::memory_order_acquire) : nullptr;
    }

    bool FEventQueues::Commit()
    {
        bool bAnyMoved = false;
        for (int32 Round = 0; Round < MaxCommitRounds; ++Round)
        {
            // Snapshotted, since a fan-out handler may create a queue of a type nothing had named yet.
            {
                FScopeLock Lock(CreateLock);
                CommitScratch.Reset();
                for (const TUniquePtr<FEventQueue>& Queue : Owned)
                {
                    CommitScratch.Add(Queue.get());
                }
            }

            bool bMoved = false;
            for (FEventQueue* Queue : CommitScratch)
            {
                bMoved |= Queue->Commit(Registry);
            }

            if (!bMoved)
            {
                return bAnyMoved;
            }
            bAnyMoved = true;
        }

        LOG_WARN("Event fan-out kept sending for {} rounds at one sync point, so the rest waits for the next.", MaxCommitRounds);
        return bAnyMoved;
    }

    void FEventQueues::Advance()
    {
        FScopeLock Lock(CreateLock);
        for (const TUniquePtr<FEventQueue>& Queue : Owned)
        {
            Queue->Advance();
        }
    }

    void FEventQueues::AddInterest(FComponentTypeID TypeID)
    {
        if (TypeID < MaxComponentTypes)
        {
            Interest[TypeID].store(true, std::memory_order_relaxed);
        }
    }

    bool FEventQueues::HasInterest(FComponentTypeID TypeID) const
    {
        return TypeID < MaxComponentTypes && Interest[TypeID].load(std::memory_order_relaxed);
    }

    void FEventQueues::Clear()
    {
        FScopeLock Lock(CreateLock);
        for (const TUniquePtr<FEventQueue>& Queue : Owned)
        {
            Queue->Clear();
        }
    }
}
