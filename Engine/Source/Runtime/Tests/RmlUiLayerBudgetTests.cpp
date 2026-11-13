#include <gtest/gtest.h>

#include "UI/RmlUiRenderer.h"

using namespace Lumina;

namespace Lumina
{
    struct FRmlUiRendererTestAccess
    {
        // Matches GMaxLiveLayers in RmlUiRenderer.cpp.
        static constexpr uint32 kLayerBudget = 48;

        static void FillLayerBudget(FRmlUiRenderer& Renderer)
        {
            for (uint32 Index = 0; Index < kLayerBudget; ++Index)
            {
                FRmlUiRenderer::FLayer Layer;
                Layer.Color.Texture.Handle = Index + 1;
                Renderer.OwnedLayers.Add(Layer);
            }
        }

        // A push into the frame target draws the layer's content on screen, and a composite from it blends the screen onto itself.
        static int32 CountCommandsUsingFrameTargetAsLayer(const FRmlUiRenderer& Renderer)
        {
            int32 Count = 0;
            for (const FRmlUiRenderer::FLayerCommand& Cmd : Renderer.LayerCommands)
            {
                const bool bPush      = (Cmd.Op == FRmlUiRenderer::ELayerOp::Push      && Cmd.TargetLayer == FRmlUiRenderer::kNoLayer);
                const bool bComposite = (Cmd.Op == FRmlUiRenderer::ELayerOp::Composite && Cmd.Source      == FRmlUiRenderer::kNoLayer);
                Count += (bPush || bComposite) ? 1 : 0;
            }
            return Count;
        }
    };
}

TEST(RmlUiLayerBudget, PushPastTheBudgetNeverDrawsIntoTheFrameTarget)
{
    FRmlUiRenderer Renderer;
    Rml::RenderInterface& RenderInterface = Renderer;
    FRmlUiRendererTestAccess::FillLayerBudget(Renderer);

    const Rml::LayerHandle Layer = RenderInterface.PushLayer();
    EXPECT_NE(Layer, Rml::LayerHandle(0)) << "RmlUi still needs a nonzero handle for the refused layer";
    EXPECT_EQ(RenderInterface.SaveLayerAsTexture(), Rml::TextureHandle(0)) << "A box shadow in a refused layer must draw nothing, not undefined texels";
    RenderInterface.CompositeLayers(Layer, Rml::LayerHandle(0), Rml::BlendMode::Blend, {});
    RenderInterface.PopLayer();

    EXPECT_EQ(FRmlUiRendererTestAccess::CountCommandsUsingFrameTargetAsLayer(Renderer), 0);
}
