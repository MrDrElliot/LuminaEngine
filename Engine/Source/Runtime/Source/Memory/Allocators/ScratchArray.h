#pragma once

#include "Containers/Array.h"
#include "Core/Assertions/Assert.h"
#include "Memory/Memory.h"
#include "Platform/GenericPlatform.h"
#include <type_traits>

namespace Lumina
{
    namespace ScratchPool
    {
        struct FStats
        {
            SIZE_T RetainedBytes  = 0;
            SIZE_T RetainedBlocks = 0;
            SIZE_T LiveBytes      = 0;
            SIZE_T LiveBlocks     = 0;
        };

        // Uninitialized 64-byte-aligned block of at least Size bytes; OutCapacity is what Release must be given.
        RUNTIME_API void* Acquire(SIZE_T Size, SIZE_T& OutCapacity);

        RUNTIME_API void Release(void* Block, SIZE_T Capacity);

        // Frees every retained block; live blocks are untouched.
        RUNTIME_API void Trim();

        RUNTIME_API FStats GetStats();
    }

    // Pooled uninitialized bulk storage owned by the object, so it survives fiber migration and frame resets.
    template<typename T>
    class TScratchArray
    {
        static_assert(std::is_trivially_copyable_v<T>, "TScratchArray never constructs its elements");
        static_assert(std::is_trivially_destructible_v<T>, "TScratchArray never destroys its elements");

    public:

        TScratchArray() = default;

        explicit TScratchArray(SIZE_T InCount) { Reset(InCount); }

        ~TScratchArray() { ReleaseBlock(); }

        TScratchArray(const TScratchArray&)            = delete;
        TScratchArray& operator=(const TScratchArray&) = delete;

        TScratchArray(TScratchArray&& Other) noexcept
            : Data(Other.Data)
            , Count(Other.Count)
            , Capacity(Other.Capacity)
        {
            Other.Data     = nullptr;
            Other.Count    = 0;
            Other.Capacity = 0;
        }

        TScratchArray& operator=(TScratchArray&& Other) noexcept
        {
            if (this != &Other)
            {
                ReleaseBlock();

                Data     = Other.Data;
                Count    = Other.Count;
                Capacity = Other.Capacity;

                Other.Data     = nullptr;
                Other.Count    = 0;
                Other.Capacity = 0;
            }
            return *this;
        }

        // Sizes without initializing, and keeps the current block when it already fits.
        void Reset(SIZE_T NewCount)
        {
            const SIZE_T Bytes = NewCount * sizeof(T);
            ASSERT(NewCount == 0 || Bytes / NewCount == sizeof(T));

            if (Bytes > Capacity)
            {
                ReleaseBlock();

                if (Bytes > 0)
                {
                    Data = static_cast<T*>(ScratchPool::Acquire(Bytes, Capacity));
                }
            }

            Count = NewCount;
        }

        void Fill(const T& Value)
        {
            for (SIZE_T Index = 0; Index < Count; ++Index)
            {
                Data[Index] = Value;
            }
        }

        FORCEINLINE T*       GetData()       { return Data; }
        FORCEINLINE const T* GetData() const { return Data; }
        FORCEINLINE SIZE_T   Num() const     { return Count; }
        FORCEINLINE bool     IsEmpty() const { return Count == 0; }

        FORCEINLINE T&       operator[](SIZE_T Index)       { ASSERT(Index < Count); return Data[Index]; }
        FORCEINLINE const T& operator[](SIZE_T Index) const { ASSERT(Index < Count); return Data[Index]; }

        FORCEINLINE T*       begin()       { return Data; }
        FORCEINLINE T*       end()         { return Data + Count; }
        FORCEINLINE const T* begin() const { return Data; }
        FORCEINLINE const T* end()   const { return Data + Count; }

        FORCEINLINE TSpan<T>       AsSpan()       { return TSpan<T>(Data, Count); }
        FORCEINLINE TSpan<const T> AsSpan() const { return TSpan<const T>(Data, Count); }

    private:

        void ReleaseBlock()
        {
            if (Data != nullptr)
            {
                ScratchPool::Release(Data, Capacity);
                Data = nullptr;
            }

            Count    = 0;
            Capacity = 0;
        }

        T*     Data     = nullptr;
        SIZE_T Count    = 0;
        SIZE_T Capacity = 0;
    };
}
