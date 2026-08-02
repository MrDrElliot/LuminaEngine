#pragma once
#include "UpdateStage.h"
#include "Platform/GenericPlatform.h"


namespace Lumina
{
    class RUNTIME_API FUpdateContext
    {
    public:

        friend class FEngine;
        
        FORCEINLINE void MarkFrameStart(double InStart)
        {
            if (LastFrameStart > 0.0)
            {
                DeltaTime = InStart - LastFrameStart;
            }
            LastFrameStart = InStart;
            FrameStart = InStart;
        }

        FORCEINLINE void MarkFrameEnd(double InTime)
        {
            Frame++;
            Time = InTime;
        }
        
        FORCEINLINE double GetFrameStartTime() const { return FrameStart; }
        FORCEINLINE double GetTime() const { return Time; }
        FORCEINLINE float GetFPS() const { return 1.0f / (float)DeltaTime; }
        FORCEINLINE double GetDeltaTime() const { return DeltaTime; }
        FORCEINLINE uint64 GetFrame() const { return Frame; }
        FORCEINLINE EUpdateStage GetUpdateStage() const { return UpdateStage; }

        
    protected:

        double              Time = 0;
        double              FrameStart = 0;
        double              DeltaTime = 1.0 / 60.0;
        double              LastFrameStart = 0.0;
        float               FrameRateLimit = 144.0f;
        uint64              Frame = 0;
        EUpdateStage        UpdateStage = EUpdateStage::Max;
    };
}
