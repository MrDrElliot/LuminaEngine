#pragma once

#include <cstdlib>

class FLinearAllocator
{
public:

    const size_t DEFAULT_CAPACITY = 2ULL << 30;  // 2 GiB (2^31 bytes)
    
    FLinearAllocator()
        : Data(nullptr)
        , Offset(0)
        , Capacity(DEFAULT_CAPACITY)
    {
        Data = static_cast<char*>(std::malloc(Capacity));
    }

    ~FLinearAllocator()
    {
        std::free(Data);
        Data = nullptr;
        Offset = 0;
        Capacity = 0;
    }
    
    void* Alloc(size_t Size, size_t Alignment = 16)
    {
        size_t Padding = 0;
        size_t Remainder = Offset % Alignment;
        if (Remainder != 0)
        {
            Padding = Alignment - Remainder;
        }
        
        size_t AlignedOffset = Offset + Padding;
        
        if (AlignedOffset + Size > Capacity)
        {
            std::cerr << "Linear allocator out of memory!\n";
            std::abort();
        }
        
        void* Result = Data + AlignedOffset;
        
        Offset = AlignedOffset + Size;
        
        return Result;
    }
    
    void Reset()
    {
        Offset = 0;
    }
    
    size_t GetUsedSize() const { return Offset; }
    size_t GetCapacity() const { return Capacity; }

private:
    char*   Data;
    size_t  Offset;
    size_t  Capacity;
};
