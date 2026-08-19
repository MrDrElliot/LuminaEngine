
#include "Containers/StringFormat.h"
#include "EditorTool.h"

#include <Tools/PrimitiveManager/PrimitiveManager.h>

#include "imgui_internal.h"
#include "EditorToolContext.h"
#include "ToolFlags.h"
#include "Animation/SkeletonDebugDraw.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Object/Package/Package.h"
#include "Core/Windows/Window.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Transactions/EcsRegistrySnapshotCommand.h"
#include "Transactions/EntityCreationCommand.h"
#include "Transactions/EntityTransformCommand.h"
#include "Settings/EditorSettings.h"
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
#include "Renderer/ImmediateLineRenderer.h"
#include "World/Entity/Systems/DebugDrawSystem.h"
#include "UI/RmlUiBridge.h"
#include "World/WorldManager.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/InputComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/TerrainSculptSystem.h"
#include "Tools/ComponentVisualizers/ComponentVisualizer.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Log/Log.h"

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
        RHI::SetDebugName(Readback, "Readback.ToolPreview");

        auto RecordCapture = [&]()
        {
            RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdBarrier(CL, RHI::EStageFlags::AllCommands, RHI::EStageFlags::Transfer);
            RHI::CmdCopyTextureToMemory(CL, RenderTarget, RHI::FTextureSlice{}, Readback, SourceWidth);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::Host);
            // Wait only on this copy, not the whole device (see RHI::SubmitAndWait): a WaitDeviceIdle
            // here would stall on unrelated in-flight frame work.
            RHI::SubmitAndWait(CL);
            RHI::ResetCommandList(CL);
        };

        RecordCapture();

        if (const void* MappedMemory = RHI::ToHost(Readback))
        {
            ThumbnailUtils::StoreDownsampledRGBA(*Package->GetPackageThumbnail(),
                static_cast<const uint8*>(MappedMemory), SourceWidth, SourceHeight, (size_t)SourceWidth * 4u);
        }

        RHI::Free(Readback);
    }

    void FEditorTool::Initialize()
    {
        ToolName = Format("{0} {1}", GetTitlebarIcon(), GetToolName().c_str()).c_str();

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

        // Shortcuts belong to the focused tool. Every open tool ticks its actions every frame, so
        // without this an unfocused tool's chords fire from whatever the user typed into a different
        // one -- the world editor saving the world while a material graph had focus, or its Undo
        // running on a Ctrl+Z meant for a text field elsewhere.
        if (!bIsActiveTool)
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
        FVector3 CameraLocation;
        FVector3 CameraTarget;
        GetDefaultCameraPose(CameraLocation, CameraTarget);

        STransformComponent& CameraTransform = World->GetComponent<STransformComponent>(EditorEntity);
        CameraTransform.SetLocation(CameraLocation);
        CameraTransform.SetRotation(Math::FindLookAtRotation(CameraTarget, CameraLocation));

        World->SetActiveCamera(EditorEntity);
    }

    void FEditorTool::GetDefaultCameraPose(FVector3& OutLocation, FVector3& OutTarget) const
    {
        OutLocation = FVector3(0.0f, 1.25f, 3.25f);
        // Straight ahead down -Z, which is the orientation this pose has always had.
        OutTarget = OutLocation - FVector3(0.0f, 0.0f, 1.0f);
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
        MeshComponent.SetStaticMesh(CPrimitiveManager::Get().PlaneMesh);
        
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
            // Tinted by dirty state. The icon used to look identical saved or not, so the only signal that
            // an edit still needed saving was the tab's UnsavedDocument dot -- which asset editors did not
            // report either. Left clickable when clean so a deliberate re-save is still possible.
            const bool bUnsaved = IsUnsavedDocument();
            ImGui::PushStyleColor(ImGuiCol_Text, bUnsaved ? EditorColors::Warning() : EditorColors::TextDim());
            if (ImGui::MenuItem(bUnsaved ? LE_ICON_CONTENT_SAVE_EDIT "##Save" : LE_ICON_CONTENT_SAVE "##Save"))
            {
                OnSave();
            }
            ImGui::PopStyleColor();
            // "{}" + arg: TextTooltip takes a consteval format string, so a ternary of two literals
            // cannot be passed as the format itself.
            ImGuiX::TextTooltip("{}", bUnsaved ? "Save -- this asset has unsaved changes" : "Save (no changes)");
        }

        const FFixedString AssetVirtualPath = GetAssetVirtualPath();
        if (!AssetVirtualPath.empty())
        {
            if (ImGui::MenuItem(LE_ICON_MAGNIFY"##BrowseToAsset"))
            {
                ToolContext->BrowseToAsset(FStringView(AssetVirtualPath.c_str(), AssetVirtualPath.size()));
            }

            ImGuiX::TextTooltip("Browse to asset in Content Browser");
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

        DrawViewModeMenu();
        DrawToolMenu(UpdateContext);
    }

    void FEditorTool::DrawViewModeMenu()
    {
        // One entry point per tool: scene tools already carry this in their viewport toolbar, so adding it
        // here too would give them two menus editing the same render settings.
        if (!HasWorld() || DrawsViewModeInViewportToolbar())
        {
            return;
        }

        if (ImGui::BeginMenu(LE_ICON_EYE " Visualize"))
        {
            DrawViewModePopupContents();
            ImGui::EndMenu();
        }
    }

    void FEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        ImGui::Dummy(ImVec2(0, 0));
    }

    void FEditorTool::DrawProjectionToggle(const ImVec2& Position, float Radius)
    {
        const ImViewGuizmo::Style& Style = ImViewGuizmo::GetStyle();
        const bool bOrtho = CameraState.bOrthographic;

        // A real ImGui item rather than a raw hit-test, so it stacks correctly against overlapping
        // panels; the view gizmo beside it cannot do this because it reads the mouse directly.
        const ImVec2 Cursor = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(Position);
        ImGui::InvisibleButton("##ViewProjectionToggle", ImVec2(Radius * 2.0f, Radius * 2.0f));

        const bool bHovered = ImGui::IsItemHovered();
        bViewGizmoOrthoHovered = bHovered || ImGui::IsItemActive();

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            SetEditorCameraOrthographic(!bOrtho);
        }

        if (bHovered)
        {
            ImGui::SetTooltip(bOrtho ? "Orthographic (click for perspective)" : "Perspective (click for orthographic)");
        }

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const ImVec2 Center(Position.x + Radius, Position.y + Radius);

        const ImU32 Background = (bHovered || bOrtho) ? Style.toolButtonHoveredColor : Style.toolButtonColor;
        DrawList->AddCircleFilled(Center, Radius, Background);

        if (bOrtho)
        {
            DrawList->AddCircle(Center, Radius, Style.highlightColor, 0, 2.0f * Style.scale);
        }

        const char* Icon = bOrtho ? LE_ICON_VECTOR_SQUARE : LE_ICON_ANGLE_ACUTE;
        const float FontSize = ImGui::GetFontSize();
        const ImVec2 IconSize = ImGui::GetFont()->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Icon);
        DrawList->AddText(ImGui::GetFont(), FontSize,
            ImVec2(Center.x - IconSize.x * 0.5f, Center.y - IconSize.y * 0.5f),
            Style.toolButtonIconColor, Icon);

        ImGui::SetCursorScreenPos(Cursor);
    }

    void FEditorTool::DrawViewGizmo(const ImVec2& ViewportOrigin, const ImVec2& ViewportSize)
    {
        const bool bHasEditorCamera = ShouldDrawViewGizmo() && HasEditorCameraControl()
            && EditorEntity != entt::null && World->IsValidEntity(EditorEntity)
            && World->HasComponent<STransformComponent>(EditorEntity);

        const float UIScale = ImGuiX::GetUIScale();

        ImViewGuizmo::Style& Style = ImViewGuizmo::GetStyle();
        Style.scale = 0.55f * UIScale;

        // Upstream's near-transparent light gray vanishes against a bright viewport.
        Style.toolButtonColor        = IM_COL32(18, 20, 24, 150);
        Style.toolButtonHoveredColor = IM_COL32(45, 50, 58, 195);

        // Distance from the gizmo's center out to the tip of an axis handle.
        const float GizmoExtent  = 80.0f * Style.scale;
        const float ButtonRadius = Style.toolButtonRadius * Style.scale;
        const float ButtonSize   = ButtonRadius * 2.0f;
        const float Margin       = 12.0f * UIScale;
        const float Spacing      = 6.0f * UIScale;

        const float NeededWidth  = (GizmoExtent * 2.0f) + (Margin * 2.0f);
        const float NeededHeight = (GizmoExtent * 2.0f) + (ButtonSize * 3.0f) + (Spacing * 4.0f) + (Margin * 2.0f);
        const bool bFits = ViewportSize.x >= NeededWidth && ViewportSize.y >= NeededHeight;

        if (!bHasEditorCamera || !bFits)
        {
            // Not drawing this frame, so nothing will release a latched drag or hover.
            ViewGizmoContext.Clear();
            CameraState.bViewGizmoActive = false;
            bViewGizmoOrthoHovered = false;
            return;
        }

        ImViewGuizmo::SetContext(&ViewGizmoContext);
        ImViewGuizmo::BeginFrame();
        ImViewGuizmo::Enable(bViewportHovered || ImViewGuizmo::IsUsing());

        STransformComponent& Transform = World->GetComponent<STransformComponent>(EditorEntity);

        const FVector3 StartLocation = Transform.GetLocation();
        const FQuat    StartRotation = Transform.GetRotation();

        FVector3 Pivot = CameraState.OrbitTarget;
        if (CameraState.Mode == EEditorCameraMode::Free)
        {
            if (!CameraState.bViewGizmoActive)
            {
                float PivotDistance = 10.0f;
                if (CameraState.bHasLastFocusPoint)
                {
                    const float FocusDistance = Math::Distance(StartLocation, CameraState.LastFocusPoint);
                    if (FocusDistance > 0.1f)
                    {
                        PivotDistance = FocusDistance;
                    }
                }

                CameraState.ViewGizmoPivot = StartLocation + Transform.GetForward() * PivotDistance;
            }

            Pivot = CameraState.ViewGizmoPivot;
        }

        const float PivotDistance = Math::Max(Math::Distance(StartLocation, Pivot), 0.1f);

        const ImVec2 GizmoCenter(
            ViewportOrigin.x + ViewportSize.x - Margin - GizmoExtent,
            ViewportOrigin.y + Margin + GizmoExtent);

        const float ButtonX = GizmoCenter.x + GizmoExtent - ButtonSize;
        const float DollyY  = GizmoCenter.y + GizmoExtent + Spacing;
        const float PanY    = DollyY + ButtonSize + Spacing;
        const float OrthoY  = PanY + ButtonSize + Spacing;

        DrawProjectionToggle(ImVec2(ButtonX, OrthoY), ButtonRadius);

        FVector3 Location = StartLocation;
        FQuat    Rotation = StartRotation;

        // 0.4 deg/px matches the RMB orbit; the translation speeds track the pivot distance so the
        // gizmo feels the same at any working scale.
        const bool bRotated = ImViewGuizmo::Rotate(Location, Rotation, Pivot, GizmoCenter, Math::Radians(0.4f));

        const FVector3 RotatedLocation = Location;
        const bool bDollied = ImViewGuizmo::Dolly(Location, Rotation, ImVec2(ButtonX, DollyY), PivotDistance * 0.005f);
        const FVector3 DollyDelta = Location - RotatedLocation;

        const FVector3 DolliedLocation = Location;
        const bool bPanned = ImViewGuizmo::Pan(Location, Rotation, ImVec2(ButtonX, PanY), PivotDistance * 0.002f);
        const FVector3 PanDelta = Location - DolliedLocation;

        CameraState.bViewGizmoActive = ImViewGuizmo::IsUsing() || ImViewGuizmo::IsAnimating();

        ImViewGuizmo::SetContext(nullptr);

        if (!bRotated && !bDollied && !bPanned)
        {
            return;
        }

        // A gizmo drag is an explicit camera move; drop any focus lerp still in flight.
        CameraState.bFocusInterp = false;
        CameraState.Velocity = FVector3(0.0f);

        if (CameraState.Mode == EEditorCameraMode::Orbit)
        {
            // Orbit mode regenerates the transform from these every tick, so writing it directly
            // would be overwritten by the next ApplyOrbitTransform.
            if (bRotated)
            {
                const FVector3 Offset = RotatedLocation - Pivot;
                const float Distance = Math::Max(Math::Length(Offset), 0.05f);

                CameraState.OrbitDistance = Distance;
                CameraState.OrbitYaw   = Math::Degrees(std::atan2(Offset.x, Offset.z));
                CameraState.OrbitPitch = Math::Clamp(Math::Degrees(std::asin(Math::Clamp(Offset.y / Distance, -1.0f, 1.0f))), -89.0f, 89.0f);
            }

            if (bDollied)
            {
                const FVector3 Forward = Rotation * FVector3(0.0f, 0.0f, 1.0f);
                CameraState.OrbitDistance = Math::Max(CameraState.OrbitDistance - Math::Dot(DollyDelta, Forward), 0.05f);
            }

            if (bPanned)
            {
                CameraState.OrbitTarget += PanDelta;
            }

            ApplyOrbitTransform();
            return;
        }

        // Dollying along forward is invisible under a parallel projection, so in ortho the button
        // drives the zoom distance instead and the position delta is discarded.
        if (bDollied && CameraState.bOrthographic)
        {
            const FVector3 Forward = Rotation * FVector3(0.0f, 0.0f, 1.0f);
            CameraState.OrthoFreeDistance = Math::Max(CameraState.OrthoFreeDistance - Math::Dot(DollyDelta, Forward), 0.05f);
            Location -= DollyDelta;
        }

        Transform.SetLocation(Location);
        Transform.SetRotation(Rotation);

        // Carry both free-mode pivots along so a following tumble doesn't snap the view.
        CameraState.ViewGizmoPivot += PanDelta;
        if (CameraState.bFreeOrbitPivotValid)
        {
            CameraState.FreeOrbitPivot += PanDelta;
        }
    }

    void FEditorTool::UpdateViewportInput(const FUpdateContext& UpdateContext)
    {
        if ((bViewportFocused || bViewportHovered) && ImGui::IsKeyPressed(ImGuiKey_F11, false))
        {
            ToggleViewportFullscreen();
        }

        const ImVec2 ContentRegion = ImGui::GetContentRegionAvail();
        const ImVec2 ViewportSize(Math::Max(ContentRegion.x, 64.0f), Math::Max(ContentRegion.y, 64.0f));
        const ImVec2 CursorScreenPos = ImGui::GetCursorScreenPos();

        const float AspectRatio = (ViewportSize.x / ViewportSize.y);
        float t = (ViewportSize.x - 500) / (1200 - 500);
        t = Math::Clamp(t, 0.0f, 1.0f);
        const float NewFOV = Math::Mix(120.0f, 50.0f, t);

        if (SCameraComponent* CameraComponent = World->GetActiveCamera())
        {
            CameraComponent->SetAspectRatio(AspectRatio);
            CameraComponent->SetFOV(NewFOV);

            if (CameraState.bOrthographic && HasEditorCameraControl())
            {
                // Match the world-space span perspective covered at the pivot, so toggling holds framing.
                const float Span = 2.0f * GetEditorViewDistance() * std::tan(Math::Radians(NewFOV) * 0.5f);
                CameraComponent->SetOrthographic(Span * AspectRatio);
            }
            else if (CameraComponent->IsOrthographic())
            {
                CameraComponent->SetPerspectiveProjection();
            }
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

        // Kept in SCREEN space (not window-relative like the input rect above) so a drop handler can turn
        // ImGui::GetMousePos() straight into a viewport-local pixel without knowing which window it is in.
        ViewportScreenMin  = CursorScreenPos;
        ViewportScreenSize = ViewportSize;

        uint32 RTW = uint32(Math::Max(ViewportSize.x, 1.0f));
        uint32 RTH = uint32(Math::Max(ViewportSize.y, 1.0f));
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
            RmlUi::SetWorldDisplaySize(World, FUIntVector2(uint32(Math::Max(ViewportSize.x, 1.0f)), uint32(Math::Max(ViewportSize.y, 1.0f))));
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
        const ImVec2 ViewportSize(Math::Max(ContentRegion.x, 64.0f), Math::Max(ContentRegion.y, 64.0f));
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

        // Before the overlay: the overlay's picking/gizmo code reads ShouldSuppressViewportClickInput.
        DrawViewGizmo(WindowPosition, ViewportSize);

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
        
        // Not while ejected: the editor owns the camera then, so a viewport click is a selection,
        // not a request to hand the mouse back to the game.
        if (World != nullptr && World->IsGameWorld() && !HasEditorCameraControl() && bViewportHovered
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

    bool FEditorTool::BeginViewportToolbarWindow(float& OutButtonSize)
    {
        const float Scale = ImGuiX::GetUIScale();
        const float Padding = 8.0f * Scale;
        const float ItemSpacing = 6.0f * Scale;
        constexpr float CornerRounding = 8.0f;

        OutButtonSize = 24.0f * Scale;

        const ImVec2 Pos = ImGui::GetWindowPos();
        ImGui::SetNextWindowPos(Pos + ImVec2(Padding, Padding));
        ImGui::SetNextWindowBgAlpha(0.85f);

        constexpr ImGuiWindowFlags WindowFlags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_AlwaysAutoResize;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Padding, Padding));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, CornerRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ItemSpacing, ItemSpacing));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        // Per-tool window ID. ImGui keys windows globally by name, so a shared literal made every open
        // tool's bar the SAME window -- harmless while only the world and prefab editors drew one (rarely
        // both at once), but now that every preview-world tool does, two open tools would append into one
        // another's bar and fight over its position.
        const FFixedString WindowName = GetToolWindowName("##ViewportToolbar", CurrDockspaceID);
        return ImGui::Begin(WindowName.c_str(), nullptr, WindowFlags);
    }

    void FEditorTool::EndViewportToolbarWindow()
    {
        ImGui::End();
        ImGui::PopStyleVar(4);
    }

    void FEditorTool::DrawViewModeButton(float ButtonSize)
    {
        if (ImGuiX::IconButton(LE_ICON_EYE, "##ViewMode", 0xFFFFFFFF, ImVec2(ButtonSize, ButtonSize)))
        {
            ImGui::OpenPopup("ViewModePopup");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("View Mode Options");
        }

        if (ImGui::BeginPopup("ViewModePopup", ImGuiWindowFlags_NoMove))
        {
            DrawViewModePopupContents();
            ImGui::EndPopup();
        }
    }

    void FEditorTool::DrawViewModePopupContents()
    {
        IRenderScene* RenderScene = HasWorld() ? World->GetRenderer() : nullptr;

        ImGui::Text("Visualizations");
        ImGui::Separator();

        if (ImGui::BeginMenu("Components"))
        {
            ImGui::Checkbox("Show All", &bShowComponentVisualizers);
            ImGui::BeginDisabled(!bShowComponentVisualizers);
            for (auto&& [Struct, Visualizer] : CComponentVisualizerRegistry::Get().GetVisualizers())
            {
                bool bFoobar = false;
                ImGui::Checkbox(Struct->MakeDisplayName().c_str(), &bFoobar);
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Physics"))
        {
            if (const bool* bValue = FConsoleRegistry::Get().TryGetAs<bool>("Jolt.Debug.Draw"))
            {
                bool bProxy = *bValue;
                if (ImGui::MenuItem("Toggle Collision", nullptr, &bProxy))
                {
                    FConsoleRegistry::Get().SetAs("Jolt.Debug.Draw", bProxy);
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Navigation"))
        {
            if (const bool* bValue = FConsoleRegistry::Get().TryGetAs<bool>("Nav.DrawDebug"))
            {
                bool bProxy = *bValue;
                if (ImGui::MenuItem("Draw NavMesh", nullptr, &bProxy))
                {
                    FConsoleRegistry::Get().SetAs("Nav.DrawDebug", bProxy);
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(LE_ICON_BONE " Skeleton"))
        {
            DrawSkeletonDebugMenuItems();
            ImGui::EndMenu();
        }

        // Everything below edits the world's render scene. A tool whose world has no renderer yet (still
        // initializing, or headless) keeps the debug-draw menus above, which are CVar-backed and global.
        if (RenderScene == nullptr)
        {
            DrawViewModeExtraItems();
            return;
        }

        if (ImGui::BeginMenu("Rendering"))
        {
            FSceneRenderSettings& Settings = RenderScene->GetSceneRenderSettings();

            if (ImGui::BeginMenu("View Mode"))
            {
                struct FViewModeEntry { ERenderSceneDebugFlags Mode; const char* Label; };

                static const FViewModeEntry Shading[] =
                {
                    { ERenderSceneDebugFlags::None,  "Lit"   },
                    { ERenderSceneDebugFlags::Unlit, "Unlit" },
                };
                static const FViewModeEntry Buffers[] =
                {
                    { ERenderSceneDebugFlags::BaseColor,         "Base Color"        },
                    { ERenderSceneDebugFlags::WorldNormal,       "World Normal"      },
                    { ERenderSceneDebugFlags::ShadingNormal,     "Shading Normal"    },
                    { ERenderSceneDebugFlags::Roughness,         "Roughness"         },
                    { ERenderSceneDebugFlags::Metallic,          "Metallic"          },
                    { ERenderSceneDebugFlags::AmbientOcclusion,  "Ambient Occlusion" },
                    { ERenderSceneDebugFlags::Emissive,          "Emissive"          },
                    { ERenderSceneDebugFlags::Specular,          "Specular"          },
                    { ERenderSceneDebugFlags::SelfShadow,        "Self Shadow"       },
                    { ERenderSceneDebugFlags::UV,                "UV"                },
                };
                // Clearcoat's two channels BORROW SelfShadow and Specular in the GBuffer, so each of
                // those four views marks the pixels it has no stored value for (violet) rather than
                // showing the unpack fallback. Shading Model is the key to reading them.
                static const FViewModeEntry ShadingModels[] =
                {
                    { ERenderSceneDebugFlags::ShadingModel,       "Shading Model"       },
                    { ERenderSceneDebugFlags::Clearcoat,          "Clearcoat"           },
                    { ERenderSceneDebugFlags::ClearcoatRoughness, "Clearcoat Roughness" },
                };
                static const FViewModeEntry Geometry[] =
                {
                    { ERenderSceneDebugFlags::Meshlets,         "Meshlets"          },
                    { ERenderSceneDebugFlags::MaterialID,       "Material ID"       },
                    { ERenderSceneDebugFlags::TriangleID,       "Triangle ID"       },
                    // Unlike the rest, this one keeps the lit shading and draws over it. Deferred
                    // opaque only -- the wire comes from the VisBuffer triangle, which terrain and
                    // translucency do not go through.
                    { ERenderSceneDebugFlags::WireframeOverlay, "Wireframe Overlay" },
                };
                static const FViewModeEntry Lighting[] =
                {
                    { ERenderSceneDebugFlags::LightComplexity, "Light Complexity" },
                    { ERenderSceneDebugFlags::ClusterGrid,     "Light Clusters"   },
                    { ERenderSceneDebugFlags::ShadowCascades,  "Shadow Cascades"  },
                    { ERenderSceneDebugFlags::ShadowPenumbra,  "Shadow Penumbra"  },
                    { ERenderSceneDebugFlags::GTAO,            "GTAO"             },
                    // Influence = does a probe reach this pixel and which one (black = none, so pure
                    // sky). Radiance = what that probe actually captured. The two failure modes look
                    // identical in a lit view, hence two separate inspectors.
                    { ERenderSceneDebugFlags::ProbeInfluence,  "Probe Influence"  },
                    { ERenderSceneDebugFlags::ProbeRadiance,   "Probe Radiance"   },
                };
                // Raw MBOIT target inspectors (OITResolve.slang): accum color flags INF red / NaN
                // magenta, moments shows the raw absorbance moments the generation pass wrote, and
                // transmittance is exp(-b_0), what the opaque scene behind the glass is multiplied
                // by. For chasing translucency artifacts.
                static const FViewModeEntry Translucency[] =
                {
                    { ERenderSceneDebugFlags::OITAccumColor,   "OIT Accum Color"   },
                    { ERenderSceneDebugFlags::OITMoments,      "OIT Moments"       },
                    { ERenderSceneDebugFlags::OITTransmittance,"OIT Transmittance" },
                    { ERenderSceneDebugFlags::OITLayerCount,   "OIT Layer Count"   },
                };

                auto DrawGroup = [&](const char* Header, const FViewModeEntry* Entries, size_t Count)
                {
                    ImGui::TextDisabled("%s", Header);
                    ImGui::Separator();
                    for (size_t i = 0; i < Count; ++i)
                    {
                        bool bSelected = Settings.Flags == Entries[i].Mode;
                        if (ImGui::MenuItem(Entries[i].Label, nullptr, bSelected))
                        {
                            Settings.Flags = Entries[i].Mode;
                        }
                    }
                };

                DrawGroup("Shading", Shading, std::size(Shading));
                ImGui::Spacing();
                DrawGroup("Buffers", Buffers, std::size(Buffers));
                ImGui::Spacing();
                DrawGroup("Shading Models", ShadingModels, std::size(ShadingModels));
                ImGui::Spacing();
                DrawGroup("Geometry", Geometry, std::size(Geometry));
                ImGui::Spacing();
                DrawGroup("Lighting", Lighting, std::size(Lighting));
                ImGui::Spacing();
                DrawGroup("Translucency", Translucency, std::size(Translucency));

                ImGui::EndMenu();
            }

            // Every stage that can remove geometry, individually switchable. This is a diagnostic
            // menu, not a quality one: when part of a mesh is missing, the question is always which
            // stage dropped it, and turning them off one at a time answers it in seconds where
            // reading the cull shaders takes an afternoon. Each entry says what it costs to disable
            // so nobody leaves one off and later reports the frame rate as a bug.
            if (ImGui::BeginMenu("Culling"))
            {
                // Spelled out per entry rather than driven by a pointer-to-member table: these are
                // bitfields, and there is no such thing as a pointer to one.
                static const char* const kNames[] =
                {
                    "Frustum Cull",
                    "Cone Cull",
                    "Occlusion Cull (Instances)",
                    "Occlusion Cull (Meshlets, 2-phase)",
                    "Occlusion Cull (Shadows)",
                    "CPU Instance Cull",
                    "Level of Detail",
                };
                static const char* const kTooltips[] =
                {
                    "Rejects instances and meshlets outside a view's frustum.\nOff: everything is submitted from every angle.",
                    "Rejects meshlets whose normal cone faces away from the view.\nOff: backfacing clusters are submitted. The only cull besides occlusion that removes PARTS of a mesh.",
                    "Whole instances hidden by last frame's depth pyramid are rejected outright. Single-phase and\nexact enough at this granularity: an object is hidden or it is not.\nOff: no instance-level occlusion culling.",
                    "Per-meshlet Hi-Z, resolved across two VisBuffer phases: the early pass defers what last frame's\npyramid hid, the pyramid is rebuilt from this frame's own depth, and the late pass re-tests exactly\nthose. No frame latency -- disoccluded meshlets still draw the same frame.\nOff: the frame collapses to a single geometry phase and meshlets are frustum-culled only.",
                    "The same test for shadow cascades, against the cascade pyramid.",
                    "Pre-upload reject of instances outside every contributing view, on the CPU.\nOff: the whole retained set is uploaded each frame.",
                    "Distance-based LOD selection.\nOff: always LOD 0, full detail.",
                };

                bool bValues[] =
                {
                    (bool)Settings.bFrustumCull,
                    (bool)Settings.bConeCull,
                    (bool)Settings.bOcclusionCull,
                    (bool)Settings.bMeshletOcclusionCull,
                    (bool)Settings.bShadowOcclusionCull,
                    (bool)Settings.bCPUInstanceCull,
                    (bool)Settings.bUseLODs,
                };
                static_assert(std::size(kNames) == std::size(kTooltips), "Cull toggle names and tooltips must pair up.");

                bool bChanged[std::size(kNames)] = {};
                for (size_t i = 0; i < std::size(kNames); ++i)
                {
                    bChanged[i] = ImGui::MenuItem(kNames[i], nullptr, &bValues[i]);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    {
                        ImGui::SetTooltip("%s", kTooltips[i]);
                    }
                }

                if (bChanged[0]) { Settings.bFrustumCull          = bValues[0]; }
                if (bChanged[1]) { Settings.bConeCull             = bValues[1]; }
                if (bChanged[2]) { Settings.bOcclusionCull        = bValues[2]; }
                if (bChanged[3]) { Settings.bMeshletOcclusionCull = bValues[3]; }
                if (bChanged[4]) { Settings.bShadowOcclusionCull  = bValues[4]; }
                if (bChanged[5]) { Settings.bCPUInstanceCull      = bValues[5]; }
                if (bChanged[6]) { Settings.bUseLODs              = bValues[6]; }

                ImGui::Separator();

                // The state to compare a suspicious frame against: whatever is still missing with all
                // of these off was not culled, which is worth more than any single toggle.
                const bool bAllOff = !Settings.bFrustumCull && !Settings.bConeCull && !Settings.bOcclusionCull
                                  && !Settings.bMeshletOcclusionCull && !Settings.bShadowOcclusionCull
                                  && !Settings.bCPUInstanceCull && !Settings.bUseLODs;
                if (ImGui::MenuItem("Disable All Culling", nullptr, bAllOff))
                {
                    const uint8 bEnable = bAllOff ? 1u : 0u;
                    Settings.bFrustumCull          = bEnable;
                    Settings.bConeCull             = bEnable;
                    Settings.bOcclusionCull        = bEnable;
                    Settings.bMeshletOcclusionCull = bEnable;
                    Settings.bShadowOcclusionCull  = bEnable;
                    Settings.bCPUInstanceCull      = bEnable;
                    Settings.bUseLODs              = bEnable;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGui::SetTooltip("Turns every stage above off (and back on). Expensive -- for isolating "
                                      "missing geometry, not for working in.");
                }

                ImGui::Separator();

                // Deliberately outside "Disable All Culling": this does not turn a stage off, it pins every
                // stage's inputs so you can fly out and look at what they decided.
                bool bFrozen = (bool)Settings.bFreezeCulling;
                if (ImGui::MenuItem("Freeze Culling", nullptr, &bFrozen))
                {
                    Settings.bFreezeCulling = bFrozen;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGui::SetTooltip("Pins the cull to the camera, frustums and depth pyramid as they are now, "
                                      "then lets you fly out and see what it selected.\n"
                                      "The frozen volume is drawn in orange. Rendering keeps following the real "
                                      "camera; only culling is frozen.");
                }

                ImGui::EndMenu();
            }

            ImGui::Separator();

            bool bWireframe = Settings.bWireframe;
            if (ImGui::MenuItem("Wireframe", nullptr, &bWireframe))
            {
                Settings.bWireframe = bWireframe;
            }

            bool bDrawBillboards = Settings.bDrawBillboards;
            if (ImGui::MenuItem("Draw Billboards", nullptr, &bDrawBillboards))
            {
                Settings.bDrawBillboards = bDrawBillboards;
            }

            bool bDrawAABB = Settings.bDrawAABB;
            if (ImGui::MenuItem("Draw Bounds", nullptr, &bDrawAABB))
            {
                Settings.bDrawAABB = bDrawAABB;
            }

            // Tool-specific view-mode items (world: Entity Debug Info, Game View).
            DrawViewModeExtraItems();

            ImGui::EndMenu();
        }
    }

    void FEditorTool::DrawViewportToolbar(const FUpdateContext& UpdateContext)
    {
        // Asset tools get the visualization menu on the menu bar next to Help, not as a floating overlay
        // -- see DrawViewModeMenu. Scene tools override this and keep theirs in the viewport.
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
        if (!HasEditorCameraControl())
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

        // Remembered so a later Alt+LMB tumble in Free mode picks a pivot at the distance the user
        // last framed something at, instead of an arbitrary default.
        CameraState.LastFocusPoint     = EntityWorldLocation;
        CameraState.bHasLastFocusPoint = true;

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

    void FEditorTool::SetEditorCameraOrthographic(bool bOrthographic)
    {
        // Seed before the flag flips, so GetEditorViewDistance still reports the pivot distance and
        // the ortho framing picks up where perspective left off.
        if (bOrthographic && !CameraState.bOrthographic && CameraState.Mode == EEditorCameraMode::Free)
        {
            CameraState.OrthoFreeDistance = GetEditorViewDistance();
        }

        CameraState.bOrthographic = bOrthographic;

        // UpdateViewportInput rebuilds the projection every frame, but a tool whose viewport is not
        // drawing this frame would otherwise keep the stale one.
        if (!bOrthographic && HasEditorCameraControl())
        {
            if (SCameraComponent* Camera = World->GetActiveCamera())
            {
                Camera->SetPerspectiveProjection();
            }
        }
    }

    float FEditorTool::GetEditorViewDistance() const
    {
        if (CameraState.Mode == EEditorCameraMode::Orbit)
        {
            return Math::Max(CameraState.OrbitDistance, 0.05f);
        }

        if (CameraState.bOrthographic)
        {
            return Math::Max(CameraState.OrthoFreeDistance, 0.05f);
        }

        if (CameraState.bHasLastFocusPoint && World != nullptr
            && EditorEntity != entt::null && World->IsValidEntity(EditorEntity)
            && World->HasComponent<STransformComponent>(EditorEntity))
        {
            const FVector3 Location = World->GetComponent<STransformComponent>(EditorEntity).GetLocation();
            const float Distance = Math::Distance(Location, CameraState.LastFocusPoint);
            if (Distance > 0.1f)
            {
                return Distance;
            }
        }

        return 10.0f;
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

    // Pointer travel, in pixels, before a held right button counts as a look instead of a context-menu
    // tap. Deliberately under ImGui's 15px viewport tap threshold, so a gesture can never both arm the
    // camera and still read as a tap.
    static constexpr float RightLookArmThresholdPixels = 4.0f;

    void FEditorTool::TickEditorCamera(double DeltaTime)
    {
        // Cleared first so a tool that early-outs never leaves the gesture flag latched for the
        // viewport code that reads it later this frame.
        CameraState.bLeftDragGesture = false;

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

        // The wheel needs a stricter gate than the buttons.
        const bool bWheelOverViewport = bViewportHovered
                                     || CameraState.bWasLooking                          // capture already in progress
                                     || Raw.IsMouseButtonDown(EMouseKey::ButtonRight);   // ...and its first frame
        const double WheelDelta = (bAllowInput && bWheelOverViewport) ? Raw.GetMouseZ() : 0.0;

        // The Alt modifier is read from ImGui like every other keyboard state here; only the mouse
        // comes from the raw context (see the fly-key comment further down).
        const bool bAltDown   = IO.KeyAlt;
        const bool bLeftDown  = bAllowInput && Raw.IsMouseButtonDown(EMouseKey::ButtonLeft);
        const bool bRightDown = bAllowInput && Raw.IsMouseButtonDown(EMouseKey::ButtonRight);

        // A left press made with no camera modifier belongs to selection/gizmo; latch it out of the
        // camera until release, so adding Alt or RMB mid-gizmo-drag can't also start a gesture.
        if (bLeftDown && !CameraState.bLeftMouseDownPrev)
        {
            CameraState.bLeftGestureBlocked = !bAltDown && !bRightDown;
        }
        else if (!bLeftDown)
        {
            CameraState.bLeftGestureBlocked = false;
        }
        CameraState.bLeftMouseDownPrev = bLeftDown;

        // A right press only becomes a look once the pointer actually travels; until then it stays a
        // candidate context-menu tap (see FEditorCameraState::bRightLookArmed). Pixels are accumulated
        // rather than measured from the press point so a slow drift still arms.
        if (!bRightDown)
        {
            CameraState.bRightLookArmed = false;
            CameraState.RightLookTravel = 0.0f;
        }
        else if (!CameraState.bRightLookArmed)
        {
            CameraState.RightLookTravel += static_cast<float>(Math::Abs(Raw.GetMouseDeltaX()) + Math::Abs(Raw.GetMouseDeltaY()));

            // A fly key or the wheel while the button is held is camera intent with no pointer motion at
            // all: RMB+W must fly immediately rather than after a jiggle. These only arm because the right
            // button is already down, which is the same gate the fly keys themselves live behind.
            const bool bFlyKeyDown = ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_A)
                                  || ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_D)
                                  || ImGui::IsKeyDown(ImGuiKey_Q) || ImGui::IsKeyDown(ImGuiKey_E);

            CameraState.bRightLookArmed = CameraState.RightLookTravel >= RightLookArmThresholdPixels
                                       || bFlyKeyDown
                                       || WheelDelta != 0.0;
        }

        // Mutually exclusive, highest priority first: LMB+RMB pans, Alt+LMB orbits, RMB alone looks.
        // Both buttons held must pan only, never pan and look at once.
        const bool bLeftGesture   = bLeftDown && !CameraState.bLeftGestureBlocked;
        const bool bWantPanDrag   = bLeftGesture && bRightDown;
        const bool bWantOrbitDrag = bLeftGesture && bAltDown && !bWantPanDrag;
        // LMB+RMB pan is never a tap, so it engages immediately; RMB alone waits to arm.
        const bool bWantLook      = bRightDown && CameraState.bRightLookArmed && !bWantPanDrag;
        const bool bWantPan       = bAllowInput
                                 && CameraState.Mode == EEditorCameraMode::Orbit
                                 && Raw.IsMouseButtonDown(EMouseKey::ButtonMiddle);
        const bool bWantsCaptured = bWantLook || bWantPan || bWantPanDrag || bWantOrbitDrag;

        CameraState.bLeftDragGesture = bWantPanDrag || bWantOrbitDrag;

        if (!bWantOrbitDrag)
        {
            CameraState.bFreeOrbitActive = false;
        }

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
            const bool bWheel = WheelDelta != 0.0;
            // Fly keys only count while a mouse gesture is held, so bWantsCaptured covers every case.
            const bool bMoveInput = bWantsCaptured;

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

            // Nothing to fly into under a parallel projection, so the wheel drives the zoom distance
            // rather than the fly-speed multiplier. Same 10%-per-notch feel as the orbit zoom.
            if (CameraState.bOrthographic && WheelDelta != 0.0)
            {
                const float Zoom = 0.1f * CameraState.OrthoFreeDistance;
                CameraState.OrthoFreeDistance = Math::Max(
                    CameraState.OrthoFreeDistance - static_cast<float>(WheelDelta) * Zoom, 0.05f);
            }

            const FVector3 Forward = Transform.GetForward();
            const FVector3 Right   = Transform.GetRight();
            const FVector3 Up      = Transform.GetUp();

            // Alt+LMB tumbles around a pivot captured once on the gesture's rising edge. It owns the
            // transform for the whole drag, so the fly integration below is skipped entirely.
            if (bWantOrbitDrag)
            {
                if (!CameraState.bFreeOrbitActive)
                {
                    // Free mode has no focal point: frame one out along forward. Borrow the distance
                    // to the last F-focus point when there is one, else the 10.0f FocusViewportToEntity uses.
                    float PivotDistance = 10.0f;
                    if (CameraState.bHasLastFocusPoint)
                    {
                        const float FocusDistance = Math::Distance(Transform.GetLocation(), CameraState.LastFocusPoint);
                        if (FocusDistance > 0.1f)
                        {
                            PivotDistance = FocusDistance;
                        }
                    }

                    CameraState.FreeOrbitPivot       = Transform.GetLocation() + Forward * PivotDistance;
                    CameraState.FreeOrbitDistance    = PivotDistance;
                    CameraState.bFreeOrbitPivotValid = true;
                    CameraState.bFreeOrbitActive     = true;
                    CameraState.Velocity             = FVector3(0.0f);
                }

                const FVector3 Offset = Transform.GetLocation() - CameraState.FreeOrbitPivot;
                const float Distance  = Math::Max(Math::Length(Offset), 0.05f);

                // Spherical form and 0.4 sensitivity match ApplyOrbitTransform so both modes tumble identically.
                float Yaw   = Math::Degrees(std::atan2(Offset.x, Offset.z));
                float Pitch = Math::Degrees(std::asin(Math::Clamp(Offset.y / Distance, -1.0f, 1.0f)));

                Yaw   -= static_cast<float>(Raw.GetMouseDeltaX() * 0.4);
                Pitch += static_cast<float>(Raw.GetMouseDeltaY() * 0.4);
                // Clamp the accumulated elevation, not the per-frame delta, so the camera can't flip past vertical.
                Pitch = Math::Clamp(Pitch, -89.0f, 89.0f);

                const float YawRad   = Math::Radians(Yaw);
                const float PitchRad = Math::Radians(Pitch);
                const FVector3 NewOffset(
                    Distance * std::cos(PitchRad) * std::sin(YawRad),
                    Distance * std::sin(PitchRad),
                    Distance * std::cos(PitchRad) * std::cos(YawRad));

                const FVector3 NewLocation = CameraState.FreeOrbitPivot + NewOffset;
                Transform.SetLocation(NewLocation);
                Transform.SetRotation(Math::FindLookAtRotation(CameraState.FreeOrbitPivot, NewLocation));
                CameraState.FreeOrbitDistance = Distance;
                return;
            }

            float Speed = CameraState.Speed;
            if (ImGui::IsKeyDown(ImGuiKey_LeftShift))
            {
                Speed *= 10.0f;
            }

            // WASDQE flies only while the right mouse button is held (UE-style), so the
            // W/E/R gizmo hotkeys and Q/E don't shove the camera around. The LMB+RMB pan keeps
            // the fly keys live, since the right button never left the mouse.
            // Q/E rise and fall along world vertical, not the camera's own up. Using the camera's up
            // means a pitched view slides you forward or back while you are only asking for height,
            // and looking straight down makes the keys do nothing useful at all.
            const FVector3 WorldUp(0.0f, 1.0f, 0.0f);

            FVector3 Acceleration(0.0f);
            if (bWantLook || bWantPanDrag)
            {
                if (ImGui::IsKeyDown(ImGuiKey_W)) Acceleration += Forward;
                if (ImGui::IsKeyDown(ImGuiKey_S)) Acceleration -= Forward;
                if (ImGui::IsKeyDown(ImGuiKey_D)) Acceleration += Right;
                if (ImGui::IsKeyDown(ImGuiKey_A)) Acceleration -= Right;
                if (ImGui::IsKeyDown(ImGuiKey_E)) Acceleration += WorldUp;
                if (ImGui::IsKeyDown(ImGuiKey_Q)) Acceleration -= WorldUp;
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

            // LMB+RMB grabs the world. Signs match the orbit MMB pan so both modes feel the same;
            // the scale tracks the tumble pivot when one exists, else the focus distance convention
            // scaled by the fly-speed multiplier so it stays sane at any working scale.
            if (bWantPanDrag)
            {
                float PanReference = 10.0f * CameraState.SpeedScale;
                if (CameraState.bFreeOrbitPivotValid)
                {
                    PanReference = CameraState.FreeOrbitDistance;
                }

                const float PanScale = PanReference * 0.002f;
                const FVector3 PanDelta = (Up * static_cast<float>(Raw.GetMouseDeltaY()) * PanScale)
                                        + (Right * static_cast<float>(Raw.GetMouseDeltaX()) * PanScale);
                Transform.Translate(PanDelta);

                if (CameraState.bFreeOrbitPivotValid)
                {
                    // Carry the tumble pivot along so a following Alt+LMB doesn't snap the view.
                    CameraState.FreeOrbitPivot += PanDelta;
                }
            }

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

                // In ortho the wheel is already spoken for as zoom, above.
                if (!CameraState.bOrthographic)
                {
                    const double WheelZ = WheelDelta;
                    CameraState.SpeedScale += Math::Pow(1.05f, CameraState.SpeedScale) * static_cast<float>(WheelZ);
                    CameraState.SpeedScale = Math::Clamp(CameraState.SpeedScale, 0.2f, 100.0f);
                }
            }
        }
        else // Orbit
        {
            if (bAllowInput)
            {
                // Alt+LMB drives the same yaw/pitch as RMB, so the tumble is identical in both modes.
                if (bWantLook || bWantOrbitDrag)
                {
                    CameraState.OrbitYaw   -= static_cast<float>(Raw.GetMouseDeltaX() * 0.4);
                    CameraState.OrbitPitch += static_cast<float>(Raw.GetMouseDeltaY() * 0.4);
                    CameraState.OrbitPitch = Math::Clamp(CameraState.OrbitPitch, -89.0f, 89.0f);
                }

                // LMB+RMB pans the orbit target exactly like MMB does.
                if (bWantPan || bWantPanDrag)
                {
                    const float PanScale = CameraState.OrbitDistance * 0.002f;
                    const FVector3 Right = Transform.GetRight();
                    const FVector3 Up    = Transform.GetUp();
                    CameraState.OrbitTarget += Right * static_cast<float>(Raw.GetMouseDeltaX()) * PanScale;
                    CameraState.OrbitTarget += Up    * static_cast<float>(Raw.GetMouseDeltaY()) * PanScale;
                }

                const double WheelZ = WheelDelta;
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

    bool FEditorTool::ShouldSuppressViewportClickInput() const
    {
        if (CameraState.bLeftDragGesture)
        {
            return true;
        }

        // The view gizmo draws before the overlay, so this is current-frame hover state.
        if (ImViewGuizmo::IsOver(ViewGizmoContext) || ImViewGuizmo::IsUsing(ViewGizmoContext)
            || bViewGizmoOrthoHovered)
        {
            return true;
        }

        // Alt is the orbit modifier, so an Alt-held click over the viewport is always camera intent.
        // Suppressing on the modifier alone also covers the press frame itself: ImGui has already
        // latched that click by the time TickEditorCamera resolves the gesture.
        return (bViewportFocused || bViewportHovered) && ImGui::GetIO().KeyAlt;
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

    void FEditorTool::DrawWorldGrid()
    {
        if (!HasEditorCameraControl() || !bWorldGridEnabled)
        {
            return;
        }

        const CViewportGridSettings* Settings = GetDefault<CViewportGridSettings>();

        const float Spacing = Math::Max(Settings->Spacing, 0.01f);
        const float Extent  = Math::Max(Settings->Extent, Spacing);

        // Past the cap the grid coarsens instead of adding lines. Extent and spacing are independent
        // settings, so nothing stops a large extent meeting a fine spacing, and the batcher would take
        // the resulting hundreds of thousands of lines without complaining.
        int32       HalfCount = (int32)(Extent / Spacing);
        const int32 HalfLimit = Math::Max(Settings->MaxLinesPerAxis / 2, 1);
        float       Step      = Spacing;

        if (HalfCount > HalfLimit)
        {
            HalfCount = HalfLimit;
            Step      = Extent / (float)HalfLimit;
        }

        const float Reach = Step * (float)HalfCount;

        // Grid lines go on the immediate path: uniform thickness, single frame, and there can be
        // thousands of them. The three axis lines keep their own thickness, which is raster state, so
        // they stay on the batched path where a per-line thickness still means something.
        const FDebugDrawState*  DrawState = DebugDraw::GetState(World);
        FImmediateLineRenderer* Lines     = DebugDraw::GetLines(World);
        const uint32            GridColor = PackColor(Settings->LineColor);

        for (int32 i = -HalfCount; i <= HalfCount; ++i)
        {
            const float Coord  = (float)i * Step;
            const bool  bIsAxis = (i == 0);

            if (bIsAxis)
            {
                World->DrawLine(FVector3(Coord, 0, -Reach), FVector3(Coord, 0, Reach),
                                FVector4(0.0f, 0.0f, 1.0f, 1.0f), Settings->AxisThickness);
                World->DrawLine(FVector3(-Reach, 0, Coord), FVector3(Reach, 0, Coord),
                                FVector4(1.0f, 0.0f, 0.0f, 1.0f), Settings->AxisThickness);
                continue;
            }

            if (Lines == nullptr)
            {
                continue;
            }

            // Each grid line is axis-aligned, so its own AABB is a tight cull volume -- looking away
            // from most of the grid actually prunes it, which a bounding sphere would never manage.
            if (DebugDraw::ShouldDraw(*DrawState, FAABB(FVector3(Coord, 0.0f, -Reach), FVector3(Coord, 0.0f, Reach))))
            {
                Lines->Line(FVector3(Coord, 0, -Reach), FVector3(Coord, 0, Reach), GridColor);
            }

            if (DebugDraw::ShouldDraw(*DrawState, FAABB(FVector3(-Reach, 0.0f, Coord), FVector3(Reach, 0.0f, Coord))))
            {
                Lines->Line(FVector3(-Reach, 0, Coord), FVector3(Reach, 0, Coord), GridColor);
            }
        }

        if (Settings->bShowVerticalAxis)
        {
            // Reaches as far as the grid does. It used to use the line count rather than a distance,
            // so it only matched the grid while spacing happened to be 1.
            World->DrawLine(
                FVector3(0, -Reach, 0),
                FVector3(0,  Reach, 0),
                FVector4(0.0f, 1.0f, 0.0f, 1.0f),
                Settings->VerticalAxisThickness);
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
        DEBUG_ASSERT(Algo::NoneOf(ToolWindows.begin(), ToolWindows.end(), [&](const TUniquePtr<FToolWindow>& W)
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

        // EndDisabled must NOT run here when the menu opened. BeginMenu returning true has already pushed
        // the submenu WINDOW, which recorded the disabled-stack depth at its Begin; popping the scope now
        // would pop it inside that window, and EndMenu -> EndPopup -> End would then see a mismatched
        // stack and trip ImGui's error recovery. Closed after EndMenu instead, back in the window that
        // opened the scope. (No visual cost: a disabled BeginMenu cannot open, so bOpen implies !bDisabled.)
        if (!bOpen)
        {
            ImGui::EndDisabled();
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
        ImGui::EndDisabled();
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

    void FEditorTool::BeginTransformTransaction(const TVector<entt::entity>& Entities)
    {
        if (!CanTransact())
        {
            return;
        }

        TransactionManager.BeginTransaction(FName());
        TransactionManager.Record(MakeUnique<FEntityTransformCommand>(World, Entities));
    }

    void FEditorTool::BeginCreationTransaction()
    {
        if (!CanTransact())
        {
            return;
        }

        TransactionManager.BeginTransaction(FName());
        TransactionManager.Record(MakeUnique<FEntityCreationCommand>(World));
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

    bool FEditorTool::BuildViewportRay(ImVec2 ScreenPos, FVector3& OutOrigin, FVector3& OutDirection) const
    {
        if (World == nullptr)
        {
            return false;
        }

        const SCameraComponent* Camera = World->GetActiveCamera();
        if (Camera == nullptr && EditorEntity != entt::null && World->IsValidEntity(EditorEntity))
        {
            Camera = World->TryGetComponent<SCameraComponent>(EditorEntity);
        }
        if (Camera == nullptr)
        {
            return false;
        }

        // Same construction the terrain sculpt cursor uses, including the ImGui Y-flip; anything else and
        // the result lands mirrored vertically about the viewport center.
        const ImVec2 Size = ImVec2(Math::Max(ViewportScreenSize.x, 1.0f), Math::Max(ViewportScreenSize.y, 1.0f));
        const float  Sx   = ((ScreenPos.x - ViewportScreenMin.x) / Size.x) * 2.0f - 1.0f;
        const float  Sy   = 1.0f - ((ScreenPos.y - ViewportScreenMin.y) / Size.y) * 2.0f;

        const FViewVolume& View    = Camera->GetViewVolume();
        const FVector3     Forward = View.GetForwardVector();
        const FVector3     Up      = View.GetUpVector();
        const FVector3     Right   = Math::Normalize(Math::Cross(Up, Forward));

        const float AspectRatio = Size.x / Size.y;

        // Ortho rays are parallel, so the pixel selects the ORIGIN rather than the direction.
        if (View.IsOrthographic())
        {
            const float HalfWidth  = View.GetOrthoWidth() * 0.5f;
            const float HalfHeight = HalfWidth / Math::Max(AspectRatio, 0.001f);

            OutOrigin    = Camera->GetPosition() + Right * (Sx * HalfWidth) + Up * (Sy * HalfHeight);
            OutDirection = Forward;
            return true;
        }

        const float TanHalfFov = std::tan(Math::Radians(View.GetFOV()) * 0.5f);

        OutOrigin    = Camera->GetPosition();
        OutDirection = Math::Normalize(Forward
                                     + Right * (Sx * TanHalfFov * AspectRatio)
                                     + Up    * (Sy * TanHalfFov));
        return true;
    }

    bool FEditorTool::TraceViewportPlacement(ImVec2 ScreenPos, FVector3& OutLocation, entt::entity* OutHitEntity) const
    {
        if (OutHitEntity != nullptr)
        {
            *OutHitEntity = entt::null;
        }

        if (World == nullptr)
        {
            return false;
        }

        const SCameraComponent* Camera = World->GetActiveCamera();
        if (Camera == nullptr && EditorEntity != entt::null && World->IsValidEntity(EditorEntity))
        {
            Camera = World->TryGetComponent<SCameraComponent>(EditorEntity);
        }
        if (Camera == nullptr)
        {
            return false;
        }

        FVector3 RayOrigin, RayDir;
        if (!BuildViewportRay(ScreenPos, RayOrigin, RayDir))
        {
            return false;
        }

        constexpr float FallbackDistance = 5.0f;
        float BestDistance = FLT_MAX;
        bool  bHit         = false;

        // 1. Terrain, via the heightmap raycast the sculpt tools already use. This is the case that
        //    matters most -- a landscape has no collision, so nothing else can hit it accurately.
        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);
        for (auto&& [Entity, Terrain] : Registry.view<STerrainComponent>().each())
        {
            FVector3 TerrainOrigin(0.0f);
            if (const STransformComponent* TerrainTransform = Registry.try_get<STransformComponent>(Entity))
            {
                TerrainOrigin = TerrainTransform->GetLocation();
            }

            FVector3 Hit;
            if (FTerrainSculptSystem::Raycast(Terrain, TerrainOrigin, RayOrigin, RayDir, Hit))
            {
                const float Distance = Math::Distance(RayOrigin, Hit);
                if (Distance < BestDistance)
                {
                    BestDistance = Distance;
                    OutLocation  = Hit;
                    bHit         = true;
                }
            }
        }

        // 2. Mesh entities, against the bounding sphere the resolve cache already caches on the
        //    component. Coarse -- the hit sits on the sphere, not the surface -- but it puts the asset
        //    on the thing you pointed at, which is the whole point. No physics needed.
        //
        //    Static and skeletal both: the cached bounds live on the shared SMeshComponent base, and a
        //    drop that ignored skeletal meshes could never land on a character -- which is exactly what
        //    an animation drop has to hit to mean anything.
        auto TraceMeshView = [&](auto View)
        {
            for (auto&& [Entity, Mesh, Transform] : View.each())
            {
                if (Mesh.CachedLocalRadius <= 0.0f)
                {
                    continue;
                }

                // World-space bounding sphere. Composed from the transform's own basis rather than a
                // transform-point helper, which VTransform does not expose.
                const FTransform& WorldXform = Transform.GetWorldTransform();
                const FVector3    Scale      = WorldXform.GetScale();
                const FVector3    Local      = Mesh.CachedLocalCenter;
                const FVector3    Center     = WorldXform.GetLocation()
                                             + WorldXform.GetRight()   * (Local.x * Scale.x)
                                             + WorldXform.GetUp()      * (Local.y * Scale.y)
                                             + WorldXform.GetForward() * (Local.z * Scale.z);
                const float    Radius = Mesh.CachedLocalRadius * Math::Max(Scale.x, Math::Max(Scale.y, Scale.z));

                // Ray-sphere: solve |Origin + t*Dir - Center|^2 = Radius^2 for the nearer positive root.
                const FVector3 ToCenter = Center - RayOrigin;
                const float    Along    = Math::Dot(ToCenter, RayDir);
                const float    DistSq   = Math::Dot(ToCenter, ToCenter) - Along * Along;
                const float    RadiusSq = Radius * Radius;
                if (DistSq > RadiusSq)
                {
                    continue;
                }

                const float Back = std::sqrt(RadiusSq - DistSq);
                const float T    = Along - Back;
                if (T <= 0.0f || T >= BestDistance)
                {
                    continue;   // behind the camera, or something nearer already won
                }

                BestDistance = T;
                OutLocation  = RayOrigin + RayDir * T;
                bHit         = true;
                if (OutHitEntity != nullptr)
                {
                    *OutHitEntity = Entity;
                }
            }
        };

        TraceMeshView(Registry.view<SStaticMeshComponent, STransformComponent>());
        TraceMeshView(Registry.view<SSkeletalMeshComponent, STransformComponent>());

        if (bHit)
        {
            return true;
        }

        // 3. Ground plane. Covers an empty world, where "on the floor" beats "floating in the air".
        //    Only when the ray actually descends, or a camera tilted up would place behind the viewer.
        if (RayDir.y < -1e-4f)
        {
            const float T = -RayOrigin.y / RayDir.y;
            if (T > 0.0f && T < 1000.0f)
            {
                OutLocation = RayOrigin + RayDir * T;
                return true;
            }
        }

        // 4. Nothing to hit: keep the old behavior, but along the CURSOR ray rather than straight
        //    ahead, so the asset still lands where the user pointed.
        OutLocation = RayOrigin + RayDir * FallbackDistance;
        return true;
    }

    entt::entity FEditorTool::HandleContentBrowserAssetDrop(FStringView VirtualPath, entt::entity DropTarget, bool bAttachToTarget)
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

        // Graph load, not the inline one: a dropped prefab pulls in its whole closure (a large one is
        // dozens of meshes plus their materials and textures), and the inline path resolves every import
        // depth-first on this thread from inside the parent's Serialize. Already-resident assets skip the
        // BFS and the graph loader's mutex entirely -- same fast path StaticLoadObject takes.
        CObject* Loaded = FindObject<CObject>(AssetData->AssetGUID);
        if (Loaded == nullptr)
        {
            Loaded = LoadObjectGraph<CObject>(AssetData->AssetGUID);
        }
        if (Loaded == nullptr)
        {
            LOG_WARN("Asset drop: failed to load '{}'", FString(VirtualPath.data(), VirtualPath.size()).c_str());
            return entt::null;
        }

        // Place where the cursor points, not straight ahead. TraceViewportPlacement always yields a
        // usable point, so the camera-relative transform is only for the no-camera case.
        FTransform SpawnTransform = GetCameraSpawnTransform();
        FVector3   TracedLocation;
        if (TraceViewportPlacement(ImGui::GetMousePos(), TracedLocation))
        {
            SpawnTransform.SetLocation(TracedLocation);
        }

        return (*Handler)(World, Loaded, SpawnTransform, DropTarget, bAttachToTarget);
    }
}
