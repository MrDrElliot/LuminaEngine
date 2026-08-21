#pragma once

#include <bit>
#include <cstring>

#include "ContainerTraits.h"

namespace Lumina::Containers
{
    /** Fixed-width bit array packed into 64-bit words, with the width in the type. */
    template <size_t N>
    class TBitSet
    {
        static constexpr size_t kBitsPerWord = 64;
        static constexpr size_t kWordCount   = (N + kBitsPerWord - 1) / kBitsPerWord;
        static constexpr size_t kTailBits    = N % kBitsPerWord;

        static constexpr uint64 kTailMask = kTailBits == 0
            ? ~static_cast<uint64>(0)
            : ((static_cast<uint64>(1) << kTailBits) - 1);

    public:

        /** Writes back through the owning set, so Bits[3] = true does what it reads like. */
        class FReference
        {
            friend class TBitSet;

        public:

            FReference& operator=(bool Value) noexcept
            {
                Owner->Set(Index, Value);
                return *this;
            }

            FReference& operator=(const FReference& Other) noexcept
            {
                Owner->Set(Index, static_cast<bool>(Other));
                return *this;
            }

            NODISCARD operator bool() const noexcept { return Owner->Test(Index); }
            NODISCARD bool operator~() const noexcept { return !Owner->Test(Index); }

            FReference& Flip() noexcept
            {
                Owner->Flip(Index);
                return *this;
            }

        private:

            FReference(TBitSet* InOwner, size_t InIndex) noexcept : Owner(InOwner), Index(InIndex) {}

            TBitSet* Owner;
            size_t   Index;
        };

        constexpr TBitSet() noexcept : Words{} {}

        explicit TBitSet(uint64 Value) noexcept : Words{}
        {
            Words[0] = Value;
            Trim();
        }

        NODISCARD static constexpr size_t size() noexcept { return N; }
        NODISCARD static constexpr size_t Num() noexcept { return N; }

        NODISCARD FORCEINLINE bool Test(size_t Index) const noexcept
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, N);
            return (Words[Index / kBitsPerWord] & Bit(Index)) != 0;
        }

        NODISCARD FORCEINLINE bool test(size_t Index) const noexcept { return Test(Index); }

        FORCEINLINE TBitSet& Set(size_t Index, bool Value = true) noexcept
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, N);
            if (Value)
            {
                Words[Index / kBitsPerWord] |= Bit(Index);
            }
            else
            {
                Words[Index / kBitsPerWord] &= ~Bit(Index);
            }
            return *this;
        }

        TBitSet& Set() noexcept
        {
            for (size_t Word = 0; Word < kWordCount; ++Word)
            {
                Words[Word] = ~static_cast<uint64>(0);
            }
            Trim();
            return *this;
        }

        FORCEINLINE TBitSet& Reset(size_t Index) noexcept { return Set(Index, false); }

        TBitSet& Reset() noexcept
        {
            for (size_t Word = 0; Word < kWordCount; ++Word)
            {
                Words[Word] = 0;
            }
            return *this;
        }

        FORCEINLINE TBitSet& Flip(size_t Index) noexcept
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, N);
            Words[Index / kBitsPerWord] ^= Bit(Index);
            return *this;
        }

        TBitSet& Flip() noexcept
        {
            for (size_t Word = 0; Word < kWordCount; ++Word)
            {
                Words[Word] = ~Words[Word];
            }
            Trim();
            return *this;
        }

        FORCEINLINE TBitSet& set(size_t Index, bool Value = true) noexcept { return Set(Index, Value); }
        FORCEINLINE TBitSet& set() noexcept { return Set(); }
        FORCEINLINE TBitSet& reset(size_t Index) noexcept { return Reset(Index); }
        FORCEINLINE TBitSet& reset() noexcept { return Reset(); }
        FORCEINLINE TBitSet& flip(size_t Index) noexcept { return Flip(Index); }
        FORCEINLINE TBitSet& flip() noexcept { return Flip(); }

        NODISCARD size_t Count() const noexcept
        {
            size_t Total = 0;
            for (size_t Word = 0; Word < kWordCount; ++Word)
            {
                Total += static_cast<size_t>(std::popcount(Words[Word]));
            }
            return Total;
        }

        NODISCARD FORCEINLINE size_t count() const noexcept { return Count(); }

        NODISCARD bool Any() const noexcept
        {
            for (size_t Word = 0; Word < kWordCount; ++Word)
            {
                if (Words[Word] != 0)
                {
                    return true;
                }
            }
            return false;
        }

        NODISCARD FORCEINLINE bool None() const noexcept { return !Any(); }
        NODISCARD bool All() const noexcept { return Count() == N; }

        NODISCARD FORCEINLINE bool any() const noexcept { return Any(); }
        NODISCARD FORCEINLINE bool none() const noexcept { return None(); }
        NODISCARD FORCEINLINE bool all() const noexcept { return All(); }

        /** Index of the lowest set bit, or N when there is none. */
        NODISCARD size_t FindFirst() const noexcept
        {
            for (size_t Word = 0; Word < kWordCount; ++Word)
            {
                if (Words[Word] != 0)
                {
                    return Word * kBitsPerWord + static_cast<size_t>(std::countr_zero(Words[Word]));
                }
            }
            return N;
        }

        NODISCARD FORCEINLINE bool operator[](size_t Index) const noexcept { return Test(Index); }
        NODISCARD FORCEINLINE FReference operator[](size_t Index) noexcept { return FReference(this, Index); }

        TBitSet& operator&=(const TBitSet& Other) noexcept
        {
            for (size_t Word = 0; Word < kWordCount; ++Word)
            {
                Words[Word] &= Other.Words[Word];
            }
            return *this;
        }

        TBitSet& operator|=(const TBitSet& Other) noexcept
        {
            for (size_t Word = 0; Word < kWordCount; ++Word)
            {
                Words[Word] |= Other.Words[Word];
            }
            return *this;
        }

        TBitSet& operator^=(const TBitSet& Other) noexcept
        {
            for (size_t Word = 0; Word < kWordCount; ++Word)
            {
                Words[Word] ^= Other.Words[Word];
            }
            return *this;
        }

        NODISCARD TBitSet operator~() const noexcept
        {
            TBitSet Result = *this;
            Result.Flip();
            return Result;
        }

        NODISCARD friend TBitSet operator&(TBitSet Left, const TBitSet& Right) noexcept { return Left &= Right; }
        NODISCARD friend TBitSet operator|(TBitSet Left, const TBitSet& Right) noexcept { return Left |= Right; }
        NODISCARD friend TBitSet operator^(TBitSet Left, const TBitSet& Right) noexcept { return Left ^= Right; }

        NODISCARD friend bool operator==(const TBitSet& Left, const TBitSet& Right) noexcept
        {
            for (size_t Word = 0; Word < kWordCount; ++Word)
            {
                if (Left.Words[Word] != Right.Words[Word])
                {
                    return false;
                }
            }
            return true;
        }

        NODISCARD const uint64* GetWords() const noexcept { return Words; }
        NODISCARD static constexpr size_t GetWordCount() noexcept { return kWordCount; }

    private:

        NODISCARD FORCEINLINE static constexpr uint64 Bit(size_t Index) noexcept
        {
            return static_cast<uint64>(1) << (Index % kBitsPerWord);
        }

        // The bits past N in the last word must stay clear, or Count and Any would report them.
        FORCEINLINE void Trim() noexcept
        {
            if constexpr (kWordCount > 0)
            {
                Words[kWordCount - 1] &= kTailMask;
            }
        }

        uint64 Words[kWordCount > 0 ? kWordCount : 1];
    };
}

namespace Lumina
{
    template <size_t N>
    using TBitSet = Containers::TBitSet<N>;
}
