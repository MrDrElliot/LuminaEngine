
#include "EditorTool.h"

#include <Tools/PrimitiveManager/PrimitiveManager.h>

#include "imgui_internal.h"
#include "ToolFlags.h"
#include "Animation/SkeletonDebugDraw.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Object/Package/Package.h"
#include "Core/Windows/Window.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Transactions/EcsRegistrySnapshotCommand.h"
#include "Thumbnails/ThumbnailManager.h"
#include "Thumbnails/ThumbnailUtils.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/EditorAssetDropHandlers.h"
#include "Core/Application/Application.h"
#include "Core/UpdateContext.h"
#include "Input/InputContext.h"
#include "Input/InputProcessor.h"
#include "Input/InputViewport.h"
#include "Renderer/RHI.h"
#include "Renderer/RenderThread.h"
#include "UI/RmlUiBridge.h"
#include "World/WorldManager.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/InputComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    namespace
    {
        // Skeleton debug visualization. Editor-scoped and off by default; toggling the master
        // CVar makes bones appear in every editor tool's viewport (world, anim, anim-graph, etc.).
        static TConsoleVar<bool>  CVarDrawSkeletons    ("Editor.Debug.Skeletons",       false, "Draw the bone hierarchy for every skeletal mesh in the editor viewport.");
        static TConsoleVar<bool>  CVarSkeletonNames    ("Editor.Debug.SkeletonNames",   false, "Label bones with their names (requires Editor.Debug.Skeletons).");
        static TConsoleVar<bool>  CVarSkeletonAxes     ("Editor.Debug.SkeletonAxes",    false, "Draw a per-bone local X/Y/Z axis triad.");
        static TConsoleVar<bool>  CVarSkeletonXRay     ("Editor.Debug.SkeletonXRay",    true,  "Draw bones without depth testing so they show through the mesh.");
        static TConsoleVar<float> CVarSkeletonNameDist ("Editor.Debug.SkeletonNameDist", 10.0f, "Max camera distance (meters) at which bone names are drawn.");
    }

    FEditorTool::FEditorTool(IEditorToolContext* Context, const FString& DisplayName, CWorld* InWorld)
        : ToolContext(Context)
        , ToolName(DisplayName)
        , World(InWorld)
        , EditorEntity(entt::null)
    {
        ToolFlags |= EEditorToolFlags::Tool_WantsToolbar;

        // Command-based Undo/Redo routes its post-apply cache rebuild through the tool's existing hook.
        TransactionManager.OnPostApply = [this]() { OnPostUndoRedo(); };
    }

    FEditorTool::~FEditorTool() = default;

    void FEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);
    }

    void FEditorTool::GenerateThumbnail(CPackage* Package)
    {
        if (!World || !World->GetRenderer())
        {
            return;
        }

        const RHI::FTextureH RenderTarget = World->GetRenderer()->GetDisplayTexture();
        if (!RHI::IsValid(RenderTarget))
        {
            return;
        }

        const RHI::FTextureDesc Desc = RHI::GetTextureDesc(RenderTarget);
        const uint32 SourceWidth  = Desc.Dimension.x;
        const uint32 SourceHeight = Desc.Dimension.y;
        const RHI::GPUPtr Readback = RHI::Malloc((uint64)SourceWidth * SourceHeight * 4u, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);

        // Submit on the render thread (the sole graphics submitter) so this never
        // races the in-flight frame's command lists.
        auto RecordCapture = [&]()
        {
            RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdBarrier(CL, RHI::EStageFlags::AllCommands, RHI::EStageFlags::Transfer);
            RHI::CmdCopyTextureToMemory(CL, RenderTarget, RHI::FTextureSlice{}, Readback, SourceWidth);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::Host);
            // Wait only on this copy, not the whole device (see RHI::SubmitAndWait): WaitDeviceIdle from inside
            // a render-thread command wedges the cooperative drain and hangs later flushes.
            RHI::SubmitAndWait(CL);
            RHI::ResetCommandList(CL);
        };

        if (GRenderThread != nullptr && GRenderThread->IsRunning())
        {
            GRenderThread->EnqueueAndWait("ToolThumbnailCapture", [&RecordCapture]() { RecordCapture(); });
        }
        else
        {
            RecordCapture();
        }

        if (const void* MappedMemory = RHI::ToHost(Readback))
        {
            ThumbnailUtils::StoreDownsampledRGBA(*Package->GetPackageThumbnail(),
                static_cast<const uint8*>(MappedMemory), SourceWidth, SourceHeight, (size_t)SourceWidth * 4u);
        }

        RHI::Free(Readback);
    }

    void FEditorTool::Initialize()
    {
        ToolName = std::format("{0} {1}", GetTitlebarIcon(), GetToolName().c_str()).c_str();

        if (HasWorld())
        {
            // Use world context as "is initialized", editor worlds don't have a physics scene.
            if (GWorldManager->FindContext(World) == nullptr)
            {
                GWorldManager->CreateWorldContext(World, EWorldType::Editor);
            }

            SetupWorldForTool();

            Internal_CreateViewportTool();

            InputViewport = MakeUnique<FInputViewport>();
            InputViewport->SetWorld(World);
            InputViewport->GetContext().SetInputMode(EInputMode::Game);
            FInputViewportRegistry::Get().Register(InputViewport.get());
        }

        OnInitialize();
    }

    void FEditorTool::Deinitialize(const FUpdateContext& UpdateContext)
    {
        OnDeinitialize(UpdateContext);

        ToolWindows.clear();

        if (InputViewport)
        {
            FInputViewportRegistry::Get().Unregister(InputViewport.get());
            InputViewport.reset();
        }

        if (HasWorld())
        {
            // DestroyWorldContext tears the world down and releases the world manager's strong ref;
            // releasing ours then drops the refcount to zero and frees it. Do NOT ForceDestroyNow here:
            // this TObjectPtr still holds the world, so force-freeing would dangle (and the subsequent
            // release would touch freed memory).
            GWorldManager->DestroyWorldContext(World);
            World.Reset();
        }

        ToolWindows.clear();
    }

    void FEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_PROFILE_TAG(GetToolName().c_str());
        
        TickEditorCamera(UpdateContext.GetDeltaTime());
        TickEditorActions();

        // Enqueue bone debug lines here (mirrors DrawWorldGrid timing) so every tool that
        // calls the base Update gets skeleton visualization for free.
        DrawSkeletonDebug();
    }

    void FEditorTool::DrawDrawerContent(bool bFocused)
    {
        LUMINA_PROFILE_SCOPE();

        for (const TUniquePtr<FToolWindow>& Window : ToolWindows)
        {
            if (Window && Window->DrawFunction)
            {
                Window->DrawFunction(bFocused);
            }
        }
    }

    void FEditorTool::DrawSkeletonDebug()
    {
        if (World == nullptr || !CVarDrawSkeletons.GetValue())
        {
            return;
        }

        SkeletonDebugDraw::FOptions Options;
        Options.bAxes      = CVarSkeletonAxes.GetValue();
        Options.bDepthTest = !CVarSkeletonXRay.GetValue();

        // CWorld implements IPrimitiveDrawInterface, so it is the draw target.
        SkeletonDebugDraw::DrawWorldSkeletons(World, World, Options);
    }

    void FEditorTool::DrawSkeletonNameLabels(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize)
    {
        if (World == nullptr || !CVarDrawSkeletons.GetValue() || !CVarSkeletonNames.GetValue())
        {
            return;
        }

        SCameraComponent* Camera = World->GetActiveCamera();
        if (Camera == nullptr)
        {
            return;
        }

        TVector<SkeletonDebugDraw::FBoneLabel> Labels;
        SkeletonDebugDraw::GatherWorldBoneLabels(World, Labels);
        if (Labels.empty())
        {
            return;
        }

        // Same convention the world editor uses for its off-screen indicators: flip the
        // projection Y, then map NDC -> panel pixels.
        FMatrix4 Proj = Camera->GetProjectionMatrix();
        Proj[1][1] *= -1.0f;
        const FMatrix4 ViewProj = Proj * Camera->GetViewMatrix();
        const FVector3 CameraPos = FVector3(Math::Inverse(Camera->GetViewMatrix())[3]);

        const float MaxDist     = CVarSkeletonNameDist.GetValue();
        const float MaxDistSq   = MaxDist * MaxDist;
        ImDrawList* DrawList    = ImGui::GetWindowDrawList();
        const ImU32 TextColor   = IM_COL32(245, 248, 255, 255);
        const ImU32 PillColor   = IM_COL32(18, 19, 24, 190);
        const ImU32 BorderColor = IM_COL32(255, 168, 56, 130);  // matches the amber bones
        const ImU32 DotColor    = IM_COL32(255, 210, 120, 255);
        const ImVec2 Pad(5.0f, 2.0f);

        DrawList->PushClipRect(ViewportOrigin, ImVec2(ViewportOrigin.x + ViewportSize.x, ViewportOrigin.y + ViewportSize.y), true);
        for (const SkeletonDebugDraw::FBoneLabel& Label : Labels)
        {
            const FVector3 Delta = Label.WorldPosition - CameraPos;
            if (Math::Dot(Delta, Delta) > MaxDistSq)
            {
                continue;
            }

            const FVector4 Clip = ViewProj * FVector4(Label.WorldPosition, 1.0f);
            if (Clip.w <= 1e-4f)
            {
                continue; // behind the camera
            }

            const float NdcX = Clip.x / Clip.w;
            const float NdcY = Clip.y / Clip.w;
            const float ScreenX = (NdcX * 0.5f + 0.5f) * ViewportSize.x + ViewportOrigin.x;
            const float ScreenY = (1.0f - (NdcY * 0.5f + 0.5f)) * ViewportSize.y + ViewportOrigin.y;

            const char* Text = Label.Name.c_str();
            const ImVec2 TextSize = ImGui::CalcTextSize(Text);

            // A dark rounded pill behind the text keeps names legible against any geometry,
            // anchored just to the upper-right of the joint with a small marker dot.
            const ImVec2 TextPos(ScreenX + 8.0f, ScreenY - TextSize.y - 4.0f);
            const ImVec2 PillMin(TextPos.x - Pad.x, TextPos.y - Pad.y);
            const ImVec2 PillMax(TextPos.x + TextSize.x + Pad.x, TextPos.y + TextSize.y + Pad.y);

            DrawList->AddRectFilled(PillMin, PillMax, PillColor, 4.0f);
            DrawList->AddRect(PillMin, PillMax, BorderColor, 4.0f);
            DrawList->AddCircleFilled(ImVec2(ScreenX, ScreenY), 2.5f, DotColor);
            DrawList->AddText(TextPos, TextColor, Text);
        }
        DrawList->PopClipRect();
    }

    void FEditorTool::DrawSkeletonDebugMenuItems()
    {
        FConsoleRegistry& Console = FConsoleRegistry::Get();

        auto ToggleBool = [&](const char* Name, const char* Label, const char* Tooltip)
        {
            if (const bool* Value = Console.TryGetAs<bool>(Name))
            {
                bool Proxy = *Value;
                if (ImGui::MenuItem(Label, nullptr, &Proxy))
                {
                    Console.SetAs(Name, Proxy);
                }
                if (Tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGui::SetTooltip("%s", Tooltip);
                }
            }
        };

        ToggleBool("Editor.Debug.Skeletons",     LE_ICON_BONE " Show Skeleton", "Draw the bone hierarchy for skeletal meshes.");
        ImGui::Separator();

        const bool bMaster = Console.TryGetAs<bool>("Editor.Debug.Skeletons") && Console.GetAs<bool>("Editor.Debug.Skeletons");
        ImGui::BeginDisabled(!bMaster);
        ToggleBool("Editor.Debug.SkeletonNames", "Bone Names", "Label each bone with its name.");
        ToggleBool("Editor.Debug.SkeletonAxes",  "Bone Axes",  "Draw a per-bone local axis triad.");
        ToggleBool("Editor.Debug.SkeletonXRay",  "X-Ray",      "Draw bones through the mesh (no depth test).");

        if (const float* Dist = Console.TryGetAs<float>("Editor.Debug.SkeletonNameDist"))
        {
            ImGui::SetNextItemWidth(120.0f);
            float Proxy = *Dist;
            if (ImGui::DragFloat("Name Distance", &Proxy, 0.25f, 1.0f, 100.0f, "%.0f m"))
            {
                Console.SetAs("Editor.Debug.SkeletonNameDist", Proxy);
            }
        }
        ImGui::EndDisabled();
    }

    void FEditorTool::TickEditorActions()
    {
        if (EditorActions.empty())
        {
            return;
        }

        // Don't fire shortcuts while a text input field is active.
        const ImGuiIO& IO = ImGui::GetIO();
        if (IO.WantTextInput)
        {
            return;
        }

        for (const FEditorAction& Action : EditorActions)
        {
            const FInputChord& Chord = Action.DefaultChord;
            if (!Chord.IsValid() || !Action.Callback)
            {
                continue;
            }
            if (Chord.bCtrl  != IO.KeyCtrl)  continue;
            if (Chord.bShift != IO.KeyShift) continue;
            if (Chord.bAlt   != IO.KeyAlt)   continue;

            const bool bTriggered = ImGui::IsKeyPressed(Chord.Key, Action.bRepeatOnHold);
            if (!bTriggered)
            {
                continue;
            }
            if (Action.CanExecute && !Action.CanExecute())
            {
                continue;
            }
            Action.Callback();
        }
    }

    FString FInputChord::ToDisplayString() const
    {
        if (!IsValid())
        {
            return FString();
        }
        FString Out;
        if (bCtrl)  Out += "Ctrl+";
        if (bShift) Out += "Shift+";
        if (bAlt)   Out += "Alt+";
        Out += ImGui::GetKeyName(Key);
        return Out;
    }

    ImGuiID FEditorTool::CalculateDockspaceID() const
    {
        uint32 DockspaceID = CurrLocationID;
        char const* const EditorToolTypeName = GetUniqueTypeName();
        DockspaceID = ImHashData(EditorToolTypeName, strlen(EditorToolTypeName), DockspaceID);
        return DockspaceID;
    }

    void FEditorTool::SetWorld(CWorld* InWorld)
    {
        if (World == InWorld)
        {
            return;
        }

        if (World.IsValid())
        {
            // Release our strong ref after the context teardown rather than force-freeing while still
            // held (which would dangle this TObjectPtr). Refcount hitting zero frees the old world.
            GWorldManager->DestroyWorldContext(World);
            World.Reset();
        }

        World = InWorld;

        // Initialize the world (creates its context) if needed. Keyed on the world context, not
        // the physics scene -- editor worlds intentionally have no physics scene.
        if (GWorldManager->FindContext(World) == nullptr)
        {
            GWorldManager->CreateWorldContext(World, EWorldType::Editor);
        }

        if (InputViewport)
        {
            InputViewport->SetWorld(World);
        }

        SetupWorldForTool();
    }

    void FEditorTool::RebindToWorld(CWorld* InWorld)
    {
        World = InWorld;
        if (InputViewport)
        {
            InputViewport->SetWorld(InWorld);
        }
    }

    void FEditorTool::SetupWorldForTool()
    {
        EditorEntity = World->ConstructEntity("Editor Entity");
        World->EmplaceComponent<FHideInSceneOutliner>(EditorEntity);
        World->EmplaceComponent<SCameraComponent>(EditorEntity);
        World->EmplaceComponent<SInputComponent>(EditorEntity);
        World->EmplaceComponent<FEditorComponent>(EditorEntity);
        World->GetComponent<STransformComponent>(EditorEntity).SetLocation(FVector3(0.0f, 1.25f, 3.25f));

        World->SetActiveCamera(EditorEntity);
    }

    entt::entity FEditorTool::CreateFloorPlane(float YOffset, float ScaleX, float ScaleY)
    {
        FTransform Transform;
        Transform.Rotate({-90.0f, 0.0f, 0.0f});
        Transform.SetScale(FVector3(ScaleX, ScaleY, 1.0f));
        Transform.Translate(FVector3(0.0f, YOffset, 0.0f));
        
        entt::entity FloorEntity = World->ConstructEntity("FloorPlane", Transform);
        World->EmplaceComponent<FHideInSceneOutliner>(FloorEntity);
        SStaticMeshComponent& MeshComponent = World->EmplaceComponent<SStaticMeshComponent>(FloorEntity);
        MeshComponent.StaticMesh = CPrimitiveManager::Get().PlaneMesh;
        
        return FloorEntity;
    }

    void FEditorTool::DrawMainToolbar(const FUpdateContext& UpdateContext)
    {
        if (ImGui::MenuItem(LE_ICON_FILE_PLUS_OUTLINE"##New"))
        {
            OnNew();
        }

        if (IsAssetEditorTool())
        {
            if (ImGui::MenuItem(LE_ICON_CONTENT_SAVE"##Save"))
            {
                OnSave();
            }
        }

        ImGui::BeginDisabled(!AllowsUndoRedo() || !CanUndo());

        if (ImGui::MenuItem(LE_ICON_UNDO_VARIANT"##Undo"))
        {
            Undo();
        }
        ImGui::EndDisabled();
        
        ImGuiX::TextTooltip("Undo last transaction");

        ImGui::BeginDisabled(!AllowsUndoRedo() || !CanRedo());

        if (ImGui::MenuItem(LE_ICON_REDO_VARIANT"##Redo"))
        {
            Redo();
        }
        ImGui::EndDisabled();
        
        ImGuiX::TextTooltip("Redo last undo");
        

        if (ImGui::BeginMenu(LE_ICON_HELP_CIRCLE_OUTLINE" Help"))
        {
            DrawKeybindsMenu();

            if (ImGui::BeginTable("HelpTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                DrawHelpMenu();
                ImGui::EndTable();
            }
            ImGui::EndMenu();
        }

        DrawToolMenu(UpdateContext);
    }

    void FEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        ImGui::Dummy(ImVec2(0, 0));
    }

    void FEditorTool::UpdateViewportInput(const FUpdateContext& UpdateContext)
    {
        if ((bViewportFocused || bViewportHovered) && ImGui::IsKeyPressed(ImGuiKey_F11, false))
        {
            ToggleViewportFullscreen();
        }

        const ImVec2 ContentRegion = ImGui::GetContentRegionAvail();
        const ImVec2 ViewportSize(eastl::max(ContentRegion.x, 64.0f), eastl::max(ContentRegion.y, 64.0f));
        const ImVec2 CursorScreenPos = ImGui::GetCursorScreenPos();

        const float AspectRatio = (ViewportSize.x / ViewportSize.y);
        float t = (ViewportSize.x - 500) / (1200 - 500);
        t = Math::Clamp(t, 0.0f, 1.0f);
        const float NewFOV = Math::Mix(120.0f, 50.0f, t);

        if (SCameraComponent* CameraComponent = World->GetActiveCamera())
        {
            CameraComponent->SetAspectRatio(AspectRatio);
            CameraComponent->SetFOV(NewFOV);
        }

        if (bViewportHovered)
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            {
                ImGui::SetWindowFocus();
                bViewportFocused = true;
            }
        }

        if (InputViewport == nullptr)
        {
            return;
        }

        ImGuiViewport* HostViewport = ImGui::GetWindowViewport();
        const ImVec2 WindowOrigin = HostViewport->Pos;
        InputViewport->SetNativeWindowHandle(HostViewport->PlatformHandle);

        const ImVec2 PanelMin(CursorScreenPos.x - WindowOrigin.x, CursorScreenPos.y - WindowOrigin.y);
        const ImVec2 PanelMax(PanelMin.x + ViewportSize.x, PanelMin.y + ViewportSize.y);
        InputViewport->SetWindowRect(int(PanelMin.x), int(PanelMin.y), int(PanelMax.x), int(PanelMax.y));

        uint32 RTW = uint32(eastl::max(ViewportSize.x, 1.0f));
        uint32 RTH = uint32(eastl::max(ViewportSize.y, 1.0f));
        if (World != nullptr)
        {
            if (IRenderScene* Scene = World->GetRenderer())
            {
                const FUIntVector2 Extent = Scene->GetRenderExtent();
                if (Extent.x > 0 && Extent.y > 0)
                {
                    RTW = Extent.x;
                    RTH = Extent.y;
                }
            }
        }
        InputViewport->SetRenderTargetSize(RTW, RTH);

        if (World != nullptr)
        {
            RmlUi::SetWorldDisplaySize(World, FUIntVector2(uint32(eastl::max(ViewportSize.x, 1.0f)), uint32(eastl::max(ViewportSize.y, 1.0f))));
        }

        InputViewport->SetHovered(bViewportHovered);
        InputViewport->SetFocused(bViewportFocused);

        FInputViewportRegistry& Reg = FInputViewportRegistry::Get();

        if (bViewportHovered)
        {
            Reg.SetHoveredViewport(InputViewport.get());
        }
        else if (Reg.GetHoveredViewport() == InputViewport.get())
        {
            Reg.SetHoveredViewport(nullptr);
        }

        if (Reg.IsGameInputFocused())
        {
            void* const ThisWindow = InputViewport->GetNativeWindowHandle();
            if (Windowing::IsNativeWindowFocused(ThisWindow))
            {
                FInputViewport* Active = Reg.GetActiveViewport();
                const bool bActiveWindowFocused = Active != nullptr
                    && Windowing::IsNativeWindowFocused(Active->GetNativeWindowHandle());
                if (!bActiveWindowFocused)
                {
                    Reg.SetActiveViewport(InputViewport.get());
                    Reg.SetFocusedViewport(InputViewport.get());
                }
            }
        }
        else if (bViewportFocused)
        {
            Reg.SetFocusedViewport(InputViewport.get());
            Reg.SetActiveViewport(InputViewport.get());
        }
    }

    bool FEditorTool::DrawViewport(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture)
    {
        const ImVec2 ContentRegion = ImGui::GetContentRegionAvail();
        const ImVec2 ViewportSize(eastl::max(ContentRegion.x, 64.0f), eastl::max(ContentRegion.y, 64.0f));
        const ImVec2 WindowPosition = ImGui::GetCursorScreenPos();
        const ImVec2 WindowBottomRight = { WindowPosition.x + ViewportSize.x, WindowPosition.y + ViewportSize.y };

        ImGui::GetWindowDrawList()->AddRectFilled(WindowPosition, WindowBottomRight, IM_COL32(255, 0, 0, 255));

        ImGui::GetWindowDrawList()->AddImage(
            ViewportTexture,
            WindowPosition,
            WindowBottomRight,
            ImVec2(0, 0), ImVec2(1, 1),
            IM_COL32_WHITE
        );

        const ImGuiStyle& ImStyle = ImGui::GetStyle();

        ImVec2 Origin = ImGui::GetCursorStartPos();

        ImGui::Dummy(ImStyle.ItemSpacing);
        ImGui::SetCursorPos(Origin + ImStyle.ItemSpacing);
        DrawViewportOverlayElements(UpdateContext, ViewportTexture, ViewportSize);

        // Bone-name labels project onto the viewport panel; drawn after the overlay so they
        // sit on top. WindowPosition is the viewport image's top-left in screen space.
        DrawSkeletonNameLabels(WindowPosition, ViewportSize);

        Origin = ImGui::GetCursorStartPos();

        ImGui::Dummy(ImStyle.ItemSpacing);
        ImGui::SetCursorPos(Origin + ImStyle.ItemSpacing);
        DrawViewportToolbar(UpdateContext);
        
        if (World != nullptr && World->IsGameWorld() && bViewportHovered
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGui::IsAnyItemHovered())
        {
            FInputViewportRegistry& Reg = FInputViewportRegistry::Get();
            if (!Reg.IsGameInputFocused())
            {
                Reg.SetActiveViewport(InputViewport.get());
                Reg.SetGameInputFocused(true);
            }
        }

        if (ImGuiDockNode* pDockNode = ImGui::GetWindowDockNode())
        {
           pDockNode->LocalFlags = 0;
           pDockNode->LocalFlags |= ImGuiDockNodeFlags_NoDockingOverMe;
           pDockNode->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
        }

        return false;
    }

    void FEditorTool::DrawViewportToolbar(const FUpdateContext& UpdateContext)
    {
        ImGui::Dummy(ImVec2(0, 0));
    }

    void FEditorTool::DrawGameFocusIndicator(ImVec2 ViewportSize)
    {
        // The Shift+F1 hint shows on every visible game viewport while game input is focused (focus is
        // global), so a user looking at any preview -- including a docked client Game Preview that isn't the
        // active viewport -- always sees how to hand input back. The accent border marks the one viewport
        // input is actually routed to.
        FInputViewportRegistry& Reg = FInputViewportRegistry::Get();
        if (InputViewport == nullptr || World == nullptr || !World->IsGameWorld() || !Reg.IsGameInputFocused())
        {
            return;
        }

        ImDrawList* DL = ImGui::GetWindowDrawList();

        // Back out ItemSpacing to land the outline exactly on the image rect.
        const ImVec2 Spacing = ImGui::GetStyle().ItemSpacing;
        const ImVec2 Cursor  = ImGui::GetCursorScreenPos();
        const ImVec2 Min(Cursor.x - Spacing.x, Cursor.y - Spacing.y);
        const ImVec2 Max(Min.x + ViewportSize.x, Min.y + ViewportSize.y);

        // Accent border only on the active viewport -- the one actually receiving input.
        if (InputViewport.get() == Reg.GetActiveViewport())
        {
            const ImU32 Accent = IM_COL32(255, 176, 64, 200);
            // Drawn 1px inside the edge so the full 2px stroke stays within the image.
            DL->AddRect(ImVec2(Min.x + 1.0f, Min.y + 1.0f), ImVec2(Max.x - 1.0f, Max.y - 1.0f),
                Accent, 0.0f, 0, 2.0f);
        }

        // Faint, translucent hint in the top-right (clear of the toolbar at top-left).
        const char*  Hint     = "Shift+F1: Editor focus";
        const ImVec2 TextSize = ImGui::CalcTextSize(Hint);
        const float  Pad = 6.0f, Margin = 8.0f;
        const ImVec2 BgMin(Max.x - TextSize.x - Pad * 2.0f - Margin, Min.y + Margin);
        const ImVec2 BgMax(BgMin.x + TextSize.x + Pad * 2.0f, BgMin.y + TextSize.y + Pad * 1.5f);
        DL->AddRectFilled(BgMin, BgMax, IM_COL32(0, 0, 0, 70), 4.0f);
        DL->AddText(ImVec2(BgMin.x + Pad, BgMin.y + Pad * 0.75f), IM_COL32(235, 235, 235, 120), Hint);
    }

    void FEditorTool::FocusViewportToEntity(entt::entity Entity)
    {
        if (!HasWorld() || World->IsGameWorld())
        {
            return;
        }

        if (!World->IsValidEntity(Entity))
        {
            return;
        }

        const STransformComponent& EntityTransform = World->GetComponent<STransformComponent>(Entity);
        STransformComponent& EditorTransform = World->GetComponent<STransformComponent>(EditorEntity);

        // Resolve to world space, local would mis-frame any entity parented under another.
        const FVector3 EntityWorldLocation = EntityTransform.GetWorldLocation();
        const float FocusDistance = (CameraState.Mode == EEditorCameraMode::Orbit) ? CameraState.OrbitDistance : 10.0f;

        if (CameraState.Mode == EEditorCameraMode::Orbit)
        {
            // Re-anchor on the focused entity; TickEditorCamera lerps the orbit target here
            // over a few frames. Anchor snaps so a later ResetOrbitPan returns to it.
            CameraState.OrbitAnchor      = EntityWorldLocation;
            CameraState.FocusOrbitTarget = EntityWorldLocation;
            CameraState.FocusOrbitDistance = FocusDistance;
            CameraState.bFocusInterp     = true;
            return;
        }

        FVector3 CurrentForward = EditorTransform.GetForward();
        CameraState.FocusFreePosition = EntityWorldLocation - CurrentForward * FocusDistance;
        CameraState.FocusFreeRotation = Math::FindLookAtRotation(EntityWorldLocation, CameraState.FocusFreePosition);
        CameraState.bFocusInterp      = true;
    }

    void FEditorTool::SetCameraMode(EEditorCameraMode Mode)
    {
        if (CameraState.Mode == Mode)
        {
            return;
        }

        // Derive orbit yaw/pitch/distance from current camera so the first frame doesn't snap.
        if (Mode == EEditorCameraMode::Orbit && HasWorld() && EditorEntity != entt::null
            && World->IsValidEntity(EditorEntity))
        {
            const STransformComponent& Transform = World->GetComponent<STransformComponent>(EditorEntity);
            const FVector3 Position = Transform.GetLocation();
            const FVector3 Offset   = Position - CameraState.OrbitTarget;
            const float Distance = Math::Length(Offset);

            CameraState.OrbitDistance = Math::Max(Distance, 0.1f);
            // Yaw is around world-Y measured from +Z; pitch is the elevation above the XZ plane.
            CameraState.OrbitYaw   = Math::Degrees(std::atan2(Offset.x, Offset.z));
            CameraState.OrbitPitch = Math::Degrees(std::asin(Math::Clamp(Offset.y / CameraState.OrbitDistance, -1.0f, 1.0f)));
        }

        CameraState.Mode = Mode;
        CameraState.Velocity = FVector3(0.0f);

        // Apply immediately so the first render after SetupWorldForTool isn't at the default origin.
        if (Mode == EEditorCameraMode::Orbit)
        {
            ApplyOrbitTransform();
        }
    }

    void FEditorTool::SetOrbitTarget(const FVector3& Target, float Distance)
    {
        CameraState.OrbitTarget = Target;
        CameraState.OrbitAnchor = Target;
        if (Distance > 0.0f)
        {
            CameraState.OrbitDistance = Distance;
        }

        if (CameraState.Mode == EEditorCameraMode::Orbit)
        {
            ApplyOrbitTransform();
        }
    }

    void FEditorTool::ResetOrbitPan()
    {
        CameraState.OrbitTarget = CameraState.OrbitAnchor;
        if (CameraState.Mode == EEditorCameraMode::Orbit)
        {
            ApplyOrbitTransform();
        }
    }

    void FEditorTool::ApplyOrbitTransform()
    {
        if (!HasWorld() || EditorEntity == entt::null)
        {
            return;
        }
        if (!World->IsValidEntity(EditorEntity))
        {
            return;
        }

        const float YawRad   = Math::Radians(CameraState.OrbitYaw);
        const float PitchRad = Math::Radians(CameraState.OrbitPitch);
        const FVector3 Offset(
            CameraState.OrbitDistance * std::cos(PitchRad) * std::sin(YawRad),
            CameraState.OrbitDistance * std::sin(PitchRad),
            CameraState.OrbitDistance * std::cos(PitchRad) * std::cos(YawRad));

        const FVector3 NewPosition = CameraState.OrbitTarget + Offset;
        STransformComponent& Transform = World->GetComponent<STransformComponent>(EditorEntity);
        Transform.SetLocation(NewPosition);
        Transform.SetRotation(Math::FindLookAtRotation(CameraState.OrbitTarget, NewPosition));
    }

    void FEditorTool::DrawCameraModeSelector(float ItemWidth)
    {
        const char* Label = (CameraState.Mode == EEditorCameraMode::Orbit) ? "Orbit" : "Free";
        ImGui::PushItemWidth(ItemWidth);
        if (ImGui::BeginCombo("##CameraMode", Label, ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable("Free", CameraState.Mode == EEditorCameraMode::Free))
            {
                SetCameraMode(EEditorCameraMode::Free);
            }
            if (ImGui::Selectable("Orbit", CameraState.Mode == EEditorCameraMode::Orbit))
            {
                SetCameraMode(EEditorCameraMode::Orbit);
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();

        // Reset-pan button: only meaningful in orbit mode, and only when MMB-drag has actually
        // moved OrbitTarget off its anchor. Hidden otherwise so it doesn't add noise.
        if (CameraState.Mode == EEditorCameraMode::Orbit)
        {
            const bool bPanned = Math::Distance(CameraState.OrbitTarget, CameraState.OrbitAnchor) > 1e-4f;
            ImGui::SameLine();
            ImGui::BeginDisabled(!bPanned);
            if (ImGui::Button(LE_ICON_HOME "##ResetPan"))
            {
                ResetOrbitPan();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGui::SetTooltip("Reset Pan (return to anchor)");
            }
        }
    }

    void FEditorTool::TickEditorCamera(double DeltaTime)
    {
        if (!HasWorld() || EditorEntity == entt::null)
        {
            return;
        }
        if (!World->IsValidEntity(EditorEntity))
        {
            return;
        }

        if (InputViewport == nullptr)
        {
            return;
        }

        const ImGuiIO& IO = ImGui::GetIO();
        const bool bAllowInput = bViewportFocused && !IO.WantTextInput;
        
        const FInputContext& Raw = FInputViewportRegistry::Get().GetRawInput();

        const bool bWantLook = bAllowInput && Raw.IsMouseButtonDown(EMouseKey::ButtonRight);
        const bool bWantPan  = bAllowInput
                            && CameraState.Mode == EEditorCameraMode::Orbit
                            && Raw.IsMouseButtonDown(EMouseKey::ButtonMiddle);
        const bool bWantsCaptured = bWantLook || bWantPan;

        if (bWantsCaptured)
        {
            FInputViewportRegistry::Get().SetActiveViewport(InputViewport.get());
        }

        if (bWantsCaptured && !CameraState.bWasLooking)
        {
            BeginEditorLookCapture();
        }
        else if (!bWantsCaptured && CameraState.bWasLooking)
        {
            EndEditorLookCapture();
        }
        
        CameraState.bWasLooking = bWantsCaptured;

        STransformComponent& Transform = World->GetComponent<STransformComponent>(EditorEntity);

        // Advance any in-flight focus lerp before reading user input. Movement input
        // from the focused viewport cancels the lerp so the user can take over mid-flight.
        if (CameraState.bFocusInterp)
        {
            const bool bWheel = bAllowInput && Raw.GetMouseZ() != 0.0;
            // Fly keys only count while right-mouse looking, so bWantLook alone covers Free mode.
            const bool bMoveInput = bAllowInput && (bWantLook || bWantPan);

            if (bMoveInput || bWheel)
            {
                CameraState.bFocusInterp = false;
            }
            else
            {
                const float Alpha = 1.0f - std::exp(-CameraState.FocusInterpRate * static_cast<float>(DeltaTime));
                if (CameraState.Mode == EEditorCameraMode::Free)
                {
                    const FVector3 NewLoc = Math::Mix(Transform.GetLocation(), CameraState.FocusFreePosition, Alpha);
                    const FQuat NewRot = Math::Slerp(Transform.GetRotation(), CameraState.FocusFreeRotation, Alpha);
                    Transform.SetLocation(NewLoc);
                    Transform.SetRotation(NewRot);

                    if (Math::Distance(NewLoc, CameraState.FocusFreePosition) < 1e-3f)
                    {
                        Transform.SetLocation(CameraState.FocusFreePosition);
                        Transform.SetRotation(CameraState.FocusFreeRotation);
                        CameraState.bFocusInterp = false;
                    }
                }
                else
                {
                    CameraState.OrbitTarget   = Math::Mix(CameraState.OrbitTarget, CameraState.FocusOrbitTarget, Alpha);
                    CameraState.OrbitDistance = Math::Mix(CameraState.OrbitDistance, CameraState.FocusOrbitDistance, Alpha);

                    if (Math::Distance(CameraState.OrbitTarget, CameraState.FocusOrbitTarget) < 1e-3f)
                    {
                        CameraState.OrbitTarget   = CameraState.FocusOrbitTarget;
                        CameraState.OrbitDistance = CameraState.FocusOrbitDistance;
                        CameraState.bFocusInterp = false;
                    }
                }
            }
        }

        if (CameraState.Mode == EEditorCameraMode::Free)
        {
            if (!bAllowInput || CameraState.bFocusInterp)
            {
                return;
            }

            const FVector3 Forward = Transform.GetForward();
            const FVector3 Right   = Transform.GetRight();
            const FVector3 Up      = Transform.GetUp();

            float Speed = CameraState.Speed;
            if (ImGui::IsKeyDown(ImGuiKey_LeftShift))
            {
                Speed *= 10.0f;
            }

            // WASDQE flies only while the right mouse button is held (UE-style), so the
            // W/E/R gizmo hotkeys and Q/E don't shove the camera around.
            FVector3 Acceleration(0.0f);
            if (bWantLook)
            {
                if (ImGui::IsKeyDown(ImGuiKey_W)) Acceleration += Forward;
                if (ImGui::IsKeyDown(ImGuiKey_S)) Acceleration -= Forward;
                if (ImGui::IsKeyDown(ImGuiKey_D)) Acceleration += Right;
                if (ImGui::IsKeyDown(ImGuiKey_A)) Acceleration -= Right;
                if (ImGui::IsKeyDown(ImGuiKey_E)) Acceleration += Up;
                if (ImGui::IsKeyDown(ImGuiKey_Q)) Acceleration -= Up;
            }

            if (Math::Length(Acceleration) > 0.0f)
            {
                Acceleration = Math::Normalize(Acceleration) * Speed;
            }

            CameraState.Velocity += Acceleration * static_cast<float>(DeltaTime);
            constexpr float Drag = 10.0f;
            // Analytic decay; explicit Euler (v -= v*Drag*dt) flips sign below 10 FPS and stutters.
            CameraState.Velocity *= std::exp(-Drag * static_cast<float>(DeltaTime));

            Transform.Translate(CameraState.Velocity * static_cast<float>(DeltaTime) * CameraState.SpeedScale);

            if (bWantLook)
            {
                Transform.AddYaw(static_cast<float>(Raw.GetMouseDeltaX() * 0.1));

                // Clamp accumulated pitch, not the per-frame delta: derive current elevation
                // from forward and limit the delta so the camera can't flip past vertical.
                const FVector3 Forward2 = Transform.GetForward();
                const float Elevation   = Math::Degrees(Math::Asin(Math::Clamp(Forward2.y, -1.0f, 1.0f)));
                constexpr float PitchLimit = 89.0f;
                float PitchDelta = static_cast<float>(Raw.GetMouseDeltaY() * 0.1);
                PitchDelta = Math::Clamp(PitchDelta, Elevation - PitchLimit, Elevation + PitchLimit);
                Transform.AddPitch(PitchDelta);

                const double WheelZ = Raw.GetMouseZ();
                CameraState.SpeedScale += Math::Pow(1.05f, CameraState.SpeedScale) * static_cast<float>(WheelZ);
                CameraState.SpeedScale = Math::Clamp(CameraState.SpeedScale, 0.2f, 100.0f);
            }
        }
        else // Orbit
        {
            if (bAllowInput)
            {
                if (bWantLook)
                {
                    CameraState.OrbitYaw   -= static_cast<float>(Raw.GetMouseDeltaX() * 0.4);
                    CameraState.OrbitPitch += static_cast<float>(Raw.GetMouseDeltaY() * 0.4);
                    CameraState.OrbitPitch = Math::Clamp(CameraState.OrbitPitch, -89.0f, 89.0f);
                }

                if (Raw.IsMouseButtonDown(EMouseKey::ButtonMiddle))
                {
                    const float PanScale = CameraState.OrbitDistance * 0.002f;
                    const FVector3 Right = Transform.GetRight();
                    const FVector3 Up    = Transform.GetUp();
                    CameraState.OrbitTarget -= Right * static_cast<float>(Raw.GetMouseDeltaX()) * PanScale;
                    CameraState.OrbitTarget += Up    * static_cast<float>(Raw.GetMouseDeltaY()) * PanScale;
                }

                const double WheelZ = Raw.GetMouseZ();
                if (WheelZ != 0.0)
                {
                    const float Zoom = 0.1f * CameraState.OrbitDistance;
                    CameraState.OrbitDistance -= static_cast<float>(WheelZ) * Zoom;
                    CameraState.OrbitDistance = Math::Max(CameraState.OrbitDistance, 0.05f);
                }
            }

            ApplyOrbitTransform();
        }
    }

    void FEditorTool::BeginEditorLookCapture()
    {
        if (InputViewport == nullptr)
        {
            return;
        }
        InputViewport->GetContext().SetMouseMode(EMouseMode::Captured);
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
        FInputViewportRegistry::Get().ReapplyActiveCursorMode();
    }

    void FEditorTool::EndEditorLookCapture()
    {
        if (InputViewport == nullptr)
        {
            return;
        }
        InputViewport->GetContext().SetMouseMode(EMouseMode::Normal);
        ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
        FInputViewportRegistry::Get().ReapplyActiveCursorMode();
    }

    void FEditorTool::DrawWorldGrid(int Scale, int Spacing)
    {
        if (World && !World->IsGameWorld() && bWorldGridEnabled)
        {
            for (int i = -Scale; i <= Scale; ++i)
            {
                const float Coord = static_cast<float>(i * Spacing);
                
                const FVector4 ZAxisColor  = (i == 0) ? FVector4(0.0f, 0.0f, 1.0f, 1.0f) : FVector4(0.05f);
                const FVector4 XAxisColor  = (i == 0) ? FVector4(1.0f, 0.0f, 0.0f, 1.0f) : FVector4(0.05f);
                const float AxisThickness   = (i == 0) ? 5.0f : 3.5f;

                World->DrawLine(
                    FVector3(Coord, 0, -Scale * Spacing),
                    FVector3(Coord, 0,  Scale * Spacing),
                    ZAxisColor,
                    AxisThickness);
                

                World->DrawLine(
                    FVector3(-Scale * Spacing, 0, Coord),
                    FVector3( Scale * Spacing, 0, Coord),
                    XAxisColor,
                    AxisThickness);
            }
            
            World->DrawLine(
            FVector3(0, -Scale, 0),
            FVector3(0,  Scale, 0),
            FVector4(0.0f, 1.0f, 0.0f, 1.0f),
            8.0f);
        }
    }

    bool FEditorTool::BeginViewportToolbarGroup(char const* GroupID, ImVec2 GroupSize, const ImVec2& Padding)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, 0xFF2C2C2C);
        ImGui::PushStyleColor(ImGuiCol_Header, 0xFF2C2C2C);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, 0xFF2C2C2C);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0xFF303030);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0xFF3A3A3A);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Padding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);

        // Adjust "use available" height to default toolbar height
        if (GroupSize.y <= 0)
        {
            GroupSize.y = ImGui::GetFrameHeight();
        }

        return ImGui::BeginChild(GroupID, GroupSize, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    }

    void FEditorTool::EndViewportToolbarGroup()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
    }

    void FEditorTool::Internal_CreateViewportTool()
    {
        FToolWindow* Tool = CreateToolWindow(ViewportWindowName, nullptr);
        Tool->bViewport = true;
    }

    FEditorTool::FToolWindow* FEditorTool::CreateToolWindow(FName InName, const TFunction<void(bool)>& DrawFunction, const ImVec2& WindowPadding, bool DisableScrolling)
    {
        DEBUG_ASSERT(eastl::none_of(ToolWindows.begin(), ToolWindows.end(), [&](const TUniquePtr<FToolWindow>& W)
        {
            return W->Name == InName;
        }));
        
        auto ToolWindow = MakeUnique<FToolWindow>(InName, DrawFunction, WindowPadding, DisableScrolling);
        return ToolWindows.emplace_back(Move(ToolWindow)).get();
    }

    void FEditorTool::RemoveToolWindow(const FName& InName)
    {
        for (auto It = ToolWindows.begin(); It != ToolWindows.end(); ++It)
        {
            if ((*It)->Name == InName)
            {
                ToolWindows.erase(It);
                return;
            }
        }
    }
    
    void FEditorTool::DrawKeybindsMenu()
    {
        const bool bDisabled = EditorActions.empty();
        ImGui::BeginDisabled(bDisabled);
        const bool bOpen = ImGui::BeginMenu(LE_ICON_KEYBOARD" Keybinds");
        ImGui::EndDisabled();
        if (!bOpen)
        {
            return;
        }

        // Group by category, preserve registration order within each.
        TVector<FString> CategoryOrder;
        THashMap<FString, TVector<const FEditorAction*>> ByCategory;
        for (const FEditorAction& A : EditorActions)
        {
            if (ByCategory.find(A.Category) == ByCategory.end())
            {
                CategoryOrder.push_back(A.Category);
            }
            ByCategory[A.Category].push_back(&A);
        }

        if (ImGui::BeginTable("KeybindsTable", 2,
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Action",   ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch, 0.4f);

            for (const FString& Category : CategoryOrder)
            {
                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", Category.empty() ? "General" : Category.c_str());
                ImGui::TableNextColumn();

                for (const FEditorAction* A : ByCategory[Category])
                {
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(A->Name.c_str());
                    if (!A->Description.empty() && ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s", A->Description.c_str());
                    }

                    ImGui::TableNextColumn();
                    const FString Chord = A->DefaultChord.ToDisplayString();
                    ImGui::TextUnformatted(Chord.empty() ? "-" : Chord.c_str());
                }
            }
            ImGui::EndTable();
        }

        ImGui::EndMenu();
    }

    void FEditorTool::DrawHelpTextRow(const char* Label, const char* Text) const
    {
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        {
            ImGui::TextUnformatted(Label);
        }

        ImGui::TableNextColumn();
        {
            ImGui::TextUnformatted(Text);
        }
    }

    void FEditorTool::BeginTransaction()
    {
        if (!CanTransact())
        {
            return;
        }

        // World/prefab editors record a whole-registry snapshot as one command (migrated to fine-grained in Phase 3).
        TransactionManager.BeginTransaction(FName());
        TransactionManager.Record(MakeUnique<FEcsRegistrySnapshotCommand>(World));
    }

    void FEditorTool::EndTransaction(FName Name)
    {
        if (!CanTransact())
        {
            return;
        }

        TransactionManager.SetOpenTransactionName(Name);
        TransactionManager.CommitTransaction();

        // Dirty the world package so the unsaved-document indicator appears (most world sites rely on this).
        if (World != nullptr && World->GetPackage())
        {
            World->GetPackage()->MarkDirty();
        }
    }

    void FEditorTool::AbortTransaction()
    {
        TransactionManager.AbortTransaction();
    }

    void FEditorTool::Undo()
    {
        if (!AllowsUndoRedo() || !TransactionManager.CanUndo())
        {
            return;
        }

        const FName Name = TransactionManager.PeekUndoName();
        bRestoringTransaction = true;
        TransactionManager.Undo();   // applies the transaction and fires OnPostApply -> OnPostUndoRedo
        bRestoringTransaction = false;
        ImGuiX::Notifications::NotifyInfo("Undid {}", Name);
    }

    void FEditorTool::Redo()
    {
        if (!AllowsUndoRedo() || !TransactionManager.CanRedo())
        {
            return;
        }

        const FName Name = TransactionManager.PeekRedoName();
        bRestoringTransaction = true;
        TransactionManager.Redo();
        bRestoringTransaction = false;
        ImGuiX::Notifications::NotifyInfo("Redid {}", Name);
    }

    void FEditorTool::ClearTransactionHistory()
    {
        TransactionManager.Clear();
    }

    FTransform FEditorTool::GetCameraSpawnTransform(float DistanceForward) const
    {
        FTransform Result;
        if (World == nullptr)
        {
            return Result;
        }

        // Prefer the world's active camera; fall back to the EditorEntity (asset editors
        // typically own the camera there directly and may not call SetActiveCamera).
        const SCameraComponent* Camera = World->GetActiveCamera();
        if (Camera == nullptr && EditorEntity != entt::null && World->IsValidEntity(EditorEntity))
        {
            Camera = World->TryGetComponent<SCameraComponent>(EditorEntity);
        }

        if (Camera == nullptr)
        {
            return Result;
        }

        const FVector3 Position = Camera->GetPosition() + Camera->GetForwardVector() * DistanceForward;
        Result.SetLocation(Position);
        return Result;
    }

    entt::entity FEditorTool::HandleContentBrowserAssetDrop(FStringView VirtualPath, entt::entity DropTarget)
    {
        if (World == nullptr || VirtualPath.empty())
        {
            return entt::null;
        }

        FAssetData* AssetData = FAssetRegistry::Get().GetAssetByPath(VirtualPath);
        if (AssetData == nullptr)
        {
            LOG_WARN("Asset drop: no registry entry for '{}'", FString(VirtualPath.data(), VirtualPath.size()).c_str());
            return entt::null;
        }

        const FEditorAssetDropHandler* Handler = FEditorAssetDropRegistry::Get().FindHandler(AssetData->AssetClass);
        if (Handler == nullptr || !*Handler)
        {
            LOG_WARN("Asset drop: no drop handler for class '{}' ('{}')", AssetData->AssetClass.c_str(), FString(VirtualPath.data(), VirtualPath.size()).c_str());
            return entt::null;
        }

        CObject* Loaded = LoadObject<CObject>(AssetData->AssetGUID);
        if (Loaded == nullptr)
        {
            LOG_WARN("Asset drop: failed to load '{}'", FString(VirtualPath.data(), VirtualPath.size()).c_str());
            return entt::null;
        }

        const FTransform SpawnTransform = GetCameraSpawnTransform();
        return (*Handler)(World, Loaded, SpawnTransform, DropTarget);
    }
}
