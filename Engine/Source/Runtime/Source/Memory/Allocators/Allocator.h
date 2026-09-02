#pragma once

#include "Core/Assertions/Assert.h"
#include "Memory/Construct.h"
#include "Memory/Memory.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"

namespace Lumina
{

    class RUNTIME_API IAllocator
    {
    public:

        virtual ~IAllocator() = default;
        
        template<typename T, typename... Args>
        T* TAlloc(Args&&... args)
        {
            void* Mem = Allocate(sizeof(T), alignof(T));
            return Memory::ConstructAt(static_cast<T*>(Mem), Forward<Args>(args)...);
        } 
        
        virtual void* Allocate(size_t Size, size_t Alignment = alignof(std::max_align_t)) = 0;
        virtual void Free(void* Data) = 0;
        virtual size_t GetCapacity() { return 0; }
        virtual void Reset() = 0;
    };

    class RUNTIME_API FDefaultAllocator : public IAllocator
    {
    public:
        
        
        void* Allocate(size_t Size, size_t Alignment) override
        {
            return Memory::Malloc(Size, Alignment);
        }
        
        void Free(void* Data) override
        {
            Memory::Free(Data);
        }
        
        void Reset() override { }
    };
    
    class RUNTIME_API FLinearAllocator : public IAllocator
    {
    public:
        
        explicit FLinearAllocator(size_t InCapacity)
        {
            Capacity = InCapacity;
            Base = (uint8*)Memory::Malloc(Capacity);
            Offset = 0;
        }

        ~FLinearAllocator() override
        {
            Memory::Free(Base);
            Base = nullptr;
        }
        
        void* Allocate(size_t Size, size_t Alignment) override
        {
            size_t CurrentPtr = reinterpret_cast<size_t>(Base + Offset);
            SIZE_T AlignedPtr = (CurrentPtr + Alignment - 1) & ~(Alignment - 1);
            SIZE_T NextOffset = AlignedPtr - reinterpret_cast<SIZE_T>(Base) + Size;
            ASSERT(NextOffset < Capacity);
            
            void* Result = Base + (AlignedPtr - reinterpret_cast<SIZE_T>(Base));
            Offset = NextOffset;
            return Result;
        }

        void Free(void* Data) override { }

        void Reset() override
        {
            Offset = 0;
        }

        SIZE_T GetCapacity() override { return Capacity; }
        SIZE_T GetUsed() const { return Offset; }

    private:
        
        uint8* Base = nullptr;
        SIZE_T Offset = 0;
        SIZE_T Capacity = 0;
    };

    
    class RUNTIME_API FBlockLinearAllocator : public IAllocator
    {
        struct Block;
    public:

        // Snapshot of the allocation cursor. Restore to free everything allocated since,
        // without touching earlier allocations. The basis for FMemMark-style scopes.
        struct FMark
        {
            Block*  MarkBlock  = nullptr;
            SIZE_T  MarkOffset = 0;
        };

        explicit FBlockLinearAllocator(const char* AllocatorName) noexcept
            :FBlockLinearAllocator()
        {}
        
        FBlockLinearAllocator() noexcept
            : BlockSize(1024)
            , CurrentOffset(0)
            , BlockCount(0)
        { 
            AllocateNewBlock();
        } 
        
        explicit FBlockLinearAllocator(SIZE_T InBlockSize) 
            : BlockSize(InBlockSize)
            , CurrentOffset(0)
            , BlockCount(0)
        { 
            AllocateNewBlock();
        } 
    
        ~FBlockLinearAllocator() override 
        { 
            Block* Current = FirstBlock;
            while (Current)
            {
                Block* Next = Current->Next;
                Memory::Free(Current);
                Current = Next;
            }
        }

        void* allocate(size_t n, int flags = 0)
        {
            return Allocate(n, DEFAULT_ALIGNMENT);
        }
        
        void* allocate(size_t n, size_t alignment, size_t offset, int flags = 0)
        {
            return Allocate(n, alignment);
        }
        
        void deallocate(void* p, size_t n)
        {
        }
         
        void* Allocate(SIZE_T Size, SIZE_T Alignment) override
        {
            // An oversized request cannot fit any block;
            if (Size >= GetUsableBlockSize()) [[unlikely]]
            {
                PANIC("FBlockLinearAllocator: {}-byte allocation exceeds usable block size {}", Size, GetUsableBlockSize());
            }

            SIZE_T CurrentPtr = reinterpret_cast<SIZE_T>(CurrentBlock->GetData() + CurrentOffset);
            SIZE_T AlignedPtr = (CurrentPtr + Alignment - 1) & ~(Alignment - 1);
            SIZE_T NextOffset = AlignedPtr - reinterpret_cast<SIZE_T>(CurrentBlock->GetData()) + Size;

            if (NextOffset > GetUsableBlockSize())
            {
                AllocateNewBlock();
                CurrentOffset = 0;

                CurrentPtr = reinterpret_cast<SIZE_T>(CurrentBlock->GetData() + CurrentOffset);
                AlignedPtr = (CurrentPtr + Alignment - 1) & ~(Alignment - 1);
                NextOffset = AlignedPtr - reinterpret_cast<SIZE_T>(CurrentBlock->GetData()) + Size;

                ASSERT(NextOffset <= GetUsableBlockSize());
            }

            void* Result = CurrentBlock->GetData() + (AlignedPtr - reinterpret_cast<SIZE_T>(CurrentBlock->GetData()));
            CurrentOffset = NextOffset;
            return Result;
        }
    
        void Free(void* Data) override { }
    
        void Reset() override
        {
            CurrentBlock = FirstBlock;
            CurrentOffset = 0;
        }

        // Capture the current cursor; pair with RestoreToMark for scoped (pop-off) reuse.
        FMark GetMark() const
        {
            return { CurrentBlock, CurrentOffset };
        }

        // Rewind the cursor to a previously captured mark. Blocks past the mark stay
        // allocated and are reused by later Allocate calls (no free, no destructors).
        void RestoreToMark(const FMark& Mark)
        {
            CurrentBlock  = Mark.MarkBlock ? Mark.MarkBlock : FirstBlock;
            CurrentOffset = Mark.MarkOffset;
        }

        /** Frees all blocks except the first, then resets. */
        void Compact()
        {
            if (!FirstBlock)
            {
                return;
            }

            Block* Current = FirstBlock->Next;
            while (Current)
            {
                Block* Next = Current->Next;
                Memory::Free(Current);
                Current = Next;
                BlockCount--;
            }

            FirstBlock->Next = nullptr;
            CurrentBlock = FirstBlock;
            CurrentOffset = 0;
        }
    
        SIZE_T GetCapacity() override 
        { 
            return BlockCount * BlockSize; 
        }
        
        SIZE_T GetUsed() const 
        { 
            SIZE_T Used = 0;

            Block* Current = FirstBlock;
            while (Current != CurrentBlock && Current != nullptr)
            {
                Used += GetUsableBlockSize();
                Current = Current->Next;
            }

            if (CurrentBlock)
            {
                Used += CurrentOffset;
            }
            
            return Used;
        }
        
        SIZE_T GetBlockCount() const 
        { 
            return BlockCount; 
        }
    
    private:
        
        struct Block
        {
            Block* Next;
            
            uint8* GetData() { return reinterpret_cast<uint8*>(this + 1); }
            const uint8* GetData() const { return reinterpret_cast<const uint8*>(this + 1); }
        };
        
        SIZE_T GetUsableBlockSize() const
        {
            return BlockSize - sizeof(Block);
        }
        
        void AllocateNewBlock()
        {
            // Reuse a chained block from before Reset(); allocate fresh only at tail.
            if (CurrentBlock && CurrentBlock->Next)
            {
                CurrentBlock = CurrentBlock->Next;
                return;
            }

            Block* NewBlock = (Block*)Memory::Malloc(BlockSize);
            ASSERT(NewBlock != nullptr);

            NewBlock->Next = nullptr;

            if (CurrentBlock)
            {
                CurrentBlock->Next = NewBlock;
            }
            else
            {
                FirstBlock = NewBlock;
            }

            CurrentBlock = NewBlock;
            BlockCount++;
        }
        
        Block* FirstBlock = nullptr;
        Block* CurrentBlock = nullptr;
        SIZE_T BlockSize;
        SIZE_T CurrentOffset;
        SIZE_T BlockCount;
    };


    // Frame-arena-backed containers. Arena must outlive the container and is bulk-reset (no per-item free).
    template <typename T>
    using TFrameVector = Containers::TVector<T, 0, FFrameAllocator>;

    template <typename K, typename V>
    using TFrameHashMap = Containers::THashMap<K, V, Lumina::Containers::FDefaultHash, Containers::FDefaultEqual, FFrameAllocator>;


    // Per-thread scratch stack; reclaimed only by FMemMark scopes (or thread exit), never per-allocation.
    // Use it through FMemMark, not directly.
    RUNTIME_API FBlockLinearAllocator& GetThreadScratchAllocator();

    // Per-thread frame arena: a bump allocator that lives for the whole frame and is bulk-reset at the
    // frame boundary by ResetThreadFrameAllocators() (NOT per FMemMark scope). Use for per-thread scratch
    // whose data must outlive a parallel-for and be consumed later in the same frame (gather -> merge).
    // Registered globally on first touch so the boundary reset can reclaim it; grows to the per-thread
    // high-water mark and is reused every frame, so a parallel pass never reallocates its own arena pool.
    RUNTIME_API FBlockLinearAllocator& GetThreadFrameAllocator();

    // Reset every registered thread frame arena (rewind to empty, keep blocks for reuse). MUST run at a
    // quiescent frame boundary with no tasks touching frame arenas, i.e. engine frame-begin. Registry walk.
    RUNTIME_API void ResetThreadFrameAllocators();

    // RAII bump-allocator scope: mark on construction, restore on destruction (O(1) bulk free).
    // No destructors run on exit, so store only trivially destructible data. Nested marks compose (LIFO).
    class FMemMark
    {
    public:

        FMemMark() noexcept
            : Arena(GetThreadScratchAllocator())
            , Mark(Arena.GetMark())
        {}

        explicit FMemMark(FBlockLinearAllocator& InArena) noexcept
            : Arena(InArena)
            , Mark(InArena.GetMark())
        {}

        ~FMemMark() { Arena.RestoreToMark(Mark); }

        FMemMark(const FMemMark&)            = delete;
        FMemMark& operator=(const FMemMark&) = delete;

        void* Allocate(SIZE_T Size, SIZE_T Alignment = 16) { return Arena.Allocate(Size, Alignment); }

        template<typename T, typename... TArgs>
        T* Alloc(TArgs&&... Args) { return Memory::ConstructAt(static_cast<T*>(Arena.Allocate(sizeof(T), alignof(T))), Forward<TArgs>(Args)...); }

        template<typename T, typename F>
        requires(!std::is_trivially_constructible_v<T>)
        T* AllocArray(size_t N, F&& f)
        {
            void* Mem = Arena.Allocate(sizeof(T) * N, alignof(T));
            T* Ptr = static_cast<T*>(Mem);
            
            for (size_t i = 0; i < N; ++i)
            {
                Memory::ConstructAt(Ptr + i, f(i));
            }   

            return Ptr;
        }
        
        template<typename T>
        requires(std::is_trivially_constructible_v<T>)
        T* AllocArray(size_t N)
        {
            void* Mem = Arena.Allocate(sizeof(T) * N, alignof(T));
            std::memset(Mem, 0, sizeof(T) * N);
            return static_cast<T*>(Mem);
        }
        
        FBlockLinearAllocator& GetAllocator() const { return Arena; }

    private:

        FBlockLinearAllocator&   Arena;
        FBlockLinearAllocator::FMark Mark;
    };

    // Containers backed by the calling thread's scratch arena, reclaimed by the enclosing FMemMark.
    template <typename T>
    using TScratchVector = Containers::TVector<T, 0, FScratchAllocator>;

    template <typename K, typename V>
    using TScratchHashMap = Containers::THashMap<K, V, Lumina::Containers::FDefaultHash, Containers::FDefaultEqual, FScratchAllocator>;
}
