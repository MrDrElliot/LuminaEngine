#pragma once
#include <atomic>
#include <bit>

#include "ContainerTraits.h"
#include "Core/Assertions/Assert.h"
#include "Core/Threading/Thread.h"
#include "Memory/Memory.h"
#include "Memory/Construct.h"
#include "Lumina.h"


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
        ~TSegmentMap() { Clear(); }
        
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
            const uint32 Used = UsedSegments.load(std::memory_order_relaxed);
            for (uint32 SegmentIndex = 0; SegmentIndex < Used; ++SegmentIndex)
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

            UsedSegments.store(0, std::memory_order_relaxed);
            
            Head = kEndOfList;
        }
        
        template<typename TFn>
        void ForEachLive(TFn&& Fn)
        {
            const uint32 Used = UsedSegments.load(std::memory_order_acquire);
            for (uint32 SegmentIndex = 0; SegmentIndex < Used; ++SegmentIndex)
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

        template <typename TSelf>
        auto TryGet(this TSelf&& Self, HandleT Handle) -> TLikeSelf<TSelf, T>*
        {
            const FEntry* Entry = Self.FindLive(Handle);
            return Entry != nullptr ? const_cast<TLikeSelf<TSelf, T>*>(&Entry->Data) : nullptr;
        }

        bool IsLive(HandleT Handle) const { return FindLive(Handle) != nullptr; }

        template <typename TSelf>
        auto operator[](this TSelf&& Self, HandleT Handle) -> TLikeSelf<TSelf, T>&
        {
            return Self.Get(FromHandle(Handle).Index)->Data;
        }
        
        
    private:
        
        static constexpr auto kSmallSegmentsToSkip = 6;
        static constexpr uint32 kNotInFreeList = Constants::kIndexNoneU32;
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
        
        // Published with release, so a reader that sees the new count also sees the segment pointer behind it.
        void AddSegment()
        {
            const uint32 Used = UsedSegments.load(std::memory_order_relaxed);
            DEBUG_ASSERT(Used < 26, "TSegmentMap segment table exhausted.");

            uint64 SegmentSize = SlotsInSegments(Used);
            auto* Entry = Memory::Malloc(sizeof(FEntry) * SegmentSize);
            auto* Segment = static_cast<FEntry*>(Entry);

            Segments[Used] = Segment;

            uint32 SegmentOffset = CapacityForSegmentCount(Used);
            for (uint64 i = SegmentSize; i > 0; --i)
            {
                Segment[i - 1].Gen      = 0;
                Segment[i - 1].Next     = Head;


                Head                    = static_cast<uint32>(i - 1) + SegmentOffset;
            }

            UsedSegments.store(Used + 1, std::memory_order_release);
        }
        
        // The one place the three failure modes are checked, so TryGet/IsLive cannot drift apart.
        const FEntry* FindLive(HandleT Handle) const
        {
            if (Handle.Handle == 0)
            {
                return nullptr;
            }

            auto&& [I, G] = FromHandle(Handle);
            
            if (I >= CapacityForSegmentCount(UsedSegments.load(std::memory_order_acquire)))
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

        template <typename TSelf>
        auto Get(this TSelf&& Self, uint32 Index) -> TLikeSelf<TSelf, FEntry>*
        {
            const uint64 Segment = 63 - std::countl_zero(static_cast<uint64>((Index >> kSmallSegmentsToSkip) + 1));
            const uint32 Slot = Index - CapacityForSegmentCount(static_cast<uint32>(Segment));

            return &Self.Segments[Segment][Slot];
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
        
        static void DestroyEntry(T* Value) { Memory::DestroyAt(Value); }

    private:
        
        FDtorFn                 DtorFn          = &DestroyEntry;
        std::atomic<uint32>     UsedSegments    = 0;
        uint32                  Head            = kEndOfList;
        FEntry*     Segments[26]    {nullptr};
        FMutex      Mutex;
    };
    
}
