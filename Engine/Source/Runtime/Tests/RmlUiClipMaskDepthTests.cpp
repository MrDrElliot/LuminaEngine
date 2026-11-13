#include <gtest/gtest.h>

#include "LogCountTestHelpers.h"
#include "UI/RmlUiRenderer.h"

#include <RmlUi/Core/Vertex.h>

using namespace Lumina;

namespace
{
    Rml::CompiledGeometryHandle CompileUnitQuad(FRmlUiRenderer& Renderer)
    {
        const Rml::Vertex Verts[4] =
        {
            { Rml::Vector2f(0.0f, 0.0f) },
            { Rml::Vector2f(10.0f, 0.0f) },
            { Rml::Vector2f(10.0f, 10.0f) },
            { Rml::Vector2f(0.0f, 10.0f) },
        };
        const int Indices[6] = { 0, 1, 2, 0, 2, 3 };
        return Renderer.CompileGeometry(Rml::Span<const Rml::Vertex>(Verts, 4), Rml::Span<const int>(Indices, 6));
    }
}

TEST(RmlUiClipMask, NestedIntersectAtTheCapWarnsOnlyPastIt)
{
    FRmlUiRenderer Renderer;
    Renderer.EnableClipMask(true);
    const Rml::CompiledGeometryHandle Quad = CompileUnitQuad(Renderer);

    Logging::Flush();
    Logging::ClearLogQueue();

    // Set plus seven Intersect calls nests eight clip ancestors deep, exactly the cap.
    Renderer.RenderToClipMask(Rml::ClipMaskOperation::Set, Quad, Rml::Vector2f(0.0f, 0.0f));
    for (int32 Index = 0; Index < 7; ++Index)
    {
        Renderer.RenderToClipMask(Rml::ClipMaskOperation::Intersect, Quad, Rml::Vector2f(0.0f, 0.0f));
    }
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, "RmlUi", "nested clips"), 0) << "eight nested clip ancestors fit the cap";

    // A ninth ancestor exceeds kMaxClipMasksPerDraw and must be reported, not silently dropped.
    Renderer.RenderToClipMask(Rml::ClipMaskOperation::Intersect, Quad, Rml::Vector2f(0.0f, 0.0f));
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, "RmlUi", "nested clips"), 1);
}
