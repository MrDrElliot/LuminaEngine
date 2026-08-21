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

            // Persisted on the emitter, so reordering or renaming never re-points a stack.
            if (Emitter->AuthoringStackName.empty())
            {
                Emitter->AuthoringStackName = FString("ParticleStack_") + Format("{}", i).c_str();
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

        // Reflection is the registry, so a plugin or game module lands in the palette once it loads.
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

            // The CDO carries every label and is the only way to ask whether the class wants listing.
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

        TVector<TObjectPtr<CParticleModule>>& Stack = EmitterStack->GetStack(Stage);

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

            Algo::Sort(Entries.begin(), Entries.end(), [](const FPaletteEntry& A, const FPaletteEntry& B)
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

        // RefreshModuleParams checks against this before doing a value-only update.
        Emitter->ModuleParamValues   = Compiler.GetParamValues();
        Emitter->ParamBindings       = Compiler.GetParamBindings();
        Emitter->CompiledCodeHash    = Compiler.GetGeneratedCodeHash();
        // Structural, so it only moves with a rebuild, which is when the renderer resizes its buffer.
        Emitter->AttributeFloatCount = Compiler.GetAttributeFloatCount();

        // The shared vertex shader cannot know the generated layout, so it receives these as uniforms.
        Emitter->RenderAttributeSlots.resize((size_t)ParticleRenderAttribute::Count);
        for (int32 A = 0; A < (int32)ParticleRenderAttribute::Count; ++A)
        {
            Emitter->RenderAttributeSlots[A] = Compiler.FindAttributeSlot(ParticleRenderAttribute::Names[A]);
        }

        // Nothing on screen says whether the trail wiring resolved, so the log has to say it.
        if (Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosX] >= 0)
        {
            CompilationResult.CompilationLog += Prefix + "Trail active - previous position in attribute slots "
                + Format("{}", Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosX]).c_str()
                + "/" + Format("{}", Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosY]).c_str()
                + "/" + Format("{}", Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosZ]).c_str()
                + " of " + Format("{}", Emitter->AttributeFloatCount).c_str() + " floats per particle.\n";
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

        // The other emitters' slot values did not change, so re-running their stacks is pure work.
        CParticleEmitter* Emitter = PS->Emitters[SelectedEmitter].Get();
        if (Emitter == nullptr || Emitter->ComputeShaderBinaries.empty())
        {
            return false;
        }

        // The emitted HLSL is discarded, so there is no pipeline swap and no preview restart.
        FParticleCompiler Compiler;
        Stack->CompileStacks(Compiler);

        if (Compiler.HasErrors())
        {
            return false;
        }

        // Layout drift means the running shader indexes slots this value set no longer describes.
        if (Compiler.GetGeneratedCodeHash() != Emitter->CompiledCodeHash)
        {
            return false;
        }

        // Binding an input leaves the code identical, so this is the ONLY path that ever writes them.
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
