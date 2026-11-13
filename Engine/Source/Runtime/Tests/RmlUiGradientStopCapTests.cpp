#include <gtest/gtest.h>

#include "LogCountTestHelpers.h"
#include "UI/RmlUiRenderer.h"

#include <RmlUi/Core.h>

using namespace Lumina;

namespace
{
    Rml::Dictionary MakeLinearGradientParameters(size_t StopCount)
    {
        Rml::ColorStopList Stops;
        for (size_t i = 0; i < StopCount; ++i)
        {
            Rml::ColorStop Stop;
            Stop.color    = decltype(Stop.color)(255, 255, 255, 255);
            Stop.position = Rml::NumericValue(float(i) / float(StopCount), Rml::Unit::PERCENT);
            Stops.push_back(Stop);
        }

        Rml::Dictionary Parameters;
        Parameters["p0"] = Rml::Variant(Rml::Vector2f(0.0f, 0.0f));
        Parameters["p1"] = Rml::Variant(Rml::Vector2f(1.0f, 0.0f));
        Parameters["color_stop_list"] = Rml::Variant(Stops);
        return Parameters;
    }
}

TEST(RmlUiGradientStopCap, WarnsOnlyPastTheCap)
{
    FRmlUiRenderer Renderer;
    Rml::RenderInterface& RenderInterface = Renderer;

    Logging::Flush();
    Logging::ClearLogQueue();

    RenderInterface.CompileShader("linear-gradient", MakeLinearGradientParameters(16));
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, "RmlUi", "color stops"), 0) << "16 stops fit the cap";

    RenderInterface.CompileShader("linear-gradient", MakeLinearGradientParameters(17));
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, "RmlUi", "color stops"), 1);
}
