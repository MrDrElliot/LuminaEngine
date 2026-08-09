#pragma once
#include "Core/Threading/Thread.h"


namespace Lumina
{
    template<typename T>
    struct THandle
    {
        uint64 Handle = 0;
    };
    
    template<typename T>
    class TSegmentMap
    {
        using HandleT       = THandle<T>;
        using FDtorFn       = void(*)(T*);
    
    public:
        
        TSegmentMap() = default;
        TSegmentMap(FDtorFn Fn): DtorFn(Fn) {}
        
        void SetDtor(FDtorFn Fn)
        {
            DtorFn = Fn;
        }
        
        // Emplace/Erase mutate the shared free list, so they serialize on Mutex; reads (operator[])
        // stay lock-free because Segments[] is fixed-size and live entries never move.
        template<typename... TArgs>
        HandleT Emplace(TArgs&&... Value)
        {
            FScopeLock Lock(Mutex);

            if (Head == kEndOfList)
            {
                AddSegment();
            }

            uint32 Index = Head;
            DEBUG_ASSERT(Index != kNotInFreeList && Index != kEndOfList);

            FEntry* Entry = Get(Index);

            Head = Entry->Next;
            Entry->Next = kNotInFreeList;

            ::new(&Entry->Data) T(eastl::forward<TArgs>(Value)...);

            return ToHandle(Index, ++Entry->Gen);
        }

        void Erase(HandleT Handle)
        {
            auto&& [I, G] = FromHandle(Handle);
            FEntry* Entry = Get(I);

            // Cold path, so these are worth paying for here even though operator[] cannot afford
            // them. Erasing twice is the dangerous one: it pushes the slot onto the free list a
            // second time, so two later Emplaces hand out the same index and two "distinct"
            // resources alias one another. That corruption surfaces arbitrarily far away.
            DEBUG_ASSERT(Entry->Next == kNotInFreeList, "Erase of a handle that is already free (double destroy).");

            // Catches erasing a stale handle whose slot has since been recycled, which the check
            // above cannot see because the slot is legitimately live again.
            DEBUG_ASSERT(Entry->Gen == G, "Erase of a stale handle; slot was already recycled.");

            // Destruct outside the lock: the entry isn't on the free list yet (nothing can grab it),
            // and DtorFn may take other RHI locks -- holding Mutex across it would couple lock orders.
            DtorFn(&Entry->Data);

            FScopeLock Lock(Mutex);

            // Bumped on release, not just on acquire, so every handle to this slot goes stale the
            // moment it is freed rather than staying valid-looking until someone reuses it.
            ++Entry->Gen;

            Entry->Next = Head;
            Head = I;
        }
        
        void Clear()
        {
            for (uint32 SegmentIndex = 0; SegmentIndex < UsedSegments; ++SegmentIndex)
            {
                uint32 SegmentSize = SlotsInSegments(SegmentIndex);
                FEntry* Segment = Segments[SegmentIndex];
                
                for (uint32 Index = 0; Index < SegmentSize; ++Index)
                {
                    if (Segment[Index].Next == kNotInFreeList)
                    {
                        DtorFn(&Segment[Index].Data);
                    }
                }
                
                Memory::Free(Segment);
                Segments[SegmentIndex] = nullptr;
            }

            UsedSegments = 0;

            // Head still indexed into the segments that were just freed. Emplace only calls
            // AddSegment when Head == kEndOfList, so without this reset the next Emplace after a
            // Clear skips allocation entirely and dereferences Segments[n] -- now nullptr. Today
            // Clear only runs at device teardown, which is the only reason this has not fired.
            Head = kEndOfList;
        }
        
        T& operator[](HandleT Handle)
        {
            auto&& [I, G] = FromHandle(Handle);
            FEntry* Entry = Get(I);
            return Entry->Data;
        }
        
        const T& operator[](HandleT Handle) const
        {
            auto&& [I, G] = FromHandle(Handle);
            FEntry* Entry = Get(I);
            return Entry->Data;
        }
        
        
    private:
        
        static constexpr auto kSmallSegmentsToSkip = 6;
        static constexpr auto kNotInFreeList = UINT32_MAX;
        static constexpr auto kEndOfList = kNotInFreeList - 1;
        
        struct FEntry
        {
            T Data;
            uint32 Next;
            uint32 Gen;
        };
        
        struct FDecomposedHandle
        {
            uint32 Index;
            uint32 Gen;
        };
        
        static constexpr uint32 SlotsInSegments(uint32 SegmentIndex)
        {
            return (1 << kSmallSegmentsToSkip) << SegmentIndex;
        }
        
        static constexpr uint32 CapacityForSegmentCount(uint32 SegmentCount)
        {
            return ((1 << kSmallSegmentsToSkip) << SegmentCount) - (1 << kSmallSegmentsToSkip);
        }
        
        void AddSegment()
        {
            DEBUG_ASSERT(UsedSegments < 26, "TSegmentMap segment table exhausted.");

            uint64 SegmentSize = SlotsInSegments(UsedSegments);
            auto* Entry = Memory::Malloc(sizeof(FEntry) * SegmentSize);
            auto* Segment = static_cast<FEntry*>(Entry);

            Segments[UsedSegments++] = Segment;

            uint32 SegmentOffset = CapacityForSegmentCount(UsedSegments - 1);
            for (uint64 i = SegmentSize; i > 0; --i)
            {
                Segment[i - 1].Gen      = 0;
                Segment[i - 1].Next     = Head;

                // The slot just linked is (i - 1), not i. Publishing i skipped every even slot in
                // the segment: the free list ran 1, 3, 5, ... so half of every segment was
                // unreachable and the map allocated a new segment twice as often as it needed to.
                Head                    = static_cast<uint32>(i - 1) + SegmentOffset;
            }
        }
        
        FEntry* Get(uint32 Index)
        {
            uint64 Segment = 63 - std::countl_zero(static_cast<uint64>((Index >> kSmallSegmentsToSkip) + 1));
            uint32 Slot = Index - CapacityForSegmentCount(Segment);

            return &Segments[Segment][Slot];
        }

        const FEntry* Get(uint32 Index) const
        {
            uint64 Segment = 63 - std::countl_zero(static_cast<uint64>((Index >> kSmallSegmentsToSkip) + 1));
            uint32 Slot = Index - CapacityForSegmentCount(Segment);

            return &Segments[Segment][Slot];
        }
        
        static constexpr HandleT ToHandle(uint32 Index, uint32 Generation)
        {
            // Tag bit is 0x8000'0000 before the shift, not after. Or-ing it into a 64-bit value and
            // then shifting left by 32 pushed it straight off the top, so the tag never survived and
            // handles were non-zero only because Generation starts at 1. RHI::IsValid tests
            // Handle != 0, so a slot whose generation ever wrapped to 0 would read as invalid.
            return {.Handle = (0x8000'0000ull | (uint64)Generation) << 32ull | Index};
        }
        
        static constexpr FDecomposedHandle FromHandle(HandleT Handle)
        {
            return 
            {
                .Index  = static_cast<uint32>(Handle.Handle & 0xFFFF'FFFFull),
                .Gen    = static_cast<uint32>((Handle.Handle >> 32) & 0x7FFF'FFFFull)
            };
        }
        
    private:
        
        FDtorFn     DtorFn          = nullptr;
        uint32      UsedSegments    = 0;
        uint32      Head            = kEndOfList;
        FEntry*     Segments[26]    {nullptr};
        FMutex      Mutex;
    };
    
}