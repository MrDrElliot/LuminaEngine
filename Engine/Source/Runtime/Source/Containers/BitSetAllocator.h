#pragma once
#include "Array.h"
#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"


namespace Lumina
{
    class FBitSetAllocator
    {
    public:
        
        explicit FBitSetAllocator(size_t Capacity)
        {
            Allocated.resize(Capacity);
        }

        int Allocate()
        {
            FWriteScopeLock Lock(Mutex);
            
            int32 Result = -1;
            
            int32 Capacity = (int32)GetCapacity();
            for (int32 i = 0; i < Capacity; ++i)
            {
                int32 II = (NextAvailable + i) % Capacity;
                
                if (!Allocated[II])
                {
                    Result = II;
                    NextAvailable = (II + 1) % Capacity;
                    Allocated[II] = true;
                    break;
                }
            }
            
            return Result;
        }
        
        void Release(int32 Index)
        {
            FWriteScopeLock Lock(Mutex);
            
            Allocated[Index] = false;
            NextAvailable = Math::Min(NextAvailable, Index);
        }
        
        
        NODISCARD size_t GetCapacity() const { return Allocated.size(); }

    private:
        
        int32           NextAvailable = 0;
        FBitVector      Allocated;
        FSharedMutex    Mutex;
    };
}
