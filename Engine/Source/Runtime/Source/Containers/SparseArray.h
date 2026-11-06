#pragma once

#include "Vector.h"
#include "Optional.h"
#include "Containers/Algorithm.h"
#include "Memory/Construct.h"

namespace Lumina
{
    /** Stable-index array: a removal leaves a hole the next add reuses, so every other index stays put. */
    template <typename T>
    class TSparseArray
    {
    public:

        using ElementType = T;
        using value_type  = T;
        using size_type   = int32;

        TSparseArray() = default;
        TSparseArray(const TSparseArray&) = default;
        TSparseArray(TSparseArray&&) noexcept = default;
        TSparseArray& operator=(const TSparseArray&) = default;
        TSparseArray& operator=(TSparseArray&&) noexcept = default;
        ~TSparseArray() = default;

        template <typename... TArgs>
        int32 Emplace(TArgs&&... Args)
        {
            if (!FreeIndices.IsEmpty())
            {
                const int32 Index = FreeIndices.Pop();
                Elements[Index].Emplace(std::forward<TArgs>(Args)...);
                return Index;
            }
            Elements.Emplace(Containers::InPlace, std::forward<TArgs>(Args)...);
            return Elements.Num() - 1;
        }

        int32 Add(const T& Value) { return Emplace(Value); }
        int32 Add(T&& Value) { return Emplace(std::move(Value)); }

        void RemoveAt(int32 Index)
        {
            if (IsAllocated(Index))
            {
                Elements[Index].Reset();
                FreeIndices.Add(Index);
            }
        }

        NODISCARD FORCEINLINE bool IsAllocated(int32 Index) const noexcept
        {
            return Elements.IsValidIndex(Index) && Elements[Index].IsSet();
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto TryGet(this TSelf&& Self, int32 Index) noexcept -> TLikeSelf<TSelf, T>*
        {
            return Self.IsAllocated(Index) ? &*Self.Elements[Index] : nullptr;
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto operator[](this TSelf&& Self, int32 Index) noexcept -> TLikeSelf<TSelf, T>& { return *Self.Elements[Index]; }

        NODISCARD FORCEINLINE int32 Num() const noexcept { return Elements.Num() - FreeIndices.Num(); }
        NODISCARD FORCEINLINE int32 GetMaxIndex() const noexcept { return Elements.Num(); }
        NODISCARD FORCEINLINE int32 NumFree() const noexcept { return FreeIndices.Num(); }
        NODISCARD FORCEINLINE bool IsEmpty() const noexcept { return Num() == 0; }

        NODISCARD float FragmentationRatio() const noexcept
        {
            return Elements.IsEmpty() ? 0.0f : static_cast<float>(FreeIndices.Num()) / static_cast<float>(Elements.Num());
        }

        void Empty()
        {
            Elements.Empty();
            FreeIndices.Empty();
        }

        void Reset()
        {
            Elements.Reset();
            FreeIndices.Reset();
        }

        void Reserve(int32 Count) { Elements.Reserve(Count); }

        void Shrink()
        {
            Elements.Shrink();
            FreeIndices.Shrink();
        }

        /** Closes every hole. This moves elements, so indices handed out earlier no longer hold. */
        void Compact()
        {
            if (FreeIndices.IsEmpty())
            {
                return;
            }

            int32 Write = 0;
            for (int32 Read = 0; Read < Elements.Num(); ++Read)
            {
                if (!Elements[Read].IsSet())
                {
                    continue;
                }
                if (Write != Read)
                {
                    Elements[Write].Emplace(std::move(*Elements[Read]));
                    Elements[Read].Reset();
                }
                ++Write;
            }

            Elements.SetNum(Write);
            FreeIndices.Reset();
        }

        template <bool bConst>
        class TIterator
        {
            using FOwner = std::conditional_t<bConst, const TSparseArray, TSparseArray>;

        public:

            using iterator_category = std::forward_iterator_tag;
            using value_type        = T;
            using difference_type   = std::ptrdiff_t;
            using pointer           = std::conditional_t<bConst, const T*, T*>;
            using reference         = std::conditional_t<bConst, const T&, T&>;

            TIterator() = default;
            TIterator(FOwner* InOwner, int32 InIndex) : Owner(InOwner), Index(InIndex) { SkipHoles(); }

            template <bool bOther>
            requires (bConst && !bOther)
            TIterator(const TIterator<bOther>& Other) : Owner(Other.Owner), Index(Other.Index) {}

            TIterator& operator++()
            {
                ++Index;
                SkipHoles();
                return *this;
            }

            TIterator operator++(int)
            {
                TIterator Copy = *this;
                ++(*this);
                return Copy;
            }

            NODISCARD reference operator*() const { return (*Owner)[Index]; }
            NODISCARD pointer operator->() const { return &(*Owner)[Index]; }

            NODISCARD friend bool operator==(const TIterator& Left, const TIterator& Right) { return Left.Owner == Right.Owner && Left.Index == Right.Index; }

            NODISCARD int32 GetIndex() const { return Index; }

        private:

            template <bool> friend class TIterator;

            void SkipHoles()
            {
                while (Index < Owner->Elements.Num() && !Owner->Elements[Index].IsSet())
                {
                    ++Index;
                }
            }

            FOwner* Owner = nullptr;
            int32   Index = 0;
        };

        using iterator       = TIterator<false>;
        using const_iterator = TIterator<true>;

        NODISCARD iterator begin() { return iterator(this, 0); }
        NODISCARD iterator end() { return iterator(this, Elements.Num()); }
        NODISCARD const_iterator begin() const { return const_iterator(this, 0); }
        NODISCARD const_iterator end() const { return const_iterator(this, Elements.Num()); }

    private:

        TVector<Containers::TOptional<T>> Elements;
        TVector<int32>                    FreeIndices;
    };

    template <typename T>
    struct TIsTriviallyRelocatable<TSparseArray<T>>
    {
        static constexpr bool Value = true;
    };
}
