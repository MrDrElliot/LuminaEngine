#include <gtest/gtest.h>
#include "World/Entity/Components/ViewCaptureComponent.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

using namespace Lumina;

namespace
{
    FCullView MakeFilterView(uint32 Include, uint32 Exclude)
    {
        FCullView View = {};
        View.CaptureInclude = Include;
        View.CaptureExclude = Exclude;
        return View;
    }
}

TEST(ViewCapture, UnfilteredViewsPassEveryInstance)
{
    const FCullView View = MakeFilterView(0u, 0u);
    EXPECT_TRUE(PassesCaptureFilter(View, 0u));
    EXPECT_TRUE(PassesCaptureFilter(View, 0b101u));
}

TEST(ViewCapture, ShowOnlyKeepsOnlyMembersOfItsSlot)
{
    const FCullView View = MakeFilterView(0b10u, 0u);
    EXPECT_TRUE(PassesCaptureFilter(View, 0b10u));
    EXPECT_TRUE(PassesCaptureFilter(View, 0b11u));
    EXPECT_FALSE(PassesCaptureFilter(View, 0b01u));
    // An instance no capture names belongs to no slot, so a ShowOnly view drops it.
    EXPECT_FALSE(PassesCaptureFilter(View, 0u));
}

TEST(ViewCapture, HiddenDropsMembersEvenWhenAlsoShown)
{
    const FCullView View = MakeFilterView(0b001u, 0b100u);
    EXPECT_TRUE(PassesCaptureFilter(View, 0b001u));
    EXPECT_FALSE(PassesCaptureFilter(View, 0b101u));
    EXPECT_FALSE(PassesCaptureFilter(MakeFilterView(0u, 0b100u), 0b100u));
}

TEST(ViewCapture, FilteredInstancesTakeTheCustomLayerRead)
{
    // The cull reads the static payload, where the filter mask lives, only behind the custom layer bit.
    EXPECT_EQ(PackFlagsAndThinning(0.0f) & INSTANCE_CUSTOM_LAYERS_BIT, 0u);
    EXPECT_NE(PackFlagsAndThinning(0.0f, RENDER_LAYER_DEFAULT_MASK, 0u, true, false, /*bCaptureFiltered*/ true) & INSTANCE_CUSTOM_LAYERS_BIT, 0u);
    EXPECT_NE(PackFlagsAndThinning(0.0f, RENDER_LAYER_DEFAULT_MASK << 1u) & INSTANCE_CUSTOM_LAYERS_BIT, 0u);
}

TEST(ViewCapture, ScriptFiltersStayUniqueAndClearTogether)
{
    SViewCaptureComponent Capture;
    const ECS::FEntity A(3u, 0u);
    const ECS::FEntity B(7u, 1u);

    Capture.AddShowOnly(A);
    Capture.AddShowOnly(A);
    Capture.AddHidden(B);
    EXPECT_EQ(Capture.ShowOnly.Num(), 1);
    EXPECT_EQ(Capture.Hidden.Num(), 1);

    Capture.ClearFilters();
    EXPECT_TRUE(Capture.ShowOnly.IsEmpty());
    EXPECT_TRUE(Capture.Hidden.IsEmpty());
}

TEST(ViewCapture, EachRequestIsSeenOnce)
{
    SViewCaptureComponent Capture;
    const uint32 Seen = Capture.CaptureRequests;
    Capture.RequestCapture();
    Capture.RequestCapture();
    // The renderer compares against the count it last served, so two requests between frames are one capture.
    EXPECT_NE(Capture.CaptureRequests, Seen);
    EXPECT_EQ(Capture.CaptureRequests, Seen + 2u);
}
