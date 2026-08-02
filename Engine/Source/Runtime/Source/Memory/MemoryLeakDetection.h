#pragma once
#include "Core/Singleton/Singleton.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::Memory
{
    class MemoryLeakDetector : public TSingleton<MemoryLeakDetector>
    {
    public:

        void Shutdown() {}

        static void PreFrame()
        {
        }
        
        static void PostFrame()
        {
            // TODO: not implemented
        }

        void DetectLeak(uint64 pre, uint64 post)
        {
            if(post > pre)
            {
                PrintCallStack();
            }
        }


    private:

        uint64 PreFrameMemory = 0;
        
    };
}
