#pragma once

#include "Memory/Memory.h"

namespace Lumina::Jobs
{
    // Coroutine frames bump-allocated and freed last-in first-out, in chained blocks that grow geometrically.
    class FStack
    {
    public:

        static constexpr std::size_t kAlignment      = 16;
        static constexpr std::size_t kFirstBlockSize = 64 * 1024;
        static constexpr std::size_t kMaxBlockSize   = 1024 * 1024;

        FStack() = default;
        FStack(const FStack&) = delete;
        FStack& operator=(const FStack&) = delete;
        ~FStack() { Release(); }

        NODISCARD FORCEINLINE void* Allocate(std::size_t Size)
        {
            Size = RoundUp(Size);
            if (Current != nullptr && static_cast<std::size_t>(Current->End - Current->Top) >= Size)
            {
                std::byte* Result = Current->Top;
                Current->Top += Size;
                return Result;
            }
            return AllocateSlow(Size);
        }

        FORCEINLINE void Free(void*, std::size_t Size)
        {
            Current->Top -= RoundUp(Size);
            if (Current->Top == Current->Begin() && Current->Prev != nullptr)
            {
                PopBlock();
            }
        }

        NODISCARD FORCEINLINE bool IsEmpty() const
        {
            return Current == nullptr || (Current->Top == Current->Begin() && Current->Prev == nullptr);
        }

        void Release()
        {
            FreeChain(Current);
            FreeChain(Spare);
            Current = nullptr;
            Spare   = nullptr;
        }

        FStack* NextFree = nullptr;

    private:

        struct FBlock
        {
            FBlock*    Prev = nullptr;
            std::byte* Top  = nullptr;
            std::byte* End  = nullptr;
            std::size_t Size = 0;

            FORCEINLINE std::byte* Begin() { return reinterpret_cast<std::byte*>(this) + RoundUp(sizeof(FBlock)); }
        };

        static constexpr std::size_t RoundUp(std::size_t Size) { return (Size + kAlignment - 1) & ~(kAlignment - 1); }

        void* AllocateSlow(std::size_t Size)
        {
            FBlock* Block = nullptr;
            const std::size_t Needed = Size + RoundUp(sizeof(FBlock));
            if (Spare != nullptr && Spare->Size >= Needed)
            {
                Block = Spare;
                Spare = nullptr;
            }
            else
            {
                std::size_t BlockSize = Current != nullptr ? Current->Size * 2 : kFirstBlockSize;
                BlockSize = BlockSize > kMaxBlockSize ? kMaxBlockSize : BlockSize;
                BlockSize = BlockSize < Needed ? Needed : BlockSize;
                Block = static_cast<FBlock*>(Memory::Malloc(BlockSize, kAlignment));
                Block->Size = BlockSize;
                Block->End  = reinterpret_cast<std::byte*>(Block) + BlockSize;
            }
            Block->Prev = Current;
            Block->Top  = Block->Begin() + Size;
            Current = Block;
            return Block->Begin();
        }

        void PopBlock()
        {
            FBlock* Empty = Current;
            Current = Empty->Prev;
            if (Spare == nullptr || Spare->Size < Empty->Size)
            {
                FreeChain(Spare);
                Spare = Empty;
                Spare->Prev = nullptr;
            }
            else
            {
                void* Raw = Empty;
                Memory::Free(Raw);
            }
        }

        static void FreeChain(FBlock* Block)
        {
            while (Block != nullptr)
            {
                FBlock* Prev = Block->Prev;
                void* Raw = Block;
                Memory::Free(Raw);
                Block = Prev;
            }
        }

        FBlock* Current = nullptr;
        FBlock* Spare   = nullptr;
    };
}
