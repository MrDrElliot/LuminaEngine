#include "ParticleSystemEditorTool.h"
#include "ParticleParamCustomization.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/PropertyChangedEvent.h"
#include "Particles/ParticleEmitterStack.h"
#include "Particles/ParticleStockModules.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"
#include "UI/Tools/Transactions/ObjectSnapshotCommand.h"
#include "Paths/Paths.h"
#include "Renderer/RenderResource.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "World/entity/components/environmentcomponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/entity/components/lightcomponent.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    static const char* EmitterWindowName    = "Emitter";
    static const char* SelectionWindowName  = "Details";

    // All module classes the add-module palette offers. Built lazily so reflected classes are
    // registered by the time it runs. Add new stock modules here.

    FParticleSystemEditorTool::FParticleSystemEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
        , ParticleEntity()
        , DirectionalLightEntity()
    {
    }

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
            // Module inputs are SParticleParam, whose customization offers a menu of the system's user
            // parameters -- and a property handle carries no owner, so the system has to be announced
            // around the draw rather than found from the property.
            ParticleParamContext::FScope ParamScope(Cast<CParticleSystem>(Asset.Get()));
            GetPropertyTable()->DrawTree();
        });

        SyncEmitterStacks();

        // Undo for the details panel. FAssetEditorTool::SetupPropertyUndo already installed a pair, but it
        // is wrong for this tool twice over: it snapshots Asset, while this panel is re-pointed at the
        // SELECTED MODULE, so a module edit was recorded against the particle system instead; and the
        // finish callback below replaced its committing half, leaving a transaction open on every edit.
        // Both halves are reinstalled here against the object actually being edited.
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

        // Live preview: recompile on module-input edit finish (mouse-up). System-setting edits
        // (no module selected) feed the sim uniforms directly and need no recompile.
        GetPropertyTable()->SetFinishEditCallback([this](const FPropertyChangedEvent&)
        {
            // Commit first: the snapshot's after-image has to be captured before any recompile, and the
            // command drops itself if nothing actually changed.
            GetTransactionManager().CommitTransaction();

            if (SelectedModule == nullptr)
            {
                return;   // system-wide settings feed the sim uniforms directly
            }

            // Value edits go through the parameter block: a buffer rewrite, so the running simulation
            // keeps its particles, age and spawn state. Only fall back to the shader rebuild when the
            // layout actually changed (an input whose value alters generated CODE rather than a slot,
            // e.g. an enum that selects a different emitter shape branch).
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

        // The preview world holds a light entity purely to shade the particles; its billboard icon is
        // noise on top of the effect being authored. Re-applied every frame rather than once at setup
        // because the render scene (and its settings) can be rebuilt by idle reclaim. Same as the mesh,
        // skeletal, material and prefab preview tools.
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

        EmitterStacks.clear();
        EmitterStacks.reserve(PS->Emitters.size());

        for (int32 i = 0; i < (int32)PS->Emitters.size(); ++i)
        {
            CParticleEmitter* Emitter = PS->Emitters[i].Get();
            if (Emitter == nullptr)
            {
                EmitterStacks.push_back(nullptr);
                continue;
            }

            // Named once and then persisted on the emitter, so reordering or renaming emitters never
            // re-points a stack. Derived from the index only on first use.
            if (Emitter->AuthoringStackName.empty())
            {
                Emitter->AuthoringStackName = FString("ParticleStack_") + eastl::to_string(i).c_str();
            }

            CParticleEmitterStack* Stack = Cast<CParticleEmitterStack>(
                Asset->GetPackage()->LoadObjectByName(Emitter->AuthoringStackName));
            if (Stack == nullptr)
            {
                Stack = NewObject<CParticleEmitterStack>(Asset->GetPackage(), Emitter->AuthoringStackName);
                Stack->EnsureDefaultStack();
            }
            EmitterStacks.push_back(Stack);
        }

        if (SelectedEmitter >= (int32)EmitterStacks.size())
        {
            SelectedEmitter = 0;
        }
    }

    void FParticleSystemEditorTool::RefreshModulePalette()
    {
        ModulePalette.clear();

        CClass* BaseClass = CParticleModule::StaticClass();

        // Reflection is the registry: any CParticleModule subclass, wherever it is declared, is in the
        // palette the moment its module is loaded. That is what makes plugin- and game-defined modules
        // work -- the previous hardcoded list could only ever name engine classes, and forgetting to add
        // one there made it silently invisible with no error anywhere.
        GObjectArray.ForEachObject([&](CObjectBase* Object, int32)
        {
            if (Object == nullptr || !Object->IsA<CClass>())
            {
                return;
            }

            CClass* Class = static_cast<CClass*>(Object);
            if (Class == BaseClass || !Class->IsChildOf(BaseClass))
            {
                return;
            }

            // The CDO is what the palette reads every label off, and it is also the only way to ask a
            // class whether it wants to be listed at all.
            CParticleModule* CDO = Class->GetDefaultObject<CParticleModule>();
            if (CDO == nullptr || !CDO->IsPaletteVisible())
            {
                return;
            }

            ModulePalette.push_back(Class);
        });
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

        // Emitters can be added or removed by undo as well as by the buttons below, so the stacks are
        // reconciled every frame rather than only at the mutation sites.
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

        // Emitters stacked vertically, each spanning the panel's full width, with the list scrolling.
        // Side-by-side columns were tried first and were the wrong shape for this panel: it docks tall and
        // narrow, so fixed-width columns truncated every module name -- the one thing the stack is read for.
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
            // The stack object is left in the package rather than destroyed: it holds the authoring data,
            // and dropping it here would lose the modules outright. Orphaned, and re-adopted by name if
            // the emitter comes back.
            PS->RemoveEmitter(PendingRemoveEmitter);
            PendingRemoveEmitter = nullptr;
            SyncEmitterStacks();
            Asset->GetPackage()->MarkDirty();
        }

        ImGui::PopStyleVar(2);

        // Recompile after the UI loop, and only the emitter that actually changed.
        if (DirtyEmitter != INDEX_NONE)
        {
            const int32 Target = DirtyEmitter;
            DirtyEmitter = INDEX_NONE;
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

        // Captured before the header consumes the line; SameLine's offset is window-local, so this is the
        // right edge the control cluster gets pinned to.
        const float HeaderRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

        // OpenOnArrow splits the two jobs the header does: the arrow folds, the label selects. Without it
        // a framed header toggles on any click, so you could not put an emitter in the Details panel
        // without also collapsing the stack you were about to read.
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
        // The last emitter cannot be removed -- a system with none has nothing to render and every
        // consumer would need a special case for it.
        ImGui::BeginDisabled(PS->Emitters.size() <= 1);
        if (ImGui::Button(LE_ICON_DELETE"##emdel", ImVec2(BtnW, 0)))
        {
            PendingRemoveEmitter = Emitter;
        }
        ImGui::EndDisabled();

        if (bOpen)
        {
            // Indented under the header so the two stacks read as belonging to it, with a little air above
            // and below to keep adjacent emitters from running together.
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

        TVector<TObjectPtr<CParticleModule>>& Stack = EmitterStack->GetStack(Stage);

        // One uniform button width that actually fits an icon glyph + frame padding, used for every
        // control on a row. (A frame-height square clips wider glyphs like the trash icon.)
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
        // ImGui popups are keyed by name within the current ID scope, and every column pushes its own, so
        // this also has to match the emitter that opened it -- otherwise each column would try to draw the
        // popup for every stage.
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

            // Reflection hands these back in registration order, which is arbitrary and varies with module
            // load order. The header below is emitted on category CHANGE, so an unsorted list would split
            // one category into several headers scattered through the menu. Sort by (category, name) first.
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

            eastl::sort(Entries.begin(), Entries.end(), [](const FPaletteEntry& A, const FPaletteEntry& B)
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

        // Every emitter is attempted even after one fails. A shared effect is normally mid-edit on a single
        // emitter, and stopping at the first error would blank the others' shaders too -- so the preview
        // would go dark for a mistake in one column.
        for (int32 i = 0; i < (int32)PS->Emitters.size(); ++i)
        {
            CompileEmitter(i);
        }

        PS->GetPackage()->MarkDirty();
    }

    bool FParticleSystemEditorTool::CompileEmitter(int32 EmitterIndex)
    {
        CParticleSystem* PS = Cast<CParticleSystem>(Asset.Get());
        CParticleEmitterStack* Stack = GetStackFor(EmitterIndex);
        if (PS == nullptr || Stack == nullptr || EmitterIndex >= (int32)PS->Emitters.size())
        {
            return false;
        }

        CParticleEmitter* Emitter = PS->Emitters[EmitterIndex].Get();
        if (Emitter == nullptr)
        {
            return false;
        }

        const FString Prefix = Emitter->EmitterName + ": ";

        FParticleCompiler Compiler;
        Stack->CompileStacks(Compiler);

        if (Compiler.HasErrors())
        {
            for (const EdNodeGraph::FError& Error : Compiler.GetErrors())
            {
                CompilationResult.CompilationLog += Prefix + "ERROR - [" + Error.Name + "]: " + Error.Description + "\n";
            }
            CompilationResult.bIsError = true;
            return false;
        }

        const FString Source = Compiler.BuildShader();
        if (Source.empty())
        {
            CompilationResult.CompilationLog += Prefix + "Failed to build shader source.\n";
            CompilationResult.bIsError = true;
            return false;
        }

        IShaderCompiler* ShaderCompiler = GShaderCompiler;
        ShaderCompiler->CompilerShaderRaw(Source, {}, [Emitter](const FShaderHeader& Header) mutable
        {
            Emitter->ComputeShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
        });
        ShaderCompiler->Flush();

        // Slots the shader was built against; RefreshModuleParams checks against this before doing a
        // value-only update.
        Emitter->ModuleParamValues   = Compiler.GetParamValues();
        Emitter->ParamBindings       = Compiler.GetParamBindings();
        Emitter->CompiledCodeHash    = Compiler.GetGeneratedCodeHash();
        // Structural, so it only moves with a rebuild -- which is exactly when the renderer needs to
        // resize the parallel attribute buffer.
        Emitter->AttributeFloatCount = Compiler.GetAttributeFloatCount();

        // Resolved once here rather than looked up per frame: the vertex shader is shared and cannot know
        // the generated layout, so it receives these as uniforms.
        Emitter->RenderAttributeSlots.resize((size_t)ParticleRenderAttribute::Count);
        for (int32 A = 0; A < (int32)ParticleRenderAttribute::Count; ++A)
        {
            Emitter->RenderAttributeSlots[A] = Compiler.FindAttributeSlot(ParticleRenderAttribute::Names[A]);
        }

        // Trail rendering is driven by an attribute slot the vertex shader reads, so nothing on screen says
        // whether the wiring resolved. Report it: a trail that fails to appear is then either "the log never
        // said it was active" (the stack) or "it did" (the shader), rather than a guess between the two.
        if (Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosX] >= 0)
        {
            CompilationResult.CompilationLog += Prefix + "Trail active - previous position in attribute slots "
                + eastl::to_string(Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosX]).c_str()
                + "/" + eastl::to_string(Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosY]).c_str()
                + "/" + eastl::to_string(Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosZ]).c_str()
                + " of " + eastl::to_string(Emitter->AttributeFloatCount).c_str() + " floats per particle.\n";
        }

        // Route the renderer to the generated module-stack shader.
        Emitter->ShaderMode = EParticleShaderMode::Custom;
        Emitter->PostLoad();
        return true;
    }

    bool FParticleSystemEditorTool::RefreshModuleParams()
    {
        CParticleSystem* PS = Cast<CParticleSystem>(Asset.Get());
        CParticleEmitterStack* Stack = GetStackFor(SelectedEmitter);
        if (PS == nullptr || Stack == nullptr || SelectedEmitter >= (int32)PS->Emitters.size())
        {
            return false;
        }

        // Only the emitter owning the edited module is refreshed. The others' slot values did not change,
        // and re-running their stacks would be pure work.
        CParticleEmitter* Emitter = PS->Emitters[SelectedEmitter].Get();
        if (Emitter == nullptr || Emitter->ComputeShaderBinaries.empty())
        {
            return false;
        }

        // Re-runs the module Generate pass purely to collect slot values. Generation is deterministic and
        // the layout depends only on stack structure, so the emitted HLSL is discarded -- no Slang, no new
        // FShaderEntry, and therefore no pipeline swap and no preview restart.
        FParticleCompiler Compiler;
        Stack->CompileStacks(Compiler);

        if (Compiler.HasErrors())
        {
            return false;
        }

        // Layout drift means the running shader indexes slots this value set no longer describes; reading
        // it would silently feed modules the wrong inputs, so hand back to the full compile instead.
        if (Compiler.GetGeneratedCodeHash() != Emitter->CompiledCodeHash)
        {
            return false;
        }

        // Bindings ride along with the values: binding an input leaves the generated code identical, so
        // the hash check above passes and this is the ONLY path that will ever write them. Refreshing the
        // values alone would leave a binding sitting in the asset doing nothing until some unrelated
        // structural edit happened to force a full compile.
        Emitter->ModuleParamValues = Compiler.GetParamValues();
        Emitter->ParamBindings     = Compiler.GetParamBindings();
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
