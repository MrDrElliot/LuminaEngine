#include "RuntimePCH.h"
#include "SceneUpscaler.h"

#include "Config/EngineSettings.h"
#include "Core/Object/Class.h"
#include "Containers/Algorithm.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Threading/Thread.h"

namespace Lumina
{
    namespace
    {
        FMutex GUpscalerMutex;

        TConsoleVar<int32> CVarAntiAliasing(
            "r.AntiAliasing",
            -1,
            "Overrides the project's anti-aliasing method without touching the saved settings. 0 None, 1 SMAA, 2 SMAA T2x, 3 TAA, 4 DLSS, below 0 follows the project.");

        TConsoleVar<float> CVarScreenPercentage(
            "r.ScreenPercentage",
            0.0f,
            "Overrides the project's upscaler mode with a custom percentage when above zero, from 25 to 100.");

        TVector<IUpscaler*>& GetUpscalers()
        {
            static TVector<IUpscaler*> Upscalers;
            return Upscalers;
        }

        float Halton(uint32 Index, uint32 Base)
        {
            float Result   = 0.0f;
            float Fraction = 1.0f;
            while (Index > 0)
            {
                Fraction /= (float)Base;
                Result   += Fraction * (float)(Index % Base);
                Index    /= Base;
            }
            return Result;
        }
    }

    uint32 IUpscaler::GetJitterPhaseCount(const FUIntVector2& RenderSize, const FUIntVector2& DisplaySize) const
    {
        const float RatioX = (float)Math::Max(DisplaySize.x, 1u) / (float)Math::Max(RenderSize.x, 1u);
        const float RatioY = (float)Math::Max(DisplaySize.y, 1u) / (float)Math::Max(RenderSize.y, 1u);
        return (uint32)Math::Max(8.0f, Math::Ceil(8.0f * RatioX * RatioY));
    }

    void FUpscalerRegistry::Register(IUpscaler* Upscaler)
    {
        if (Upscaler == nullptr)
        {
            return;
        }
        FScopeLock Lock(GUpscalerMutex);
        TVector<IUpscaler*>& Upscalers = GetUpscalers();
        if (!Algo::Contains(Upscalers, Upscaler))
        {
            Upscalers.Add(Upscaler);
        }
    }

    void FUpscalerRegistry::Unregister(IUpscaler* Upscaler)
    {
        FScopeLock Lock(GUpscalerMutex);
        TVector<IUpscaler*>& Upscalers = GetUpscalers();
        Upscalers.Remove(Upscaler);
    }

    IUpscaler* FUpscalerRegistry::Find(EAntiAliasingMethod Method)
    {
        FScopeLock Lock(GUpscalerMutex);
        for (IUpscaler* Upscaler : GetUpscalers())
        {
            if (Upscaler->GetMethod() == Method)
            {
                return Upscaler;
            }
        }
        return nullptr;
    }

    void FUpscalerRegistry::GetRegistered(TVector<IUpscaler*>& Out)
    {
        FScopeLock Lock(GUpscalerMutex);
        Out = GetUpscalers();
    }

    namespace Upscaling
    {
        EAntiAliasingMethod GetMethod()
        {
            const int32 Override = CVarAntiAliasing.GetValue();
            if (Override >= 0 && Override <= (int32)EAntiAliasingMethod::DLSS)
            {
                return (EAntiAliasingMethod)Override;
            }
            const CRendererSettings* Settings = GetDefault<CRendererSettings>();
            return Settings != nullptr ? Settings->AntiAliasing : EAntiAliasingMethod::TAA;
        }

        bool IsTemporal(EAntiAliasingMethod Method)
        {
            return Method == EAntiAliasingMethod::TAA || Method == EAntiAliasingMethod::DLSS;
        }

        float GetResolutionScale(ERenderResolution Resolution, float CustomScale)
        {
            switch (Resolution)
            {
            case ERenderResolution::Native:           return 1.0f;
            case ERenderResolution::Quality:          return 2.0f / 3.0f;
            case ERenderResolution::Balanced:         return 0.58f;
            case ERenderResolution::Performance:      return 0.5f;
            case ERenderResolution::UltraPerformance: return 1.0f / 3.0f;
            case ERenderResolution::Custom:           break;
            }
            return Math::Clamp(CustomScale, 0.25f, 1.0f);
        }

        FUpscaleRequest GetRequest()
        {
            const CRendererSettings* Settings = GetDefault<CRendererSettings>();

            FUpscaleRequest Request;
            const float Override = CVarScreenPercentage.GetValue();
            if (Override > 0.0f)
            {
                Request.Resolution = ERenderResolution::Custom;
                Request.Scale      = GetResolutionScale(ERenderResolution::Custom, Override / 100.0f);
                return Request;
            }

            Request.Resolution = Settings != nullptr ? Settings->RenderResolution : ERenderResolution::Native;
            Request.Scale      = GetResolutionScale(Request.Resolution, Settings != nullptr ? Settings->ScreenPercentage / 100.0f : 1.0f);
            return Request;
        }

        FVector2 HaltonJitter(uint32 Index, uint32 PhaseCount)
        {
            const uint32 Phase = (Index % Math::Max(PhaseCount, 1u)) + 1u;
            return FVector2(Halton(Phase, 2) - 0.5f, Halton(Phase, 3) - 0.5f);
        }

        FUIntVector2 ScaleExtent(const FUIntVector2& DisplaySize, float Scale)
        {
            const float Clamped = Math::Clamp(Scale, 0.01f, 1.0f);
            const uint32 X = (uint32)Math::Max(1.0f, Math::Floor((float)DisplaySize.x * Clamped + 0.5f));
            const uint32 Y = (uint32)Math::Max(1.0f, Math::Floor((float)DisplaySize.y * Clamped + 0.5f));
            return FUIntVector2(Math::Min(X, Math::Max(DisplaySize.x, 1u)), Math::Min(Y, Math::Max(DisplaySize.y, 1u)));
        }
    }
}
