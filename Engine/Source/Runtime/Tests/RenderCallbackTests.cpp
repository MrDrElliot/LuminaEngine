#include <gtest/gtest.h>

#include "World/Scene/RenderScene/RenderCallbacks.h"
#include "World/Scene/RenderScene/RenderScene.h"

using namespace Lumina;

namespace
{
    class FNullRenderScene : public IRenderScene
    {
    public:

        FNullRenderScene() : IRenderScene(nullptr) {}

        void Init() override {}
        void Extract(const FViewVolume&, const SPostProcessSettings*) override {}
        void RenderView(uint8) override {}
        void Resize(const FUIntVector2&) override {}
        FUIntVector2 GetRenderExtent() const override { return FUIntVector2(0); }
    };

    struct FCallbackRig
    {
        FNullRenderScene    Scene;
        FRenderCallbackList List;
        TVector<FString>    Log;

        void Run(ERenderStage Stage)
        {
            FRenderContext Context(Scene);
            Context.Stage = Stage;
            List.Invoke(Context);
        }

        FRenderCallbackHandle AddLogger(ERenderStage Stages, const char* Name)
        {
            return List.Add(Stages, [this, Name](FRenderContext&) { Log.push_back(Name); }, FName(Name));
        }
    };
}

// A callback runs for the stages it asked for and no others, in the order the callbacks were added.
TEST(RenderCallbacks, RunOnlyForTheirStagesInRegistrationOrder)
{
    FCallbackRig Rig;
    Rig.AddLogger(ERenderStage::AfterOpaque, "First");
    Rig.AddLogger(ERenderStage::Overlay, "OverlayOnly");
    Rig.AddLogger(ERenderStage::AfterOpaque, "Second");

    Rig.Run(ERenderStage::AfterOpaque);
    ASSERT_EQ(Rig.Log.size(), 2u);
    EXPECT_EQ(Rig.Log[0], "First");
    EXPECT_EQ(Rig.Log[1], "Second");

    Rig.Log.clear();
    Rig.Run(ERenderStage::FrameStart);
    EXPECT_TRUE(Rig.Log.empty());
}

// One registration may cover several stages, and the context says which one is running.
TEST(RenderCallbacks, OneCallbackServesSeveralStages)
{
    FCallbackRig Rig;
    TVector<ERenderStage> Seen;
    Rig.List.Add(ERenderStage::Depth | ERenderStage::ShadowDepth,
        [&Seen](FRenderContext& Context) { Seen.push_back(Context.Stage); }, FName("Geometry"));

    EXPECT_TRUE(Rig.List.HasAny(ERenderStage::Depth));
    EXPECT_TRUE(Rig.List.HasAny(ERenderStage::ShadowDepth));
    EXPECT_FALSE(Rig.List.HasAny(ERenderStage::AfterOpaque));

    Rig.Run(ERenderStage::Depth);
    Rig.Run(ERenderStage::ShadowDepth);
    ASSERT_EQ(Seen.size(), 2u);
    EXPECT_EQ(Seen[0], ERenderStage::Depth);
    EXPECT_EQ(Seen[1], ERenderStage::ShadowDepth);
}

// Removing a callback stops it and clears the stage once nothing else uses it.
TEST(RenderCallbacks, RemovedCallbacksStopAndReleaseTheirStage)
{
    FCallbackRig Rig;
    FRenderCallbackHandle Handle = Rig.AddLogger(ERenderStage::Overlay, "Gone");
    EXPECT_TRUE(Handle.IsValid());

    Rig.List.Remove(Handle);
    EXPECT_FALSE(Handle.IsValid());
    EXPECT_FALSE(Rig.List.HasAny(ERenderStage::Overlay));

    Rig.Run(ERenderStage::Overlay);
    EXPECT_TRUE(Rig.Log.empty());
}

// A callback may remove itself or a later one mid-invoke, and the later one does not run.
TEST(RenderCallbacks, RemovalFromInsideACallbackIsSafe)
{
    FCallbackRig Rig;
    FRenderCallbackHandle Self;
    FRenderCallbackHandle Later;

    Self = Rig.List.Add(ERenderStage::AfterOpaque, [&](FRenderContext&)
    {
        Rig.Log.push_back("Self");
        Rig.List.Remove(Self);
        Rig.List.Remove(Later);
    }, FName("Self"));
    Later = Rig.AddLogger(ERenderStage::AfterOpaque, "Later");

    Rig.Run(ERenderStage::AfterOpaque);
    ASSERT_EQ(Rig.Log.size(), 1u);
    EXPECT_EQ(Rig.Log[0], "Self");
    EXPECT_FALSE(Rig.List.HasAny(ERenderStage::AfterOpaque));
}

// A callback added from inside a callback first runs on the next invoke, never the current one.
TEST(RenderCallbacks, AddFromInsideACallbackRunsNextTime)
{
    FCallbackRig Rig;
    bool bAdded = false;
    Rig.List.Add(ERenderStage::AfterOpaque, [&](FRenderContext&)
    {
        Rig.Log.push_back("Outer");
        if (!bAdded)
        {
            bAdded = true;
            Rig.AddLogger(ERenderStage::AfterOpaque, "Inner");
        }
    }, FName("Outer"));

    Rig.Run(ERenderStage::AfterOpaque);
    ASSERT_EQ(Rig.Log.size(), 1u);

    Rig.Log.clear();
    Rig.Run(ERenderStage::AfterOpaque);
    ASSERT_EQ(Rig.Log.size(), 2u);
    EXPECT_EQ(Rig.Log[0], "Outer");
    EXPECT_EQ(Rig.Log[1], "Inner");
}
