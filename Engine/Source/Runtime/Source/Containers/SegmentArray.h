#pragma once
#include <bit>

#include "Core/Assertions/Assert.h"
#include "Core/Threading/Thread.h"
#include "Memory/Memory.h"
#include "Memory/Construct.h"


namespace Lumina
{
    template<typename T>
    struct THandle
    {
        uint64 Handle = 0;
        
        bool operator == (const THandle& Other) const { return Handle == Other.Handle; }
        bool operator != (const THandle& Other) const { return Handle != Other.Handle; }
        
        explicit operator bool () const { return Handle != 0; }
        bool operator == (decltype(nullptr)) const { return Handle == 0; }
        bool operator != (decltype(nullptr)) const { return Handle != 0; }
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

            Memory::ConstructAt(&Entry->Data, std::forward<TArgs>(Value)...);

            return ToHandle(Index, ++Entry->Gen);
        }

        void Erase(HandleT Handle)
        {
            auto&& [I, G] = FromHandle(Handle);
            FEntry* Entry = Get(I);
            
            DEBUG_ASSERT(Entry->Next == kNotInFreeList, "Erase of a handle that is already free (double destroy).");
            
            DEBUG_ASSERT(Entry->Gen == G, "Erase of a stale handle; slot was already recycled.");
            
            DtorFn(&Entry->Data);

            FScopeLock Lock(Mutex);
            
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
            
            Head = kEndOfList;
        }
        
        template<typename TFn>
        void ForEachLive(TFn&& Fn)
        {
            for (uint32 SegmentIndex = 0; SegmentIndex < UsedSegments; ++SegmentIndex)
            {
                const uint32 SegmentSize = SlotsInSegments(SegmentIndex);
                FEntry* Segment = Segments[SegmentIndex];

                for (uint32 Index = 0; Index < SegmentSize; ++Index)
                {
                    if (Segment[Index].Next == kNotInFreeList)
                    {
                        Fn(Segment[Index].Data);
                    }
                }
            }
        }

        T* TryGet(HandleT Handle)
        {
            const FEntry* Entry = FindLive(Handle);
            return Entry != nullptr ? const_cast<T*>(&Entry->Data) : nullptr;
        }

        const T* TryGet(HandleT Handle) const
        {
            const FEntry* Entry = FindLive(Handle);
            return Entry != nullptr ? &Entry->Data : nullptr;
        }

        bool IsLive(HandleT Handle) const { return FindLive(Handle) != nullptr; }

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


                Head                    = static_cast<uint32>(i - 1) + SegmentOffset;
            }
        }
        
        // The one place the three failure modes are checked, so TryGet/IsLive cannot drift apart.
        const FEntry* FindLive(HandleT Handle) const
        {
            if (Handle.Handle == 0)
            {
                return nullptr;
            }

            auto&& [I, G] = FromHandle(Handle);
            
            if (I >= CapacityForSegmentCount(UsedSegments))
            {
                return nullptr;
            }

            const FEntry* Entry = Get(I);
            
            if (Entry->Next != kNotInFreeList || Entry->Gen != G)
            {
                return nullptr;
            }

            return Entry;
        }

        FEntry* Get(uint32 Index)
        {
            uint64 Segment = 63 - std::countl_zero(static_cast<uint64>((Index >> kSmallSegmentsToSkip) + 1));
            uint32 Slot = Index - CapacityForSegmentCount((uint32)Segment);

            return &Segments[Segment][Slot];
        }

        const FEntry* Get(uint32 Index) const
        {
            uint64 Segment = 63 - std::countl_zero(static_cast<uint64>((Index >> kSmallSegmentsToSkip) + 1));
            uint32 Slot = Index - CapacityForSegmentCount((uint32)Segment);

            return &Segments[Segment][Slot];
        }
        
        static constexpr HandleT ToHandle(uint32 Index, uint32 Generation)
        {
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
