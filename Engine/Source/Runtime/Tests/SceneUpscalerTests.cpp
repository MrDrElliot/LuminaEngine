#include <gtest/gtest.h>

#include "Config/EngineSettings.h"
#include "Core/Object/Class.h"
#include "World/Scene/RenderScene/SceneUpscaler.h"

using namespace Lumina;

namespace
{
    class FTestUpscaler : public IUpscaler
    {
    public:

        EAntiAliasingMethod GetMethod() const override { return EAntiAliasingMethod::DLSS; }
        bool  IsSupported() const override { return bSupported; }
        float GetRenderScale(const FUIntVector2&, ERenderResolution, float) const override { return 0.5f; }
        void  Evaluate(RHI::FCmdListH, const FUpscaleInputs&) override {}

        bool bSupported = true;
    };

    // Puts the project's resolution settings back, since the settings object is shared by every test in the run.
    struct FResolutionSettingScope
    {
        FResolutionSettingScope()
        {
            Settings = GetMutableDefault<CRendererSettings>();
            Resolution = Settings->RenderResolution;
            Percent    = Settings->ScreenPercentage;
        }

        ~FResolutionSettingScope()
        {
            Settings->RenderResolution = Resolution;
            Settings->ScreenPercentage = Percent;
        }

        CRendererSettings* Settings   = nullptr;
        ERenderResolution  Resolution = ERenderResolution::Native;
        float              Percent    = 100.0f;
    };

    // Restores the project's choice, since the settings object is shared by every test in the run.
    struct FAntiAliasingSettingScope
    {
        explicit FAntiAliasingSettingScope(EAntiAliasingMethod Method)
        {
            Settings = GetMutableDefault<CRendererSettings>();
            Previous = Settings->AntiAliasing;
            Settings->AntiAliasing = Method;
        }

        ~FAntiAliasingSettingScope()
        {
            Settings->AntiAliasing = Previous;
        }

        CRendererSettings*  Settings = nullptr;
        EAntiAliasingMethod Previous = EAntiAliasingMethod::TAA;
    };
}

TEST(SceneUpscaler, ScaleExtentRoundsAndClamps)
{
    EXPECT_EQ(Upscaling::ScaleExtent(FUIntVector2(1920, 1080), 0.5f), FUIntVector2(960, 540));
    EXPECT_EQ(Upscaling::ScaleExtent(FUIntVector2(1920, 1080), 1.0f), FUIntVector2(1920, 1080));
    EXPECT_EQ(Upscaling::ScaleExtent(FUIntVector2(2647, 1701), 0.67f), FUIntVector2(1773, 1140));
    EXPECT_EQ(Upscaling::ScaleExtent(FUIntVector2(3, 3), 0.01f), FUIntVector2(1, 1)) << "a view never drops to zero pixels";
    EXPECT_EQ(Upscaling::ScaleExtent(FUIntVector2(800, 600), 4.0f), FUIntVector2(800, 600)) << "it never renders above the display";
}

TEST(SceneUpscaler, HaltonJitterStaysInsideThePixelAndRepeats)
{
    constexpr uint32 Phases = 8;
    FVector2 Sum(0.0f);
    for (uint32 Index = 0; Index < Phases; ++Index)
    {
        const FVector2 Jitter = Upscaling::HaltonJitter(Index, Phases);
        EXPECT_GE(Jitter.x, -0.5f);
        EXPECT_LT(Jitter.x, 0.5f);
        EXPECT_GE(Jitter.y, -0.5f);
        EXPECT_LT(Jitter.y, 0.5f);
        EXPECT_EQ(Jitter, Upscaling::HaltonJitter(Index + Phases, Phases)) << "the cycle repeats every PhaseCount frames";
        Sum += Jitter;
    }
    EXPECT_NEAR(Sum.x / Phases, 0.0f, 0.1f);
    EXPECT_NEAR(Sum.y / Phases, 0.0f, 0.1f);
    EXPECT_NE(Upscaling::HaltonJitter(0, Phases), Upscaling::HaltonJitter(1, Phases));
}

TEST(SceneUpscaler, PhaseCountGrowsWithTheUpscaleRatio)
{
    FTestUpscaler Upscaler;
    EXPECT_EQ(Upscaler.GetJitterPhaseCount(FUIntVector2(1920, 1080), FUIntVector2(1920, 1080)), 8u);
    EXPECT_EQ(Upscaler.GetJitterPhaseCount(FUIntVector2(960, 540), FUIntVector2(1920, 1080)), 32u);
}

TEST(SceneUpscaler, RegistryFindsAnUpscalerByItsMethod)
{
    FTestUpscaler Upscaler;
    FUpscalerRegistry::Register(&Upscaler);
    FUpscalerRegistry::Register(&Upscaler);

    TVector<IUpscaler*> Registered;
    FUpscalerRegistry::GetRegistered(Registered);
    EXPECT_EQ(std::count(Registered.begin(), Registered.end(), &Upscaler), 1) << "registering twice keeps one entry";

    EXPECT_EQ(FUpscalerRegistry::Find(EAntiAliasingMethod::DLSS), &Upscaler);
    EXPECT_EQ(FUpscalerRegistry::Find(EAntiAliasingMethod::TAA), nullptr) << "TAA is built into each renderer, not registered";

    FUpscalerRegistry::Unregister(&Upscaler);
    EXPECT_EQ(FUpscalerRegistry::Find(EAntiAliasingMethod::DLSS), nullptr);
}

TEST(SceneUpscaler, OnlyTAAAndDLSSAreTemporal)
{
    EXPECT_FALSE(Upscaling::IsTemporal(EAntiAliasingMethod::None));
    EXPECT_FALSE(Upscaling::IsTemporal(EAntiAliasingMethod::SMAA));
    EXPECT_FALSE(Upscaling::IsTemporal(EAntiAliasingMethod::SMAAT2x)) << "T2x resolves two frames and leaves upscaling to the spatial pass";
    EXPECT_TRUE(Upscaling::IsTemporal(EAntiAliasingMethod::TAA));
    EXPECT_TRUE(Upscaling::IsTemporal(EAntiAliasingMethod::DLSS));
}

TEST(SceneUpscaler, MethodFollowsTheProjectSetting)
{
    FAntiAliasingSettingScope Scope{EAntiAliasingMethod::SMAAT2x};
    EXPECT_EQ(Upscaling::GetMethod(), EAntiAliasingMethod::SMAAT2x);
    Scope.Settings->AntiAliasing = EAntiAliasingMethod::DLSS;
    EXPECT_EQ(Upscaling::GetMethod(), EAntiAliasingMethod::DLSS);
}

TEST(SceneUpscaler, ResolutionsUseTheStandardRatios)
{
    EXPECT_FLOAT_EQ(Upscaling::GetResolutionScale(ERenderResolution::Native, 0.3f), 1.0f);
    EXPECT_NEAR(Upscaling::GetResolutionScale(ERenderResolution::Quality, 0.3f), 0.667f, 0.001f);
    EXPECT_FLOAT_EQ(Upscaling::GetResolutionScale(ERenderResolution::Balanced, 0.3f), 0.58f);
    EXPECT_FLOAT_EQ(Upscaling::GetResolutionScale(ERenderResolution::Performance, 0.3f), 0.5f);
    EXPECT_NEAR(Upscaling::GetResolutionScale(ERenderResolution::UltraPerformance, 0.3f), 0.333f, 0.001f);
    EXPECT_FLOAT_EQ(Upscaling::GetResolutionScale(ERenderResolution::Custom, 0.8f), 0.8f);
    EXPECT_FLOAT_EQ(Upscaling::GetResolutionScale(ERenderResolution::Custom, 0.1f), 0.25f) << "a custom percentage stays inside 25 to 100";
}

TEST(SceneUpscaler, RequestFollowsTheProjectResolution)
{
    FResolutionSettingScope Scope;

    Scope.Settings->RenderResolution = ERenderResolution::Performance;
    Scope.Settings->ScreenPercentage = 80.0f;
    Upscaling::FUpscaleRequest Request = Upscaling::GetRequest();
    EXPECT_EQ(Request.Resolution, ERenderResolution::Performance);
    EXPECT_FLOAT_EQ(Request.Scale, 0.5f) << "a named resolution ignores the custom percentage";

    Scope.Settings->RenderResolution = ERenderResolution::Custom;
    Request = Upscaling::GetRequest();
    EXPECT_EQ(Request.Resolution, ERenderResolution::Custom);
    EXPECT_FLOAT_EQ(Request.Scale, 0.8f);
}
