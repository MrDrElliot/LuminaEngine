#include <gtest/gtest.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "Containers/Function.h"
#include "Tools/UI/ImGui/ImGuiAllocator.h"
#include "UI/Tools/NodeGraph/GraphEditor.h"

using namespace Lumina;

extern "C" DLL_IMPORT void LuminaModuleSetupImGui(void* InImGui, void* InImPlot);

namespace
{
    constexpr ImVec2 kDisplaySize(1280.0f, 720.0f);
    constexpr ImVec2 kNodeContent(120.0f, 40.0f);
    constexpr float  kPinSize = 12.0f;
    constexpr uint64 kFirstTieID = 7;

    struct FTestNode
    {
        int64  ID = 0;
        ImVec2 Position;
        uint32 InputPin  = 0;
        uint32 OutputPin = 0;
    };

    // Runs ImGui frames with no renderer behind them, standing in for the backend's texture work.
    class FCanvasHarness
    {
    public:

        FCanvasHarness()
        {
            ImGuiX::InstallImGuiAllocator();
            Context = ImGui::CreateContext();
            ImGui::SetCurrentContext(Context);
            LuminaModuleSetupImGui(Context, nullptr);

            ImGuiIO& IO = ImGui::GetIO();
            IO.DisplaySize  = kDisplaySize;
            IO.DeltaTime    = 1.0f / 60.0f;
            IO.IniFilename  = nullptr;
            IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

            Nodes.push_back(FTestNode{ 1, ImVec2(100.0f, 100.0f), 11, 12 });
            Nodes.push_back(FTestNode{ 2, ImVec2(500.0f, 300.0f), 21, 22 });
        }

        ~FCanvasHarness()
        {
            ImGui::DestroyContext(Context);
            LuminaModuleSetupImGui(nullptr, nullptr);
        }

        FCanvasHarness(const FCanvasHarness&) = delete;
        FCanvasHarness& operator=(const FCanvasHarness&) = delete;
        FCanvasHarness(FCanvasHarness&&) = delete;
        FCanvasHarness& operator=(FCanvasHarness&&) = delete;

        // One frame of the test graph, with AfterEnd reading back what the canvas saw.
        void Frame(const TFunction<void()>& AfterEnd = nullptr)
        {
            ImGui::NewFrame();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(kDisplaySize);
            ImGui::Begin("Host", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            ImGui::PopStyleVar();

            Editor.Begin("Canvas", (int32)Nodes.size() + 1);
            for (FTestNode& Node : Nodes)
            {
                Editor.BeginNode(Node.ID, Node.Position);
                ImGui::Dummy(kNodeContent);
                Editor.BeginPin(Node.InputPin, EGraphPinKind::Input, IM_COL32_WHITE);
                ImGui::Dummy(ImVec2(kPinSize, kPinSize));
                Editor.EndPin();
                ImGui::SameLine(kNodeContent.x - kPinSize);
                Editor.BeginPin(Node.OutputPin, EGraphPinKind::Output, IM_COL32_WHITE);
                ImGui::Dummy(ImVec2(kPinSize, kPinSize));
                Editor.EndPin();
                Editor.EndNode();
            }
            if (bCommented)
            {
                Editor.Comment(CommentID, CommentPosition, CommentBody, 30.0f, ImVec2(100.0f, 60.0f));
            }
            if (bLinked)
            {
                Editor.Link(LinkID, Nodes[0].OutputPin, Nodes[1].InputPin, IM_COL32(200, 120, 60, 255), 2.5f);
            }
            for (size_t Index = 0; Index < TiePositions.size(); ++Index)
            {
                Editor.ZipTie(kFirstTieID + Index, TiePositions[Index], { LinkID });
            }
            Editor.End();

            if (AfterEnd)
            {
                AfterEnd();
            }
            Editor.EndWindow();

            ImGui::End();
            ImGui::Render();
            for (ImTextureData* Texture : ImGui::GetPlatformIO().Textures)
            {
                if (Texture->Status == ImTextureStatus_WantCreate || Texture->Status == ImTextureStatus_WantUpdates)
                {
                    Texture->SetTexID((ImTextureID)1);
                    Texture->SetStatus(ImTextureStatus_OK);
                }
                else if (Texture->Status == ImTextureStatus_WantDestroy)
                {
                    Texture->SetTexID(ImTextureID_Invalid);
                    Texture->SetStatus(ImTextureStatus_Destroyed);
                }
            }
        }

        void Frames(int32 Count)
        {
            for (int32 Index = 0; Index < Count; ++Index)
            {
                Frame();
            }
        }

        void MoveTo(ImVec2 Position)
        {
            ImGui::GetIO().AddMousePosEvent(Position.x, Position.y);
            Frame();
        }

        // Moves in steps, so the drag is seen crossing its threshold rather than teleporting.
        void DragTo(ImVec2 Position, int32 Steps = 6)
        {
            const ImVec2 From = ImGui::GetIO().MousePos;
            for (int32 Step = 1; Step <= Steps; ++Step)
            {
                MoveTo(From + (Position - From) * ((float)Step / (float)Steps));
            }
        }

        void Button(ImGuiMouseButton MouseButton, bool bDown, const TFunction<void()>& AfterEnd = nullptr)
        {
            ImGui::GetIO().AddMouseButtonEvent(MouseButton, bDown);
            Frame(AfterEnd);
        }

        void Modifier(ImGuiKey Key, bool bDown)
        {
            ImGui::GetIO().AddKeyEvent(Key, bDown);
            Frame();
        }

        ImVec2 NodeCenter(int32 Index) const
        {
            return Nodes[Index].Position + ImVec2(8.0f, 4.0f) + kNodeContent * 0.5f;
        }

        ImVec2 OutputPinCenter(int32 Index) const
        {
            return Nodes[Index].Position + ImVec2(8.0f + kNodeContent.x - kPinSize * 0.5f, 4.0f + kNodeContent.y + 4.0f + kPinSize * 0.5f);
        }

        ImVec2 InputPinCenter(int32 Index) const
        {
            return Nodes[Index].Position + ImVec2(8.0f + kPinSize * 0.5f, 4.0f + kNodeContent.y + 4.0f + kPinSize * 0.5f);
        }

        ImGuiContext*      Context = nullptr;
        FGraphEditor       Editor;
        TVector<FTestNode> Nodes;

        bool   bLinked = false;
        uint64 LinkID  = (12ull << 32) | 21ull;

        // Each ties the one link, with ids counting up from kFirstTieID.
        TVector<ImVec2> TiePositions;

        bool   bCommented = false;
        int64  CommentID  = 3;
        ImVec2 CommentPosition;
        ImVec2 CommentBody;
    };
}

TEST(GraphEditor, AClickSelectsTheNodeUnderIt)
{
    FCanvasHarness Harness;
    Harness.Frames(2);

    Harness.MoveTo(Harness.NodeCenter(0));
    Harness.Button(ImGuiMouseButton_Left, true);
    Harness.Button(ImGuiMouseButton_Left, false);

    EXPECT_TRUE(Harness.Editor.IsNodeSelected(1));
    EXPECT_FALSE(Harness.Editor.IsNodeSelected(2));
}

TEST(GraphEditor, DraggingANodeMovesItWithTheCursor)
{
    FCanvasHarness Harness;
    Harness.Frames(2);

    const ImVec2 Start = Harness.Nodes[0].Position;
    Harness.MoveTo(Harness.NodeCenter(0));
    Harness.Button(ImGuiMouseButton_Left, true);
    Harness.DragTo(Harness.NodeCenter(0) + ImVec2(60.0f, 40.0f));
    Harness.Button(ImGuiMouseButton_Left, false);

    EXPECT_NEAR(Harness.Nodes[0].Position.x, Start.x + 60.0f, 0.01f);
    EXPECT_NEAR(Harness.Nodes[0].Position.y, Start.y + 40.0f, 0.01f);
    EXPECT_EQ(Harness.Nodes[1].Position.x, 500.0f) << "only the dragged node moves";
}

TEST(GraphEditor, PullingAWireFromAPinReportsWhereItWasDropped)
{
    FCanvasHarness Harness;
    Harness.Frames(2);

    Harness.MoveTo(Harness.OutputPinCenter(0));
    Harness.Button(ImGuiMouseButton_Left, true);
    Harness.DragTo(Harness.InputPinCenter(1), 10);

    FGraphLinkDrag Dropped;
    bool bSawDrop = false;
    Harness.Button(ImGuiMouseButton_Left, false, [&]()
    {
        if (const FGraphLinkDrag* Drag = Harness.Editor.GetLinkDrag())
        {
            Dropped  = *Drag;
            bSawDrop = Drag->bDropped;
        }
    });

    ASSERT_TRUE(bSawDrop);
    EXPECT_EQ(Dropped.FromPin, 12u);
    EXPECT_EQ(Dropped.ToPin, 21u);
}

TEST(GraphEditor, EscapeDropsAWireBeingPulled)
{
    FCanvasHarness Harness;
    Harness.Frames(2);

    Harness.MoveTo(Harness.OutputPinCenter(0));
    Harness.Button(ImGuiMouseButton_Left, true);
    Harness.DragTo(Harness.InputPinCenter(1), 10);
    Harness.Modifier(ImGuiKey_Escape, true);
    Harness.Modifier(ImGuiKey_Escape, false);

    bool bStillPulling = true;
    Harness.Button(ImGuiMouseButton_Left, false, [&]() { bStillPulling = Harness.Editor.GetLinkDrag() != nullptr; });
    EXPECT_FALSE(bStillPulling);
}

TEST(GraphEditor, ABoxDraggedOverNodesSelectsThem)
{
    FCanvasHarness Harness;
    Harness.Frames(2);

    Harness.MoveTo(ImVec2(40.0f, 40.0f));
    Harness.Button(ImGuiMouseButton_Left, true);
    Harness.DragTo(ImVec2(700.0f, 450.0f));
    Harness.Button(ImGuiMouseButton_Left, false);

    EXPECT_TRUE(Harness.Editor.IsNodeSelected(1));
    EXPECT_TRUE(Harness.Editor.IsNodeSelected(2));
}

TEST(GraphEditor, RightClickingEmptyCanvasAsksForTheCreateMenu)
{
    FCanvasHarness Harness;
    Harness.Frames(2);

    Harness.MoveTo(ImVec2(900.0f, 600.0f));
    Harness.Button(ImGuiMouseButton_Right, true);

    EGraphItem Requested = EGraphItem::None;
    Harness.Button(ImGuiMouseButton_Right, false, [&]() { Requested = Harness.Editor.GetContextRequest().Item; });
    EXPECT_EQ(Requested, EGraphItem::Background);
}

TEST(GraphEditor, AltRightDragAcrossAWireAsksForAZipTie)
{
    FCanvasHarness Harness;
    Harness.bLinked = true;
    Harness.Frames(2);

    const ImVec2 Middle = (Harness.OutputPinCenter(0) + Harness.InputPinCenter(1)) * 0.5f;
    Harness.MoveTo(Middle + ImVec2(0.0f, -120.0f));
    Harness.Modifier(ImGuiMod_Alt, true);
    Harness.Button(ImGuiMouseButton_Right, true);
    Harness.DragTo(Middle + ImVec2(0.0f, 120.0f), 12);

    EGraphStroke Stroke = EGraphStroke::None;
    TVector<FGraphStrokeHit> Hits;
    Harness.Button(ImGuiMouseButton_Right, false, [&]() { Stroke = Harness.Editor.GetFinishedStroke(Hits); });
    Harness.Modifier(ImGuiMod_Alt, false);

    EXPECT_EQ(Stroke, EGraphStroke::ZipTie);
    ASSERT_EQ(Hits.size(), 1u);
    EXPECT_EQ(Hits[0].LinkID, Harness.LinkID);
    EXPECT_NEAR(Hits[0].Point.x, Middle.x, 2.0f);
}

TEST(GraphEditor, ATiedWireRunsThroughItsTie)
{
    FCanvasHarness Harness;
    Harness.bLinked     = true;
    Harness.TiePositions = { ImVec2(300.0f, 80.0f) };
    Harness.Frames(30);

    // Well away from where the untied curve would pass, so only the bundle reaching the tie can be under the cursor.
    uint64 Hovered = 0;
    Harness.MoveTo(Harness.TiePositions[0] + ImVec2(16.0f, 0.0f));
    Harness.Frame([&]() { Hovered = Harness.Editor.GetHoveredLink(); });
    EXPECT_EQ(Hovered, Harness.LinkID);

    uint64 HoveredTie = 0;
    Harness.MoveTo(Harness.TiePositions[0]);
    Harness.Frame([&]() { HoveredTie = Harness.Editor.GetHoveredZipTie(); });
    EXPECT_EQ(HoveredTie, kFirstTieID);
}

TEST(GraphEditor, AWireRunsThroughEveryTieItIsIn)
{
    FCanvasHarness Harness;
    Harness.bLinked      = true;
    Harness.TiePositions = { ImVec2(280.0f, 60.0f), ImVec2(420.0f, 460.0f) };
    Harness.Frames(30);

    // One tie above the untied curve and one below it, so the wire can only reach both by routing through each.
    for (size_t Index = 0; Index < Harness.TiePositions.size(); ++Index)
    {
        uint64 Hovered = 0;
        Harness.MoveTo(Harness.TiePositions[Index] + ImVec2(16.0f, 0.0f));
        Harness.Frame([&]() { Hovered = Harness.Editor.GetHoveredLink(); });
        EXPECT_EQ(Hovered, Harness.LinkID) << "tie " << Index;

        uint64 HoveredTie = 0;
        Harness.MoveTo(Harness.TiePositions[Index]);
        Harness.Frame([&]() { HoveredTie = Harness.Editor.GetHoveredZipTie(); });
        EXPECT_EQ(HoveredTie, kFirstTieID + Index) << "tie " << Index;
    }
}

TEST(GraphEditor, DraggingACommentCarriesTheNodesInsideIt)
{
    FCanvasHarness Harness;
    Harness.bCommented      = true;
    Harness.CommentPosition = ImVec2(60.0f, 40.0f);
    Harness.CommentBody     = ImVec2(300.0f, 200.0f);
    Harness.Frames(2);

    const ImVec2 NodeStart = Harness.Nodes[0].Position;
    Harness.MoveTo(Harness.CommentPosition + ImVec2(150.0f, 15.0f));
    Harness.Button(ImGuiMouseButton_Left, true);
    Harness.DragTo(Harness.CommentPosition + ImVec2(150.0f + 50.0f, 15.0f + 25.0f));
    Harness.Button(ImGuiMouseButton_Left, false);

    EXPECT_NEAR(Harness.CommentPosition.x, 110.0f, 0.01f);
    EXPECT_NEAR(Harness.Nodes[0].Position.x, NodeStart.x + 50.0f, 0.01f) << "the node sat inside the comment";
    EXPECT_NEAR(Harness.Nodes[1].Position.x, 500.0f, 0.01f) << "the node outside the comment stays";
}

TEST(GraphEditor, WheelZoomKeepsThePointUnderTheCursorInPlace)
{
    FCanvasHarness Harness;
    Harness.Frames(2);

    const ImVec2 Cursor(640.0f, 360.0f);
    Harness.MoveTo(Cursor);
    const ImVec2 Before = Harness.Editor.ScreenToCanvas(Cursor);

    ImGui::GetIO().AddMouseWheelEvent(0.0f, 3.0f);
    Harness.Frames(60);

    EXPECT_GT(Harness.Editor.GetZoom(), 1.4f);
    const ImVec2 After = Harness.Editor.ScreenToCanvas(Cursor);
    EXPECT_NEAR(After.x, Before.x, 0.5f);
    EXPECT_NEAR(After.y, Before.y, 0.5f);
}
