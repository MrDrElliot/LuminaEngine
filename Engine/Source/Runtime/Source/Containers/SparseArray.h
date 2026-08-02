#pragma once

#include <algorithm>
#include <stdexcept>
#include "Array.h"


namespace Lumina
{
    /** Stable-index array; non-contiguous storage. */
    template <typename T>
    class TSparseArray
    {
    public:
        
        using Index = size_t;
        using value_type = T;
        using size_type = size_t;
    
        TSparseArray() = default;
        
        TSparseArray(const TSparseArray& other) 
        {
            Data.reserve(other.Data.size());
            for (Index i = 0; i < other.Data.size(); ++i)
            {
                if (other.Occupied[i])
                {
                    Index newIdx = Data.size();
                    Data.emplace_back_uninit();
                    new (&Data[newIdx]) T(*reinterpret_cast<const T*>(&other.Data[i]));
                    Occupied.push_back(true);
                    
                    if (i != newIdx)
                    {
                        while (Data.size() <= i)
                        {
                            Data.emplace_back_uninit();
                            Occupied.push_back(false);
                            if (Data.size() - 1 != i)
                            {
                                FreeList.push_back(Data.size() - 1);
                            }
                        }
                        if (!Occupied[i])
                        {
                            FreeList.erase(std::remove(FreeList.begin(), FreeList.end(), i), FreeList.end());
                            new (&Data[i]) T(*reinterpret_cast<const T*>(&other.Data[i]));
                            Occupied[i] = true;
                        }
                    }
                }
            }
            for (Index idx : other.FreeList) 
            {
                if (idx < Data.size() && !Occupied[idx]) 
                {
                    FreeList.push_back(idx);
                }
            }
        }
        
        TSparseArray(TSparseArray&& other) noexcept
            : Data(std::move(other.Data))
            , Occupied(std::move(other.Occupied))
            , FreeList(std::move(other.FreeList))
        {}
        
        TSparseArray& operator=(const TSparseArray& other)
        {
            if (this != &other)
            {
                TSparseArray temp(other);
                *this = std::move(temp);
            }
            return *this;
        }
        
        TSparseArray& operator=(TSparseArray&& other) noexcept
        {
            if (this != &other)
            {
                Clear();
                Data = std::move(other.Data);
                Occupied = std::move(other.Occupied);
                FreeList = std::move(other.FreeList);
            }
            return *this;
        }
    
        ~TSparseArray()
        {
            Clear();
        }
    
        template<typename... Args>
        Index Emplace(Args&&... args)
        {
            Index idx;
            if (!FreeList.empty())
            {
                idx = FreeList.back();
                FreeList.pop_back();
                new (&Data[idx]) T(std::forward<Args>(args)...);
                Occupied[idx] = true;
            }
            else
            {
                idx = Data.size();
                Data.emplace_back();
                Occupied.push_back(true);
                new (&Data[idx]) T(std::forward<Args>(args)...);
            }
            return idx;
        }
    
        Index Insert(const T& value)
        {
            return Emplace(value);
        }
    
        Index Insert(T&& value)
        {
            return Emplace(std::move(value));
        }
    
        bool Erase(Index idx)
        {
            if (idx >= Data.size() || !Occupied[idx])
            {
                return false;
            }
            
            reinterpret_cast<T*>(&Data[idx])->~T();
            Occupied[idx] = false;
            FreeList.push_back(idx);
            return true;
        }
    
        T* Get(Index idx) noexcept
        {
            return (idx < Data.size() && Occupied[idx]) ? reinterpret_cast<T*>(&Data[idx]) : nullptr;
        }
    
        const T* Get(Index idx) const noexcept
        {
            return (idx < Data.size() && Occupied[idx]) ? reinterpret_cast<const T*>(&Data[idx]) : nullptr;
        }
    
        T& At(Index idx)
        {
            if (idx >= Data.size() || !Occupied[idx])
            {
                throw std::out_of_range("TSparseArray::At: invalid index");
            }
            return *reinterpret_cast<T*>(&Data[idx]);
        }
    
        const T& At(Index idx) const
        {
            if (idx >= Data.size() || !Occupied[idx])
            {
                throw std::out_of_range("TSparseArray::At: invalid index");
            }
            return *reinterpret_cast<const T*>(&Data[idx]);
        }
    
        T& operator[](Index idx) noexcept
        {
            return *reinterpret_cast<T*>(&Data[idx]);
        }
    
        const T& operator[](Index idx) const noexcept
        {
            return *reinterpret_cast<const T*>(&Data[idx]);
        }
    
        bool IsValid(Index idx) const noexcept
        {
            return idx < Data.size() && Occupied[idx];
        }
    
        void Clear() noexcept
        {
            for (Index i = 0; i < Data.size(); ++i)
            {
                if (Occupied[i])
                {
                    reinterpret_cast<T*>(&Data[i])->~T();
                }
            }
            Data.clear();
            Occupied.clear();
            FreeList.clear();
        }
    
        void Reserve(size_type capacity)
        {
            Data.reserve(capacity);
            Occupied.reserve(capacity);
        }

        void ShrinkToFit()
        {
            Data.shrink_to_fit();
            FreeList.shrink_to_fit();
        }
    
        size_type Size() const noexcept { return Data.size() - FreeList.size(); }
        size_type Capacity() const noexcept { return Data.size(); }
        size_type FreeCount() const noexcept { return FreeList.size(); }
        bool Empty() const noexcept { return Size() == 0; }

        /** 0.0 = none, 1.0 = max fragmentation. */
        float FragmentationRatio() const noexcept 
        {
            return Data.empty() ? 0.0f : static_cast<float>(FreeList.size()) / Data.size();
        }
    
        void Defragment()
        {
            if (FreeList.empty())
            {
                return;
            }

            std::ranges::sort(FreeList);

            Index writeIdx = 0;
            Index readIdx = 0;

            while (readIdx < Data.size()) {
                if (Occupied[readIdx]) {
                    if (writeIdx != readIdx) {
                        new (&Data[writeIdx]) T(std::move(*reinterpret_cast<T*>(&Data[readIdx])));
                        reinterpret_cast<T*>(&Data[readIdx])->~T();
                        Occupied[writeIdx] = true;
                        Occupied[readIdx] = false;
                    }
                    ++writeIdx;
                }
                ++readIdx;
            }

            Data.resize(writeIdx);
            Occupied.resize(writeIdx);
            FreeList.clear();
        }

        class iterator
        {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = T;
            using difference_type = std::ptrdiff_t;
            using pointer = T*;
            using reference = T&;
    
            iterator() : Owner(nullptr), IndexValue(0) {}
            iterator(TSparseArray* owner, Index idx) : Owner(owner), IndexValue(idx) 
            {
                SkipHoles();
            }
    
            iterator& operator++()
            {
                ++IndexValue;
                SkipHoles();
                return *this;
            }
    
            iterator operator++(int)
            {
                iterator temp = *this;
                ++(*this);
                return temp;
            }
    
            reference operator*() const
            {
                return *reinterpret_cast<T*>(&Owner->Data[IndexValue]);
            }
    
            pointer operator->() const
            {
                return reinterpret_cast<T*>(&Owner->Data[IndexValue]);
            }
    
            bool operator==(const iterator& other) const
            {
                return Owner == other.Owner && IndexValue == other.IndexValue;
            }
    
            bool operator!=(const iterator& other) const
            {
                return !(*this == other);
            }
    
            Index GetIndex() const { return IndexValue; }
    
        private:
            
            void SkipHoles()
            {
                while (IndexValue < Owner->Data.size() && !Owner->Occupied[IndexValue])
                {
                    ++IndexValue;
                }
            }
    
            TSparseArray* Owner;
            Index IndexValue;
        };
    
        class const_iterator
        {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = const T;
            using difference_type = std::ptrdiff_t;
            using pointer = const T*;
            using reference = const T&;
    
            const_iterator() : Owner(nullptr), IndexValue(0) {}
            const_iterator(const TSparseArray* owner, Index idx) : Owner(owner), IndexValue(idx) 
            {
                SkipHoles();
            }
            const_iterator(const iterator& it) : Owner(it.Owner), IndexValue(it.IndexValue) {}
    
            const_iterator& operator++()
            {
                ++IndexValue;
                SkipHoles();
                return *this;
            }
    
            const_iterator operator++(int)
            {
                const_iterator temp = *this;
                ++(*this);
                return temp;
            }
    
            reference operator*() const
            {
                return *reinterpret_cast<const T*>(&Owner->Data[IndexValue]);
            }
    
            pointer operator->() const
            {
                return reinterpret_cast<const T*>(&Owner->Data[IndexValue]);
            }
    
            bool operator==(const const_iterator& other) const
            {
                return Owner == other.Owner && IndexValue == other.IndexValue;
            }
    
            bool operator!=(const const_iterator& other) const
            {
                return !(*this == other);
            }
    
            Index GetIndex() const { return IndexValue; }
    
        private:
            
            void SkipHoles()
            {
                while (IndexValue < Owner->Data.size() && !Owner->Occupied[IndexValue])
                {
                    ++IndexValue;
                }
            }
    
            const TSparseArray* Owner;
            Index IndexValue;
        };
    
        iterator begin() { return iterator(this, 0); }
        iterator end() { return iterator(this, Data.size()); }
        const_iterator begin() const { return const_iterator(this, 0); }
        const_iterator end() const { return const_iterator(this, Data.size()); }
        const_iterator cbegin() const { return const_iterator(this, 0); }
        const_iterator cend() const { return const_iterator(this, Data.size()); }
    
    private:
        
        struct alignas(T) FSparseArrayStorage
        {
            uint8 Bytes[sizeof(T)];
        };
    
        TVector<FSparseArrayStorage> Data;
        FBitVector Occupied;
        TVector<Index> FreeList;
    };
}
