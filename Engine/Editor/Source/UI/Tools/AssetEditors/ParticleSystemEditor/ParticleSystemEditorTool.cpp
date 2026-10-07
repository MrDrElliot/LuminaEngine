#include "ParticleSystemEditorTool.h"
#include "ParticleParamCustomization.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/PropertyChangedEvent.h"
#include "Particles/ParticleEmitterStack.h"
#include "Particles/ParticleOps.h"
#include "Particles/ParticleStockModules.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"
#include "UI/Tools/Transactions/ObjectSnapshotCommand.h"
#include "Paths/Paths.h"
#include "Renderer/RenderResource.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    static const char* EmitterWindowName    = "Emitter";
    static const char* SelectionWindowName  = "Details";

    // Built lazily so reflected classes are registered by the time it runs. Add new stock modules here.

    FParticleSystemEditorTool::FParticleSystemEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
        , ParticleEntity()
        , DirectionalLightEntity()
    {
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
        ParticleComponent.ParticleSystem = Cast<CParticleSystem>(Asset.Get());

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

        CreateToolWindow(SelectionWindowName, [&](bool bFocused)
        {
            // A property handle carries no owner, so the system is announced around the draw instead.
            ParticleParamContext::FScope ParamScope(Cast<CParticleSystem>(Asset.Get()));
            GetPropertyTable()->DrawTree();
        });

        SyncEmitterStacks();

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

            if (SelectedModule == nullptr)
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

        PreviewAge += (float)UpdateContext.GetDeltaTime();
        if (bShowGizmos)
        {
            DrawEmitterGizmos();
        }
        if (bLoopPreview)
        {
            const CParticleSystem* PS = Cast<CParticleSystem>(Asset.Get());
            const float OneShotLength = PS != nullptr ? PS->GetOneShotLength() : 0.0f;
            if (OneShotLength > 0.0f && PreviewAge >= OneShotLength + PreviewReplayPause)
            {
                ReplayPreview();
            }
        }

        // Re-applied every frame, since idle reclaim can rebuild the render scene and its settings.
        if (World.IsValid() && World->GetRenderer() != nullptr)
        {
            World->GetRenderer()->GetSceneRenderSettings().bDrawBillboards = false;
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
        CParticleSystem* PS = Cast<CParticleSystem>(Asset.Get());
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

        ImGui::MenuItem(LE_ICON_REPEAT" Loop Preview", nullptr, &bLoopPreview);
        ImGui::MenuItem(LE_ICON_SHAPE_OUTLINE" Gizmos", nullptr, &bShowGizmos);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Re-fire a one-shot system once its particles have expired. Streaming emitters are left alone.");
        }
    }

    void FParticleSystemEditorTool::DrawEmitterGizmos()
    {
        const CParticleSystem* PS = Cast<CParticleSystem>(Asset.Get());
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
            World->DrawSphere(Origin, Emitter->VisibilityRadius, BoundsColor, {}, {}, {});
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
                World->DrawSphere(Origin, Size.x, ShapeColor, {}, {}, {});
                break;
            case EParticleEmitterShape::Box:
                World->DrawBox(Origin, Size, FQuat::Identity(), ShapeColor, {}, {}, {});
                break;
            case EParticleEmitterShape::Cone:
                World->DrawCone(Origin, FVector3(0.0f, 1.0f, 0.0f), Math::Atan2(Size.x, Math::Max(Size.y, 1e-4f)), Size.y, ShapeColor, {}, {}, {});
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
                        World->DrawLine(Origin + FVector3(Math::Cos(A), 0.0f, Math::Sin(A)) * Radius,
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

        CObject* Target = (Object != nullptr) ? Object : Asset.Get();
        GetPropertyTable()->SetObject(Target, Target->GetClass());
    }

    void FParticleSystemEditorTool::DrawStack()
    {
        CParticleSystem* PS = Cast<CParticleSystem>(Asset.Get());
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
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 90, 90, 255));
            ImGui::TextWrapped("%s", CompilationResult.CompilationLog.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        }

        // System header selects the asset so its user parameters show in Details.
        const bool bSystemSelected = (SelectedObject == nullptr);
        if (ImGui::Selectable(LE_ICON_COG" System Settings", bSystemSelected))
        {
            SelectObject(nullptr);
        }
        ImGui::Spacing();

        // This panel docks tall and narrow, so fixed-width columns truncated every module name.
        ImGui::BeginChild("##emitters", ImVec2(0.0f, 0.0f));

        for (int32 i = 0; i < (int32)PS->Emitters.size(); ++i)
        {
            ImGui::PushID(i);
            DrawEmitterSection(i);
            ImGui::PopID();
            ImGui::Spacing();
        }

        if (ImGui::Button(LE_ICON_PLUS" Add Emitter", ImVec2(-FLT_MIN, 0.0f)))
        {
            if (CParticleEmitter* Added = PS->AddEmitter())
            {
                SyncEmitterStacks();
                SelectedEmitter = (int32)PS->Emitters.size() - 1;
                SelectObject(Added);
                Asset->GetPackage()->MarkDirty();
                DirtyEmitter = SelectedEmitter;
            }
        }

        ImGui::EndChild();

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
            Asset->GetPackage()->MarkDirty();
        }

        ImGui::PopStyleVar(2);

        // Recompile after the UI loop, and only the emitter that actually changed.
        if (DirtyEmitter != Constants::kIndexNone)
        {
            const int32 Target = DirtyEmitter;
            DirtyEmitter = Constants::kIndexNone;
            CompilationResult = FCompilationResultInfo();
            CompileEmitter(Target);
            Asset->GetPackage()->MarkDirty();
        }
    }

    void FParticleSystemEditorTool::DrawEmitterSection(int32 EmitterIndex)
    {
        CParticleSystem* PS = Cast<CParticleSystem>(Asset.Get());
        CParticleEmitter* Emitter = (PS != nullptr && EmitterIndex < (int32)PS->Emitters.size())
                                  ? PS->Emitters[EmitterIndex].Get() : nullptr;
        if (Emitter == nullptr)
        {
            return;
        }

        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        const float Pad     = ImGui::GetStyle().FramePadding.x;
        const float BtnW    = ImGui::CalcTextSize(LE_ICON_DELETE).x + Pad * 2.0f;
        const float CheckW  = ImGui::GetFrameHeight();
        const float Cluster = CheckW + BtnW * 3.0f + Spacing * 4.0f;

        const bool bSelected = (SelectedObject == (CObject*)Emitter);

        // Captured before the header consumes the line, since SameLine's offset is window-local.
        const float HeaderRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

        // OpenOnArrow splits the jobs, or selecting an emitter also collapses the stack being read.
        ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_CollapsingHeader
                                 | ImGuiTreeNodeFlags_DefaultOpen
                                 | ImGuiTreeNodeFlags_AllowOverlap
                                 | ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (bSelected)
        {
            Flags |= ImGuiTreeNodeFlags_Selected;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 6));
        ImGui::PushStyleColor(ImGuiCol_Text, Emitter->bEnabled ? IM_COL32(235, 220, 160, 255)
                                                               : IM_COL32(125, 125, 130, 255));
        const bool bOpen = ImGui::TreeNodeEx("##emhdr", Flags, "%s", Emitter->EmitterName.c_str());
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            SelectedEmitter = EmitterIndex;
            SelectObject(Emitter);
        }

        // Controls overlaid on the header bar, right-aligned, so they stay reachable when collapsed.
        ImGui::SameLine(HeaderRight - Cluster);

        bool bEnabled = Emitter->bEnabled;
        if (ImGui::Checkbox("##emen", &bEnabled))
        {
            Emitter->bEnabled = bEnabled;
            Asset->GetPackage()->MarkDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Mute this emitter. Disabled emitters do not simulate or draw.");
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(EmitterIndex == 0);
        if (ImGui::Button(LE_ICON_ARROW_UP"##emup", ImVec2(BtnW, 0)))
        {
            PS->MoveEmitter(Emitter, -1);
            SyncEmitterStacks();
            Asset->GetPackage()->MarkDirty();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(EmitterIndex == (int32)PS->Emitters.size() - 1);
        if (ImGui::Button(LE_ICON_ARROW_DOWN"##emdn", ImVec2(BtnW, 0)))
        {
            PS->MoveEmitter(Emitter, 1);
            SyncEmitterStacks();
            Asset->GetPackage()->MarkDirty();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        // A system with no emitters renders nothing and every consumer would need a special case.
        ImGui::BeginDisabled(PS->Emitters.size() <= 1);
        if (ImGui::Button(LE_ICON_DELETE"##emdel", ImVec2(BtnW, 0)))
        {
            PendingRemoveEmitter = Emitter;
        }
        ImGui::EndDisabled();

        if (bOpen)
        {
            // Indented under the header, with air above and below so adjacent emitters do not run together.
            ImGui::Spacing();
            ImGui::Indent(12.0f);
            DrawStackSection(EmitterIndex, EParticleModuleStage::Spawn,  "Particle Spawn");
            ImGui::Spacing();
            DrawStackSection(EmitterIndex, EParticleModuleStage::Update, "Particle Update");
            ImGui::Unindent(12.0f);
            ImGui::Spacing();
        }
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

        // A frame-height square clips wider glyphs such as the trash icon, so size for icon plus padding.
        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        const float Pad     = ImGui::GetStyle().FramePadding.x;
        const float BtnW    = ImGui::CalcTextSize(LE_ICON_DELETE).x + Pad * 2.0f;
        const float Cluster = BtnW * 3.0f + Spacing * 3.0f; // up + down + delete, plus the gaps

        // Section header with a right-aligned + add button.
        const float HeaderWidth = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 200, 130, 255));
        ImGui::TextUnformatted(Label);
        ImGui::PopStyleColor();
        ImGui::SameLine(HeaderWidth - BtnW);
        if (ImGui::Button(LE_ICON_PLUS"##add", ImVec2(BtnW, 0)))
        {
            PendingAddStage   = Stage;
            PendingAddEmitter = EmitterIndex;
            RefreshModulePalette();
            ImGui::OpenPopup("##AddModule");
        }
        ImGui::Separator();

        CParticleModule* PendingRemove = nullptr;
        for (int32 i = 0; i < (int32)Stack.size(); ++i)
        {
            CParticleModule* Module = Stack[i].Get();
            if (Module == nullptr)
            {
                continue;
            }

            ImGui::PushID(i);

            bool bEnabled = Module->bEnabled;
            if (ImGui::Checkbox("##en", &bEnabled))
            {
                Module->bEnabled = bEnabled;
                Asset->GetPackage()->MarkDirty();
                DirtyEmitter = EmitterIndex;
            }
            ImGui::SameLine();

            const uint32 Accent = Module->GetAccentColor();
            ImGui::PushStyleColor(ImGuiCol_Text, bEnabled ? Accent : IM_COL32(120, 120, 125, 255));
            const bool bSelected = (SelectedModule == Module);
            const float Remaining = ImGui::GetContentRegionAvail().x - Cluster;
            const float RowWidth = Remaining > 40.0f ? Remaining : 40.0f;
            if (ImGui::Selectable(Module->GetDisplayName().c_str(), bSelected, 0, ImVec2(RowWidth, 0)))
            {
                SelectedEmitter = EmitterIndex;
                SelectModule(Module);
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && !Module->GetTooltip().empty())
            {
                ImGui::SetTooltip("%s", Module->GetTooltip().c_str());
            }

            ImGui::SameLine();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::Button(LE_ICON_ARROW_UP"##up", ImVec2(BtnW, 0))) { EmitterStack->MoveModule(Module, -1); Asset->GetPackage()->MarkDirty(); DirtyEmitter = EmitterIndex; }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(i == (int32)Stack.size() - 1);
            if (ImGui::Button(LE_ICON_ARROW_DOWN"##down", ImVec2(BtnW, 0))) { EmitterStack->MoveModule(Module, 1); Asset->GetPackage()->MarkDirty(); DirtyEmitter = EmitterIndex; }
            ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button(LE_ICON_DELETE"##del", ImVec2(BtnW, 0)))
            {
                PendingRemove = Module;
            }

            ImGui::PopID();
        }

        if (PendingRemove != nullptr)
        {
            if (SelectedModule == PendingRemove)
            {
                SelectObject(nullptr);
            }
            EmitterStack->RemoveModule(PendingRemove);
            Asset->GetPackage()->MarkDirty();
            DirtyEmitter = EmitterIndex;
        }

        DrawAddModulePopup(EmitterIndex, Stage);

        ImGui::PopID();
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
                        Asset->GetPackage()->MarkDirty();
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

        CParticleSystem* PS = Cast<CParticleSystem>(Asset.Get());
        if (PS == nullptr)
        {
            return;
        }

        SyncEmitterStacks();

        // Stopping at the first error would blank the other emitters' shaders and darken the preview.
        for (int32 i = 0; i < (int32)PS->Emitters.size(); ++i)
        {
            CompileEmitter(i);
        }

        PS->GetPackage()->MarkDirty();
    }

    bool FParticleSystemEditorTool::CompileEmitter(int32 EmitterIndex)
    {
        CParticleSystem* PS = Cast<CParticleSystem>(Asset.Get());
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
        CParticleSystem* PS = Cast<CParticleSystem>(Asset.Get());
        if (!ParticleOps::RefreshModuleParams(PS, SelectedEmitter))
        {
            return false;
        }

        PS->GetPackage()->MarkDirty();
        return true;
    }

    void FParticleSystemEditorTool::OnSave()
    {
        Compile();
        FAssetEditorTool::OnSave();
    }

    void FParticleSystemEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID LeftDockID = 0, RightDockID = 0, RightBottomDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Left, 0.28f, &LeftDockID, &RightDockID);
        ImGui::DockBuilderSplitNode(RightDockID, ImGuiDir_Down, 0.32f, &RightBottomDockID, &RightDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(EmitterWindowName).c_str(),    LeftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(),   RightDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(SelectionWindowName).c_str(),  RightBottomDockID);
    }
}
