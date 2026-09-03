#include <gtest/gtest.h>

#include "Core/Math/Easing.h"
#include "World/Entity/Components/CameraComponent.h"

using namespace Lumina;

namespace
{
    // The arithmetic EvaluateCameraBlend used before it was routed through the shared easing.
    float LegacyCameraBlend(ECameraBlendFunction Function, float Alpha)
    {
        Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);
        switch (Function)
        {
        case ECameraBlendFunction::EaseIn:    return Alpha * Alpha;
        case ECameraBlendFunction::EaseOut:   return Alpha * (2.0f - Alpha);
        case ECameraBlendFunction::EaseInOut: return Alpha * Alpha * (3.0f - 2.0f * Alpha);
        case ECameraBlendFunction::Linear:
        default:                              return Alpha;
        }
    }
}

// Camera blends are visible in cinematics, so the refactor has to be a no-op at every alpha.
TEST(CameraBlendEasing, MatchesTheCurvesItReplaced)
{
    const ECameraBlendFunction Functions[] =
    {
        ECameraBlendFunction::Linear,
        ECameraBlendFunction::EaseIn,
        ECameraBlendFunction::EaseOut,
        ECameraBlendFunction::EaseInOut,
    };

    for (ECameraBlendFunction Function : Functions)
    {
        for (int32 Step = 0; Step <= 128; ++Step)
        {
            const float Alpha = (float)Step / 128.0f;
            EXPECT_NEAR(EvaluateCameraBlend(Function, Alpha), LegacyCameraBlend(Function, Alpha), 1e-6f)
                << "blend " << (int32)Function << " drifted at alpha " << Alpha;
        }
    }
}

TEST(CameraBlendEasing, ClampsOutOfRangeAlpha)
{
    EXPECT_NEAR(EvaluateCameraBlend(ECameraBlendFunction::EaseInOut, -2.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(EvaluateCameraBlend(ECameraBlendFunction::EaseInOut, 3.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(EvaluateCameraBlend(ECameraBlendFunction::Linear, -1.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(EvaluateCameraBlend(ECameraBlendFunction::Linear, 5.0f), 1.0f, 1e-6f);
}

// EaseInOut is Hermite, not the quadratic InOut, and mapping it to one would change every camera blend.
TEST(CameraBlendEasing, EaseInOutIsNotQuadraticInOut)
{
    const float Hermite = EvaluateCameraBlend(ECameraBlendFunction::EaseInOut, 0.25f);
    const float Quad    = Easing::Evaluate(EEaseTransition::Quad, EEaseType::InOut, 0.25f);

    EXPECT_NEAR(Hermite, 0.15625f, 1e-5f);
    EXPECT_NEAR(Quad, 0.125f, 1e-5f);
    EXPECT_GT(Hermite, Quad);
}
