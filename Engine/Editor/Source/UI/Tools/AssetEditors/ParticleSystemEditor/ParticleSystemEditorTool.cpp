#include "ParticleSystemEditorTool.h"
#include "ParticleParamCustomization.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/PropertyChangedEvent.h"
#include "Particles/ParticleEmitterStack.h"
#include "Particles/ParticleGridModules.h"
#include "Particles/ParticleGraphModules.h"
#include "Assets/AssetTypes/Textures/TextureRenderTarget.h"
#include "Particles/ParticleOps.h"
#include "Particles/ParticleStockModules.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"
#include "UI/Tools/Transactions/ObjectSnapshotCommand.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "UI/Tools/EditorAssetActions.h"
#include "UI/Tools/EditorToolContext.h"
#include "Renderer/RenderResource.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "Containers/StringFormat.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/EditorColors.h"

namespace Lumina
{
    static const char* EmitterWindowName    = "Emitter";
    static const char* SelectionWindowName  = "Details";
    static const char* ModuleGraphWindowName = "Module Graph";

    // A module or stack lives in its emitter's package, and a shared emitter asset in its own rather than the system's.
    static void MarkEdited(const CObject* Object)
    {
        if (CPackage* Package = Object != nullptr ? Object->GetPackage() : nullptr)
        {
            Package->MarkDirty();
        }
    }

    // Built lazily so reflected classes are registered by the time it runs. Add new stock modules here.

    FParticleSystemEditorTool::FParticleSystemEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
        , ParticleEntity()
        , DirectionalLightEntity()
        , System(Cast<CParticleEmitter>(InAsset) != nullptr ? ParticleOps::WrapEmitter(Cast<CParticleEmitter>(InAsset)) : TStrongObjectPtr<CParticleSystem>(Cast<CParticleSystem>(InAsset)))
    {
    }

    bool FParticleSystemEditorTool::IsEmitterAsset() const
    {
        return Asset.Get() != (CObject*)System.Get();
    }

    // Idle time between a one-shot's last particle dying and the looped preview firing it again.
    static constexpr float PreviewReplayPause = 0.6f;

    void FParticleSystemEditorTool::SetupWorldForTool()
    {
        FAssetEditorTool::SetupWorldForTool();

        DirectionalLightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SSkyLightComponent>(DirectionalLightEntity);

        ParticleEntity = World->ConstructEntity("Particle System");
        SParticleSystemComponent& ParticleComponent = World->EmplaceComponent<SParticleSystemComponent>(ParticleEntity);
        ParticleComponent.ParticleSystem = GetSystem();

        STransformComponent& ParticleTransform = World->GetComponent<STransformComponent>(ParticleEntity);
        STransformComponent& EditorTransform   = World->GetComponent<STransformComponent>(EditorEntity);
        const FQuat LookRotation = Math::FindLookAtRotation(ParticleTransform.GetLocation(), EditorTransform.GetLocation());
        EditorTransform.SetRotation(LookRotation);
    }

    void FParticleSystemEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        CreateToolWindow(EmitterWindowName, [&](bool bFocused)
        {
            DrawStack();
        });

        CreateToolWindow(ModuleGraphWindowName, [&](bool bFocused)
        {
            DrawModuleGraph();
        });

        CreateToolWindow(SelectionWindowName, [&](bool bFocused)
        {
            // A property handle carries no owner, so the system is announced around the draw instead.
            ParticleParamContext::FScope ParamScope(GetSystem());
            GetPropertyTable()->DrawTree();
        });

        SyncEmitterStacks();
        // Nothing queues an emitter asset that no system has loaded, so it compiles when opened.
        if (IsEmitterAsset() && !GetSystem()->Emitters[0]->UsesCustomShader())
        {
            Compile();
        }

        // The base pair snapshots Asset while this panel points at the selected module, so both are replaced.
        GetPropertyTable()->SetStartEditCallback([this](const FPropertyChangedEvent& Event)
        {
            CObject* Target = (SelectedObject != nullptr) ? SelectedObject : Asset.Get();
            if (Target != nullptr)
            {
                FTransactionManager& Manager = GetTransactionManager();
                Manager.BeginTransaction(Event.PropertyName);
                Manager.Record(MakeUnique<FObjectSnapshotCommand>(Target, Event.PropertyName));
            }
        });

        // A system-setting edit feeds the sim uniforms directly and needs no recompile.
        GetPropertyTable()->SetFinishEditCallback([this](const FPropertyChangedEvent&)
        {
            // Commit first, since the after-image has to be captured before any recompile.
            GetTransactionManager().CommitTransaction();

            CObject* Edited = SelectedObject != nullptr ? SelectedObject : Asset.Get();
            if (Edited != nullptr && Edited->GetPackage() != nullptr)
            {
                Edited->GetPackage()->MarkDirty();
            }

            if (SelectedModule == nullptr && Cast<CEdGraphNode>(SelectedObject) == nullptr)
            {
                return;   // system-wide settings feed the sim uniforms directly
            }

            // A buffer rewrite keeps particles and spawn state, so only a layout change forces a rebuild.
            if (!RefreshModuleParams())
            {
                Compile();
            }
        });

        // Default the Details panel to the system-wide settings.
        SelectModule(nullptr);
    }

    void FParticleSystemEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        const bool bAdvancing = !bPreviewPaused || bStepPending;
        bStepPending = false;
        if (World.IsValid() && World->IsValidEntity(ParticleEntity))
        {
            if (SParticleSystemComponent* Component = World->TryGetComponent<SParticleSystemComponent>(ParticleEntity))
            {
                Component->TimeScale = bAdvancing ? PreviewSpeed : 0.0f;
            }
        }

        PreviewAge += bAdvancing ? (float)UpdateContext.GetDeltaTime() * PreviewSpeed : 0.0f;
        if (bShowGizmos)
        {
            DrawEmitterGizmos();
        }
        if (bLoopPreview)
        {
            const CParticleSystem* PS = GetSystem();
            const float OneShotLength = PS != nullptr ? PS->GetOneShotLength() : 0.0f;
            if (OneShotLength > 0.0f && PreviewAge >= OneShotLength + PreviewReplayPause)
            {
                ReplayPreview();
            }
        }

        // Re-applied every frame, since idle reclaim can rebuild the render scene and its settings.
        if (World.IsValid() && World->GetRenderer() != nullptr)
        {
            World->GetRenderSettings().bDrawBillboards = false;
        }
    }

    void FParticleSystemEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        EmitterStacks.clear();
        SelectedObject = nullptr;
        SelectedModule = nullptr;
    }

    void FParticleSystemEditorTool::SyncEmitterStacks()
    {
        CParticleSystem* PS = GetSystem();
        if (PS == nullptr)
        {
            return;
        }

        // These may be the only strong refs, and dropping them first would destroy every module and reload it from disk.
        TVector<TStrongObjectPtr<CParticleEmitterStack>> Previous = Move(EmitterStacks);
        EmitterStacks.clear();
        EmitterStacks.reserve(PS->Emitters.size());

        for (int32 i = 0; i < (int32)PS->Emitters.size(); ++i)
        {
            EmitterStacks.push_back(ParticleOps::FindOrCreateStack(PS, i));
        }

        if (SelectedEmitter >= (int32)EmitterStacks.size())
        {
            SelectedEmitter = 0;
        }
    }

    void FParticleSystemEditorTool::RefreshModulePalette()
    {
        ModulePalette = ParticleOps::GetModuleTypes();
    }

    CParticleEmitterStack* FParticleSystemEditorTool::GetStackFor(int32 EmitterIndex) const
    {
        if (EmitterIndex < 0 || EmitterIndex >= (int32)EmitterStacks.size())
        {
            return nullptr;
        }
        return EmitterStacks[(size_t)EmitterIndex].Get();
    }

    void FParticleSystemEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Emitters",
            "A system is a list of emitters shown side by side, each simulating and drawing on its own. "
            "An explosion is typically a flash, smoke, sparks and debris as four emitters. Use the "
            "checkbox to mute one while tuning the others.");
        DrawHelpTextRow("Emitter Assets",
            "Add Emitter Asset, or dropping one from the content browser onto the emitter list, shares a Particle "
            "Emitter asset, marked with a link, so edits to it reach every system using it. Right-click an emitter's header to save it as an asset, make a shared one local, "
            "or open the asset on its own.");
        DrawHelpTextRow("Stack",
            "Each emitter is built from a Spawn stack (runs once per particle) and an Update stack "
            "(runs every frame). Add modules with the + buttons and reorder them, order matters.");
        DrawHelpTextRow("Modules",
            "Each module is one behavior (shape, velocity, gravity, color over life, ...). Select a "
            "module to edit its inputs in the Details panel. Toggle the checkbox to disable it.");
        DrawHelpTextRow("Compile",
            "Compile bakes the stacks into the asset's compute shader. Save also compiles. "
            "Place 'Solve Forces and Velocity' last in Update so all forces are applied first.");
        DrawHelpTextRow("Preview",
            "The viewport spawns the system at the origin. Select an emitter's header for its spawn "
            "rate, particle budget and render settings; select System Settings for user parameters.");
    }

    void FParticleSystemEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        if (ImGui::MenuItem(LE_ICON_RECEIPT_TEXT" Compile"))
        {
            Compile();
        }

        if (ImGui::MenuItem(LE_ICON_REPLAY" Replay"))
        {
            ReplayPreview();
        }

        if (ImGui::MenuItem(bPreviewPaused ? LE_ICON_PLAY " Resume" : LE_ICON_PAUSE " Pause"))
        {
            bPreviewPaused = !bPreviewPaused;
        }

        if (ImGui::MenuItem(LE_ICON_STEP_FORWARD " Step", nullptr, false, bPreviewPaused))
        {
            bStepPending = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Advance the paused preview by one frame.");
        }

        constexpr float Speeds[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f };
        if (ImGui::BeginMenu(Format(LE_ICON_PLAY_PAUSE " Speed {}x", PreviewSpeed).c_str()))
        {
            for (float Speed : Speeds)
            {
                if (ImGui::MenuItem(Format("{}x", Speed).c_str(), nullptr, PreviewSpeed == Speed))
                {
                    PreviewSpeed = Speed;
                }
            }
            ImGui::EndMenu();
        }

        ImGui::MenuItem(LE_ICON_REPEAT" Loop Preview", nullptr, &bLoopPreview);
        ImGui::MenuItem(LE_ICON_SHAPE_OUTLINE" Gizmos", nullptr, &bShowGizmos);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Re-fire a one-shot system once its particles have expired. Streaming emitters are left alone.");
        }
    }

    void FParticleSystemEditorTool::DrawEmitterGizmos()
    {
        const CParticleSystem* PS = GetSystem();
        if (PS == nullptr || !World.IsValid() || !World->IsValidEntity(ParticleEntity)
            || SelectedEmitter < 0 || SelectedEmitter >= (int32)PS->Emitters.size() || PS->Emitters[SelectedEmitter] == nullptr)
        {
            return;
        }

        const CParticleEmitter* Emitter = PS->Emitters[SelectedEmitter].Get();
        const FVector3 Origin = World->GetComponent<STransformComponent>(ParticleEntity).GetLocation();
        const FVector4 ShapeColor(0.3f, 0.8f, 1.0f, 1.0f);
        const FVector4 BoundsColor(1.0f, 0.75f, 0.2f, 1.0f);

        if (Emitter->VisibilityRadius > 0.0f)
        {
            World->GetDebugInterface()->DrawSphere(Origin, Emitter->VisibilityRadius, BoundsColor, {}, {}, {});
        }

        const CParticleEmitterStack* Stack = GetStackFor(SelectedEmitter);
        if (Stack == nullptr)
        {
            return;
        }

        for (const TStrongObjectPtr<CParticleModule>& Module : Stack->SpawnModules)
        {
            const CParticleModule_SpawnLocation* Location = Cast<CParticleModule_SpawnLocation>(Module.Get());
            if (Location == nullptr || !Location->bEnabled)
            {
                continue;
            }

            const FVector3 Size = Location->ShapeSize.AsVector3();
            switch (Location->Shape)
            {
            case EParticleEmitterShape::Sphere:
            case EParticleEmitterShape::Hemisphere:
                World->GetDebugInterface()->DrawSphere(Origin, Size.x, ShapeColor, {}, {}, {});
                break;
            case EParticleEmitterShape::Box:
                World->GetDebugInterface()->DrawBox(Origin, Size, FQuat::Identity(), ShapeColor, {}, {}, {});
                break;
            case EParticleEmitterShape::Cone:
                World->GetDebugInterface()->DrawCone(Origin, FVector3(0.0f, 1.0f, 0.0f), Math::Atan2(Size.x, Math::Max(Size.y, 1e-4f)), Size.y, ShapeColor, {}, {}, {});
                break;
            case EParticleEmitterShape::Ring:
            case EParticleEmitterShape::Disk:
            {
                constexpr int32 Segments = 32;
                for (float Radius : { Size.x, Size.y })
                {
                    for (int32 Index = 0; Index < Segments && Radius > 0.0f; ++Index)
                    {
                        const float A = Math::TwoPi<float>() * (float)Index / (float)Segments;
                        const float B = Math::TwoPi<float>() * (float)(Index + 1) / (float)Segments;
                        World->GetDebugInterface()->DrawLine(Origin + FVector3(Math::Cos(A), 0.0f, Math::Sin(A)) * Radius,
                                        Origin + FVector3(Math::Cos(B), 0.0f, Math::Sin(B)) * Radius, ShapeColor, {}, {}, {});
                    }
                }
                break;
            }
            default:
                break;
            }
            break;
        }
    }

    void FParticleSystemEditorTool::ReplayPreview()
    {
        PreviewAge = 0.0f;
        if (World.IsValid() && World->IsValidEntity(ParticleEntity))
        {
            World->GetComponent<SParticleSystemComponent>(ParticleEntity).Activate(true);
        }
    }

    void FParticleSystemEditorTool::SelectObject(CObject* Object)
    {
        SelectedObject = Object;
        // Only a module drives the recompile-on-edit path; an emitter or the system feeds uniforms.
        SelectedModule = Cast<CParticleModule>(Object);
        if (Cast<CParticleModule_Graph>(Object) != nullptr)
        {
            GraphModule        = SelectedModule;
            GraphModuleEmitter = SelectedEmitter;
        }

        CObject* Target = (Object != nullptr) ? Object : Asset.Get();
        GetPropertyTable()->SetObject(Target, Target->GetClass());
    }

    void FParticleSystemEditorTool::DrawStack()
    {
        CParticleSystem* PS = GetSystem();
        if (PS == nullptr)
        {
            return;
        }

        // Emitters can be added or removed by undo, so the stacks reconcile every frame.
        if (EmitterStacks.size() != PS->Emitters.size())
        {
            SyncEmitterStacks();
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 5));

        if (CompilationResult.bIsError)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Danger());
            ImGui::TextWrapped(LE_ICON_ALERT_CIRCLE_OUTLINE " %s", CompilationResult.CompilationLog.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        // System header selects the asset so its user parameters show in Details.
        if (!IsEmitterAsset())
        {
            const bool bSystemSelected = SelectedObject == nullptr;
            ImGui::PushStyleColor(ImGuiCol_Header, EditorColors::RowBgActive());
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorColors::RowBgHovered());
            ImGui::PushStyleColor(ImGuiCol_Text, bSystemSelected ? EditorColors::TextPrimary() : EditorColors::TextDim());
            if (ImGui::Selectable(LE_ICON_COG " System Settings", bSystemSelected))
            {
                SelectObject(nullptr);
            }
            ImGui::PopStyleColor(3);
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
        }

        // This panel docks tall and narrow, so fixed-width columns truncated every module name.
        ImGui::BeginChild("##emitters", ImVec2(0.0f, 0.0f));

        for (int32 i = 0; i < (int32)PS->Emitters.size(); ++i)
        {
            ImGui::PushID(i);
            DrawEmitterSection(i);
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
        }

        if (!IsEmitterAsset())
        {
            DrawAddEmitterButtons();
        }

        ImGui::EndChild();

        if (!IsEmitterAsset() && ImGui::BeginDragDropTarget())
        {
            AddSharedEmitter(DragDrop::AcceptAsset<CParticleEmitter>());
            ImGui::EndDragDropTarget();
        }

        if (PendingRemoveEmitter != nullptr)
        {
            if (SelectedObject == (CObject*)PendingRemoveEmitter)
            {
                SelectObject(nullptr);
            }
            // Orphaned rather than destroyed, since it holds the authoring data and is re-adopted by name.
            PS->RemoveEmitter(PendingRemoveEmitter);
            PendingRemoveEmitter = nullptr;
            SyncEmitterStacks();
            MarkEdited(PS);
        }

        ImGui::PopStyleVar(2);

        // Recompile after the UI loop, and only the emitter that actually changed.
        if (DirtyEmitter != Constants::kIndexNone)
        {
            const int32 Target = DirtyEmitter;
            DirtyEmitter = Constants::kIndexNone;
            CompilationResult = FCompilationResultInfo();
            CompileEmitter(Target);
            MarkEdited(PS->Emitters[Target].Get());
        }
    }

    void FParticleSystemEditorTool::DrawAddEmitterButtons()
    {
        CParticleSystem* PS = GetSystem();
        const float Width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button(LE_ICON_PLUS" Add Emitter", ImVec2(Width, 0.0f)))
        {
            if (CParticleEmitter* Added = PS->AddEmitter())
            {
                SyncEmitterStacks();
                SelectedEmitter = (int32)PS->Emitters.size() - 1;
                SelectObject(Added);
                MarkEdited(PS);
                DirtyEmitter = SelectedEmitter;
            }
        }
        ImGui::SameLine();

        EmitterAssetPicker.SearchHint = "Search emitter assets...";
        EmitterAssetPicker.ItemNoun   = "emitters";
        const FPickerItem* Picked = EmitterAssetPicker.DrawButton("##AddEmitterAsset", LE_ICON_PLUS" Add Emitter Asset", [](TVector<FPickerItem>& Items)
        {
            const FName EmitterClass = CParticleEmitter::StaticClass()->GetName();
            for (const FAssetData* Data : FAssetRegistry::Get().FindByPredicate([&](const FAssetData& Asset) { return Asset.AssetClass == EmitterClass; }))
            {
                const FStringView Path(Data->Path.c_str(), Data->Path.size());
                FPickerItem& Item = Items.emplace_back();
                Item.Name     = Data->AssetName.c_str();
                Item.Key      = Data->AssetGUID.ToString();
                const FStringView Folder = VFS::Parent(Path, true);
                Item.Category = FString(Folder.data(), Folder.size());
                Item.Tooltip  = FString(Path.data(), Path.size());
                Item.Icon     = LE_ICON_SHIMMER;
            }
        }, Width);

        if (Picked != nullptr)
        {
            AddSharedEmitter(Cast<CParticleEmitter>(LoadObject<CObject>(FGuid::FromString(FStringView(Picked->Key.c_str(), Picked->Key.size())))));
        }
    }

    void FParticleSystemEditorTool::AddSharedEmitter(CParticleEmitter* Shared)
    {
        CParticleSystem* PS = GetSystem();
        if (Shared != nullptr)
        {
            PS->Emitters.push_back(Shared);
            SyncEmitterStacks();
            SelectedEmitter = (int32)PS->Emitters.size() - 1;
            SelectObject(Shared);
            MarkEdited(PS);
            DirtyEmitter = Shared->UsesCustomShader() ? Constants::kIndexNone : SelectedEmitter;
        }
    }

    void FParticleSystemEditorTool::DrawEmitterAssetMenu(int32 EmitterIndex, CParticleEmitter* Emitter)
    {
        CParticleSystem* PS = GetSystem();
        CParticleEmitter* Replacement = nullptr;
        if (Emitter->GetPackage() != PS->GetPackage())
        {
            if (ImGui::MenuItem(LE_ICON_OPEN_IN_NEW" Open Emitter Asset"))
            {
                ToolContext->OpenAssetEditor(Emitter->GetGUID());
            }
            if (ImGui::MenuItem(LE_ICON_CONTENT_COPY" Make Local"))
            {
                Replacement = ParticleOps::MakeEmitterLocal(PS, EmitterIndex);
            }
        }
        else if (ImGui::MenuItem(LE_ICON_CONTENT_SAVE" Save as Emitter Asset"))
        {
            FString Suffix = "_" + Emitter->EmitterName;
            for (char& Char : Suffix)
            {
                Char = std::isalnum((unsigned char)Char) ? Char : '_';
            }
            const FFixedString SystemPath = PS->GetPackage()->GetPackagePath();
            const FFixedString Path = MakeSiblingAssetPath(FStringView(SystemPath.c_str(), SystemPath.size()), Suffix.c_str());
            Replacement = ParticleOps::SaveEmitterAsAsset(PS, EmitterIndex, FStringView(Path.c_str(), Path.size()));
            if (Replacement != nullptr)
            {
                ImGuiX::Notifications::NotifySuccess("Saved '{0}' as '{1}'.", Emitter->EmitterName.c_str(), Path.c_str());
            }
            else
            {
                ImGuiX::Notifications::NotifyError("Could not save '{0}' as an emitter asset.", Emitter->EmitterName.c_str());
            }
        }

        if (Replacement != nullptr)
        {
            SyncEmitterStacks();
            SelectObject(Replacement);
            MarkEdited(PS);
        }
    }

    // An icon with no frame of its own, lit only under the cursor, so actions read as part of the row they sit on.
    static bool StackIcon(const char* Icon, const char* Id, const ImVec4& Color, const ImVec4& HoverColor, const char* Tooltip = nullptr, bool bEnabled = true)
    {
        const float  Size = ImGui::GetFrameHeight();
        const ImVec2 Min  = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(!bEnabled);
        const bool bPressed = ImGui::InvisibleButton(Id, ImVec2(Size, Size));
        const bool bHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        ImGui::EndDisabled();

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        if (bHovered && bEnabled)
        {
            DrawList->AddRectFilled(Min, ImVec2(Min.x + Size, Min.y + Size), EditorColors::U32(EditorColors::Lighten(EditorColors::RowBgHovered(), 0.05f)), 4.0f);
        }
        const ImVec4 Tint = !bEnabled ? EditorColors::WithAlpha(Color, 0.3f) : bHovered ? HoverColor : Color;
        DrawList->AddText(ImGuiX::CenteredIconPos(Icon, Min, ImVec2(Min.x + Size, Min.y + Size)), EditorColors::U32(Tint), Icon);
        if (bHovered && Tooltip != nullptr)
        {
            ImGui::SetTooltip("%s", Tooltip);
        }
        return bPressed;
    }

    // A dot, filled while on, which says less than a checkbox in a list where nearly everything stays on.
    static bool EnableDot(const char* Id, bool& bValue, const char* Tooltip)
    {
        const float  Size = ImGui::GetFrameHeight();
        const ImVec2 Min  = ImGui::GetCursorScreenPos();
        const bool bPressed = ImGui::InvisibleButton(Id, ImVec2(Size * 0.75f, Size));
        const bool bHovered = ImGui::IsItemHovered();
        if (bPressed)
        {
            bValue = !bValue;
        }

        const ImVec2 Center(Min.x + Size * 0.375f, Min.y + Size * 0.5f);
        const float  Radius = Size * 0.16f;
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        if (bHovered)
        {
            DrawList->AddCircleFilled(Center, Radius + 4.0f, EditorColors::U32(EditorColors::WithAlpha(EditorColors::Accent(), 0.18f)));
            ImGui::SetTooltip("%s", Tooltip);
        }
        if (bValue)
        {
            DrawList->AddCircleFilled(Center, Radius, EditorColors::U32(EditorColors::Accent()));
        }
        else
        {
            DrawList->AddCircle(Center, Radius, EditorColors::U32(EditorColors::TextMuted()), 0, 1.5f);
        }
        return bPressed;
    }

    void FParticleSystemEditorTool::DrawEmitterSection(int32 EmitterIndex)
    {
        CParticleSystem* PS = GetSystem();
        CParticleEmitter* Emitter = (PS != nullptr && EmitterIndex < (int32)PS->Emitters.size())
                                  ? PS->Emitters[EmitterIndex].Get() : nullptr;
        if (Emitter == nullptr)
        {
            return;
        }

        const bool  bSelected    = (SelectedObject == (CObject*)Emitter);
        const bool  bShared      = !IsEmitterAsset() && Emitter->GetPackage() != PS->GetPackage();
        const float CardWidth    = ImGui::GetContentRegionAvail().x;
        const float HeaderHeight = ImGui::GetFrameHeight() + 8.0f;
        const ImVec2 CardMin     = ImGui::GetCursorScreenPos();
        bool& bOpen = *ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("##open"), true);

        // The card's background has to sit under content whose height is only known once it has drawn.
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        DrawList->ChannelsSplit(2);
        DrawList->ChannelsSetCurrent(1);

        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##header", ImVec2(CardWidth, HeaderHeight)))
        {
            SelectedEmitter = EmitterIndex;
            SelectObject(Emitter);
        }
        const bool bHeaderHovered = ImGui::IsItemHovered();
        if (bHeaderHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            bOpen = !bOpen;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            ImGui::OpenPopup("##EmitterMenu");
        }
        const ImVec2 AfterHeader = ImGui::GetCursorScreenPos();

        const float RowY = CardMin.y + (HeaderHeight - ImGui::GetFrameHeight()) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(CardMin.x + 8.0f, RowY));
        if (StackIcon(bOpen ? LE_ICON_CHEVRON_DOWN : LE_ICON_CHEVRON_RIGHT, "##fold", EditorColors::TextDim(), EditorColors::TextPrimary()))
        {
            bOpen = !bOpen;
        }

        ImGui::SameLine(0.0f, 2.0f);
        bool bEnabled = Emitter->bEnabled;
        if (EnableDot("##enabled", bEnabled, "Mute this emitter. Disabled emitters do not simulate or draw."))
        {
            Emitter->bEnabled = bEnabled;
            MarkEdited(Emitter);
        }

        ImGui::SameLine(0.0f, 4.0f);
        ImGui::AlignTextToFramePadding();
        if (bShared)
        {
            ImGui::TextColored(EditorColors::AccentAlt(), LE_ICON_LINK);
            ImGuiX::TextTooltip("Shared emitter asset {}. Edits here change every system that uses it.", Emitter->GetPackage()->GetPackagePath().c_str());
            ImGui::SameLine(0.0f, 4.0f);
        }
        ImGui::TextColored(bEnabled ? EditorColors::TextPrimary() : EditorColors::TextMuted(), "%s", Emitter->EmitterName.c_str());

        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(CardMin.x + CardWidth - ImGui::GetFrameHeight() - 6.0f, RowY));
        if (StackIcon(LE_ICON_DOTS_VERTICAL, "##options", EditorColors::TextDim(), EditorColors::TextPrimary(), "Move, save or remove this emitter."))
        {
            ImGui::OpenPopup("##EmitterMenu");
        }
        DrawEmitterMenu(EmitterIndex, Emitter);

        ImGui::SetCursorScreenPos(AfterHeader);
        if (bOpen)
        {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::Indent(8.0f);
            DrawStackSection(EmitterIndex, EParticleModuleStage::Spawn,  "Particle Spawn");
            DrawStackSection(EmitterIndex, EParticleModuleStage::Update, "Particle Update");
            DrawStackSection(EmitterIndex, EParticleModuleStage::Grid,   "Simulation Stages");
            DrawStackSection(EmitterIndex, EParticleModuleStage::Target, "Render Target");
            ImGui::Unindent(8.0f);
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
        }

        const ImVec2 CardMax(CardMin.x + CardWidth, ImGui::GetCursorScreenPos().y);
        const ImVec2 HeaderMax(CardMax.x, CardMin.y + HeaderHeight);
        const ImVec4 HeaderColor = bSelected ? EditorColors::Lighten(EditorColors::ComponentHeader(), 0.04f)
                                 : bHeaderHovered ? EditorColors::Lighten(EditorColors::ComponentHeader(), 0.02f)
                                 : EditorColors::ComponentHeader();

        DrawList->ChannelsSetCurrent(0);
        DrawList->AddRectFilled(CardMin, CardMax, EditorColors::U32(EditorColors::PanelBg()), 6.0f);
        DrawList->AddRectFilled(CardMin, HeaderMax, EditorColors::U32(HeaderColor), 6.0f, bOpen ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll);
        DrawList->AddRectFilled(ImVec2(CardMin.x, CardMin.y + 6.0f), ImVec2(CardMin.x + 3.0f, HeaderMax.y - (bOpen ? 0.0f : 6.0f)),
            EditorColors::U32(bEnabled ? EditorColors::Accent() : EditorColors::TextMuted()));
        DrawList->AddRect(CardMin, CardMax, EditorColors::U32(bSelected ? EditorColors::WithAlpha(EditorColors::Accent(), 0.6f) : EditorColors::Border()), 6.0f);
        DrawList->ChannelsMerge();
    }

    void FParticleSystemEditorTool::DrawEmitterMenu(int32 EmitterIndex, CParticleEmitter* Emitter)
    {
        if (!ImGui::BeginPopup("##EmitterMenu"))
        {
            return;
        }

        CParticleSystem* PS = GetSystem();
        if (ImGui::MenuItem(LE_ICON_ARROW_UP " Move Up", nullptr, false, EmitterIndex > 0))
        {
            PS->MoveEmitter(Emitter, -1);
            SyncEmitterStacks();
            MarkEdited(PS);
        }
        if (ImGui::MenuItem(LE_ICON_ARROW_DOWN " Move Down", nullptr, false, EmitterIndex < (int32)PS->Emitters.size() - 1))
        {
            PS->MoveEmitter(Emitter, 1);
            SyncEmitterStacks();
            MarkEdited(PS);
        }

        if (!IsEmitterAsset())
        {
            ImGui::Separator();
            DrawEmitterAssetMenu(EmitterIndex, Emitter);
        }

        ImGui::Separator();
        // A system with no emitters renders nothing and every consumer would need a special case.
        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Danger());
        if (ImGui::MenuItem(LE_ICON_DELETE " Remove Emitter", nullptr, false, PS->Emitters.size() > 1))
        {
            PendingRemoveEmitter = Emitter;
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    void FParticleSystemEditorTool::DrawStackSection(int32 EmitterIndex, EParticleModuleStage Stage, const char* Label)
    {
        CParticleEmitterStack* EmitterStack = GetStackFor(EmitterIndex);
        if (EmitterStack == nullptr)
        {
            return;
        }

        ImGui::PushID(Label);
        TVector<TStrongObjectPtr<CParticleModule>>& Stack = EmitterStack->GetStack(Stage);

        const char* Icon = Stage == EParticleModuleStage::Spawn  ? LE_ICON_STAR_FOUR_POINTS
                         : Stage == EParticleModuleStage::Update ? LE_ICON_REFRESH
                         : Stage == EParticleModuleStage::Grid   ? LE_ICON_GRID
                         :                                         LE_ICON_IMAGE_OUTLINE;

        const ImVec2 HeaderMin = ImGui::GetCursorScreenPos();
        const float  Width     = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(EditorColors::SectionHeader(), "%s  %s", Icon, Label);
        if (!Stack.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(EditorColors::TextMuted(), "%d", (int32)Stack.size());
        }
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(HeaderMin.x + Width - ImGui::GetFrameHeight() - 4.0f, ImGui::GetCursorScreenPos().y));
        if (StackIcon(LE_ICON_PLUS, "##add", EditorColors::TextDim(), EditorColors::Accent(), "Add a module to this section."))
        {
            PendingAddStage   = Stage;
            PendingAddEmitter = EmitterIndex;
            RefreshModulePalette();
            ImGui::OpenPopup("##AddModule");
        }

        const float LineY = ImGui::GetCursorScreenPos().y;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(HeaderMin.x, LineY), ImVec2(HeaderMin.x + Width - 4.0f, LineY), EditorColors::U32(EditorColors::Border()));
        ImGui::Dummy(ImVec2(0.0f, 2.0f));

        // Rows sit close enough that a stage's guide line runs unbroken through the modules under it.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0f));
        CParticleModule* PendingRemove = nullptr;
        bool bUnderStage = false;
        for (int32 i = 0; i < (int32)Stack.size(); ++i)
        {
            CParticleModule* Module = Stack[i].Get();
            if (Module == nullptr)
            {
                continue;
            }

            const CParticleModule_SimulationStage* StageHeader = Cast<CParticleModule_SimulationStage>(Module);
            bUnderStage = StageHeader != nullptr || bUnderStage;

            ImGui::PushID(i);
            const EModuleRowAction Action = DrawModuleRow(EmitterIndex, Module, StageHeader, bUnderStage && StageHeader == nullptr, i > 0, i < (int32)Stack.size() - 1);
            ImGui::PopID();

            if (Action == EModuleRowAction::MoveUp || Action == EModuleRowAction::MoveDown)
            {
                EmitterStack->MoveModule(Module, Action == EModuleRowAction::MoveUp ? -1 : 1);
                MarkEdited(EmitterStack);
                DirtyEmitter = EmitterIndex;
            }
            else if (Action == EModuleRowAction::Remove)
            {
                PendingRemove = Module;
            }
            else if (Action == EModuleRowAction::Toggle)
            {
                MarkEdited(Module);
                DirtyEmitter = EmitterIndex;
            }
        }

        ImGui::PopStyleVar();

        if (Stage == EParticleModuleStage::Target)
        {
            DrawGridTargetPreview(EmitterIndex);
        }

        if (PendingRemove != nullptr)
        {
            if (SelectedModule == PendingRemove)
            {
                SelectObject(nullptr);
            }
            EmitterStack->RemoveModule(PendingRemove);
            MarkEdited(EmitterStack);
            DirtyEmitter = EmitterIndex;
        }

        DrawAddModulePopup(EmitterIndex, Stage);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::PopID();
    }

    FParticleSystemEditorTool::EModuleRowAction FParticleSystemEditorTool::DrawModuleRow(int32 EmitterIndex, CParticleModule* Module,
        const CParticleModule_SimulationStage* StageHeader, bool bInStage, bool bCanMoveUp, bool bCanMoveDown)
    {
        EModuleRowAction Action = EModuleRowAction::None;
        const bool   bSelected = (SelectedModule == Module);
        const float  Height    = ImGui::GetFrameHeight() + 2.0f;
        const ImVec2 Min       = ImGui::GetCursorScreenPos();
        const float  Width     = ImGui::GetContentRegionAvail().x - 4.0f;
        const ImVec2 Max(Min.x + Width, Min.y + Height);

        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##row", ImVec2(Width, Height)))
        {
            SelectedEmitter = EmitterIndex;
            SelectModule(Module);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !Module->GetTooltip().empty())
        {
            ImGui::SetTooltip("%s", Module->GetTooltip().c_str());
        }
        const ImVec2 AfterRow = ImGui::GetCursorScreenPos();
        const bool bHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(Min, Max);

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        if (bSelected || bHovered)
        {
            DrawList->AddRectFilled(Min, Max, EditorColors::U32(bSelected ? EditorColors::RowBgActive() : EditorColors::RowBgHovered()), 4.0f);
        }
        if (bSelected)
        {
            DrawList->AddRectFilled(ImVec2(Min.x, Min.y + 3.0f), ImVec2(Min.x + 2.0f, Max.y - 3.0f), EditorColors::U32(EditorColors::Accent()));
        }

        // Modules under a simulation stage hang off a guide line from its header.
        const float Inset = bInStage ? 16.0f : 0.0f;
        if (bInStage)
        {
            DrawList->AddLine(ImVec2(Min.x + 11.0f, Min.y - 2.0f), ImVec2(Min.x + 11.0f, Max.y), EditorColors::U32(EditorColors::Border()), 1.0f);
        }

        // A thin bar in the module's category color, which is all the color a row needs to be told apart.
        const ImVec4 Category = ImGui::ColorConvertU32ToFloat4(Module->GetAccentColor());
        DrawList->AddRectFilled(ImVec2(Min.x + Inset + 6.0f, Min.y + 6.0f), ImVec2(Min.x + Inset + 9.0f, Max.y - 6.0f),
            EditorColors::U32(EditorColors::WithAlpha(Category, Module->bEnabled ? 0.9f : 0.3f)), 1.5f);

        const float CenterY = Min.y + (Height - ImGui::GetFrameHeight()) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(Min.x + Inset + 12.0f, CenterY));
        bool bEnabled = Module->bEnabled;
        if (EnableDot("##enabled", bEnabled, "Turn this module off without removing it."))
        {
            Module->bEnabled = bEnabled;
            Action = EModuleRowAction::Toggle;
        }

        ImGui::SameLine(0.0f, 2.0f);
        ImGui::AlignTextToFramePadding();
        const ImVec4 TextColor = bEnabled ? EditorColors::TextPrimary() : EditorColors::TextMuted();
        if (StageHeader != nullptr)
        {
            ImGui::TextColored(bEnabled ? EditorColors::AccentAlt() : EditorColors::TextMuted(), LE_ICON_LAYERS_OUTLINE);
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextColored(TextColor, "%s", StageHeader->StageName.c_str());
            ImGui::SameLine();

            const FString Pill = Format("x{}", StageHeader->Iterations).c_str();
            const ImVec2 PillPos = ImGui::GetCursorScreenPos();
            const ImVec2 PillSize = ImGui::CalcTextSize(Pill.c_str());
            const float  PillY = PillPos.y + (ImGui::GetFrameHeight() - PillSize.y) * 0.5f;
            DrawList->AddRectFilled(ImVec2(PillPos.x - 1.0f, PillY - 1.0f), ImVec2(PillPos.x + PillSize.x + 9.0f, PillY + PillSize.y + 1.0f),
                EditorColors::U32(EditorColors::FrameBg()), 8.0f);
            DrawList->AddText(ImVec2(PillPos.x + 4.0f, PillY), EditorColors::U32(EditorColors::TextDim()), Pill.c_str());
        }
        else
        {
            ImGui::TextColored(TextColor, "%s", Module->GetDisplayName().c_str());
        }

        // Actions stay out of sight until the row is pointed at, so a long stack reads as a list rather than a wall of buttons.
        if (bHovered || bSelected)
        {
            const float Icon = ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos(ImVec2(Max.x - Icon * 3.0f - 2.0f, CenterY));
            if (StackIcon(LE_ICON_ARROW_UP, "##up", EditorColors::TextDim(), EditorColors::TextPrimary(), "Move up.", bCanMoveUp))
            {
                Action = EModuleRowAction::MoveUp;
            }
            ImGui::SameLine(0.0f, 0.0f);
            if (StackIcon(LE_ICON_ARROW_DOWN, "##down", EditorColors::TextDim(), EditorColors::TextPrimary(), "Move down.", bCanMoveDown))
            {
                Action = EModuleRowAction::MoveDown;
            }
            ImGui::SameLine(0.0f, 0.0f);
            if (StackIcon(LE_ICON_DELETE, "##remove", EditorColors::TextDim(), EditorColors::Danger(), "Remove this module."))
            {
                Action = EModuleRowAction::Remove;
            }
        }

        ImGui::SetCursorScreenPos(AfterRow);
        return Action;
    }

    void FParticleSystemEditorTool::DrawModuleGraph()
    {
        // Undo or a delete can take the module away, so it has to still be in its emitter's stack.
        bool bStillThere = false;
        if (CParticleEmitterStack* Stack = GraphModule != nullptr ? GetStackFor(GraphModuleEmitter) : nullptr)
        {
            for (EParticleModuleStage Stage : ParticleOps::GetStages())
            {
                for (const TStrongObjectPtr<CParticleModule>& Module : Stack->GetStack(Stage))
                {
                    bStillThere |= Module.Get() == GraphModule;
                }
            }
        }

        CParticleModule_Graph* Module = bStillThere ? Cast<CParticleModule_Graph>(GraphModule) : nullptr;
        if (Module == nullptr)
        {
            GraphModule = nullptr;
            WiredGraph  = nullptr;
            ImGui::TextDisabled("Select a Graph Module in the stack to edit its nodes here.");
            return;
        }

        CParticleNodeGraph* Graph = Module->GetGraph();
        if (Graph != WiredGraph)
        {
            WiredGraph = Graph;
            Graph->SetTransactionManager(&GetTransactionManager());
            Graph->SetNodeSelectedCallback([this](CEdGraphNode* Node)
            {
                SelectedEmitter = GraphModuleEmitter;
                SelectObject(Node);
            });
            GraphContentVersion = Graph->GetContentVersion();
        }

        Graph->DrawGraph();

        // Any edit to the nodes changes the generated code, so the emitter recompiles once the stack has drawn.
        if (Graph->GetContentVersion() != GraphContentVersion)
        {
            GraphContentVersion = Graph->GetContentVersion();
            MarkEdited(Module);
            DirtyEmitter = GraphModuleEmitter;
        }
    }

    void FParticleSystemEditorTool::DrawGridTargetPreview(int32 EmitterIndex)
    {
        CParticleSystem* PS = GetSystem();
        const CParticleEmitter* Emitter = (PS != nullptr && EmitterIndex < (int32)PS->Emitters.size()) ? PS->Emitters[EmitterIndex].Get() : nullptr;
        if (Emitter == nullptr || Emitter->GridChannelCount == 0)
        {
            return;
        }

        const CTextureRenderTarget* Target = Emitter->GridRenderTarget.Get();
        if (Target == nullptr || !Target->GetTextureResource().NewTexture.IsValid())
        {
            ImGui::TextDisabled("Set the emitter's Grid Render Target to see the output here.");
            return;
        }

        const FUIntVector3 Extent = RHI::GetTextureDesc(Target->GetTextureResource().NewTexture.Texture).Dimension;
        const float Width  = Math::Min(ImGui::GetContentRegionAvail().x, 256.0f);
        const float Height = Width * (float)Extent.y / (float)Math::Max(Extent.x, 1u);
        ImGui::Image(ImGuiX::ToImTextureRef(Target->GetTextureResource().NewTexture), ImVec2(Width, Height));
        ImGui::TextDisabled("%u x %u target, %d x %d grid, %u channels", Extent.x, Extent.y, Emitter->GridWidth, Emitter->GridHeight, Emitter->GridChannelCount);
    }

    void FParticleSystemEditorTool::DrawAddModulePopup(int32 EmitterIndex, EParticleModuleStage Stage)
    {
        // Popups are keyed within the current ID scope, so this must also match the emitter that opened it.
        if (PendingAddStage != Stage || PendingAddEmitter != EmitterIndex)
        {
            return;
        }

        CParticleEmitterStack* Emitter = GetStackFor(EmitterIndex);
        if (Emitter == nullptr)
        {
            return;
        }

        if (ImGui::BeginPopup("##AddModule"))
        {
            ImGui::TextDisabled("Add Module");
            ImGui::Separator();

            // Reflection returns registration order, and the header is emitted on category change.
            struct FPaletteEntry
            {
                FString          Category;
                FString          Name;
                CClass*          Class = nullptr;
                CParticleModule* CDO   = nullptr;
            };

            TVector<FPaletteEntry> Entries;
            Entries.reserve(ModulePalette.size());
            for (CClass* ModuleClass : ModulePalette)
            {
                CParticleModule* CDO = ModuleClass->GetDefaultObject<CParticleModule>();
                if (CDO == nullptr || CDO->GetStage() != Stage)
                {
                    continue;
                }

                FPaletteEntry Entry;
                Entry.Category = CDO->GetCategory();
                Entry.Name     = CDO->GetDisplayName();
                Entry.Class    = ModuleClass;
                Entry.CDO      = CDO;
                Entries.push_back(Entry);
            }

            Algo::Sort(Entries, [](const FPaletteEntry& A, const FPaletteEntry& B)
            {
                const int32 Cat = strcmp(A.Category.c_str(), B.Category.c_str());
                return (Cat != 0) ? (Cat < 0) : (strcmp(A.Name.c_str(), B.Name.c_str()) < 0);
            });

            if (Entries.empty())
            {
                ImGui::TextDisabled("No modules for this stage.");
            }

            FString CurrentCategory;
            for (const FPaletteEntry& Entry : Entries)
            {
                if (Entry.Category != CurrentCategory)
                {
                    CurrentCategory = Entry.Category;
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 150, 155, 255));
                    ImGui::TextUnformatted(CurrentCategory.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::Indent(10.0f);
                if (ImGui::Selectable(Entry.Name.c_str()))
                {
                    CParticleModule* Added = Emitter->AddModule(Entry.Class);
                    if (Added != nullptr)
                    {
                        SelectedEmitter = EmitterIndex;
                        SelectModule(Added);
                        MarkEdited(Added);
                        DirtyEmitter = EmitterIndex;
                    }
                }
                if (ImGui::IsItemHovered() && !Entry.CDO->GetTooltip().empty())
                {
                    ImGui::SetTooltip("%s", Entry.CDO->GetTooltip().c_str());
                }
                ImGui::Unindent(10.0f);
            }

            ImGui::EndPopup();
        }
    }

    void FParticleSystemEditorTool::Compile()
    {
        CompilationResult = FCompilationResultInfo();

        CParticleSystem* PS = GetSystem();
        if (PS == nullptr)
        {
            return;
        }

        SyncEmitterStacks();

        // Stopping at the first error would blank the other emitters' shaders and darken the preview.
        for (int32 i = 0; i < (int32)PS->Emitters.size(); ++i)
        {
            CompileEmitter(i);
            MarkEdited(PS->Emitters[i].Get());
        }
    }

    bool FParticleSystemEditorTool::CompileEmitter(int32 EmitterIndex)
    {
        CParticleSystem* PS = GetSystem();
        if (PS == nullptr || EmitterIndex < 0 || EmitterIndex >= (int32)PS->Emitters.size() || PS->Emitters[EmitterIndex] == nullptr)
        {
            return false;
        }

        const FString Prefix = PS->Emitters[EmitterIndex]->EmitterName + ": ";

        FParticleEmitterCompileOutput Output;
        ParticleOps::CompileEmitter(PS, EmitterIndex, Output);

        for (const FString& Error : Output.Errors)
        {
            CompilationResult.CompilationLog += Prefix + "ERROR - " + Error + "\n";
        }

        for (const FString& Note : Output.Notes)
        {
            CompilationResult.CompilationLog += Prefix + Note + "\n";
        }

        CompilationResult.bIsError |= !Output.bSucceeded;
        return Output.bSucceeded;
    }

    bool FParticleSystemEditorTool::RefreshModuleParams()
    {
        CParticleSystem* PS = GetSystem();
        if (!ParticleOps::RefreshModuleParams(PS, SelectedEmitter))
        {
            return false;
        }

        MarkEdited(PS->Emitters[SelectedEmitter].Get());
        return true;
    }

    void FParticleSystemEditorTool::OnSave()
    {
        Compile();
        FAssetEditorTool::OnSave();

        // A shared emitter edited from this system is saved with it, since it has no tab of its own here.
        for (const TStrongObjectPtr<CParticleEmitter>& Emitter : GetSystem()->Emitters)
        {
            CPackage* Package = Emitter->GetPackage();
            if (Package != Asset->GetPackage() && Package->IsDirty() && !CPackage::SavePackage(Package, Package->GetPackagePath()))
            {
                ImGuiX::Notifications::NotifyError("Failed to save emitter asset '{0}'.", Emitter->GetName().c_str());
            }
        }
    }

    bool FParticleSystemEditorTool::IsUnsavedDocument()
    {
        if (FAssetEditorTool::IsUnsavedDocument())
        {
            return true;
        }
        if (GetSystem() == nullptr)
        {
            return false;
        }
        for (const TStrongObjectPtr<CParticleEmitter>& Emitter : GetSystem()->Emitters)
        {
            const CPackage* Package = Emitter.IsValid() ? Emitter->GetPackage() : nullptr;
            if (Package != nullptr && Package->IsDirty())
            {
                return true;
            }
        }
        return false;
    }

    void FParticleSystemEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID LeftDockID = 0, RightDockID = 0, RightBottomDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Left, 0.28f, &LeftDockID, &RightDockID);
        ImGui::DockBuilderSplitNode(RightDockID, ImGuiDir_Down, 0.32f, &RightBottomDockID, &RightDockID);

        ImGuiID GraphDockID = 0;
        ImGui::DockBuilderSplitNode(RightDockID, ImGuiDir_Down, 0.5f, &GraphDockID, &RightDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(EmitterWindowName).c_str(),    LeftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(),   RightDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(ModuleGraphWindowName).c_str(), GraphDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(SelectionWindowName).c_str(),  RightBottomDockID);
    }

    void FParticleSystemEditorTool::OnPostUndoRedo()
    {
        FAssetEditorTool::OnPostUndoRedo();

        // The authoring stacks mirror the asset's emitters, so they are rebuilt from what the restore left.
        SyncEmitterStacks();
        Compile();
    }
}
