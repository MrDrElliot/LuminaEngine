#include "ParticleSystemEditorTool.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/PropertyChangedEvent.h"
#include "Particles/ParticleEmitterStack.h"
#include "Particles/ParticleStockModules.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"
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
    static const TVector<CClass*>& GetModuleClasses()
    {
        static TVector<CClass*> Classes;
        if (Classes.empty())
        {
            Classes.push_back(CParticleModule_SpawnLocation::StaticClass());
            Classes.push_back(CParticleModule_InitialVelocity::StaticClass());
            Classes.push_back(CParticleModule_InitialColor::StaticClass());
            Classes.push_back(CParticleModule_InitialSize::StaticClass());
            Classes.push_back(CParticleModule_Lifetime::StaticClass());
            Classes.push_back(CParticleModule_InitialRotation::StaticClass());
            Classes.push_back(CParticleModule_GravityForce::StaticClass());
            Classes.push_back(CParticleModule_Drag::StaticClass());
            Classes.push_back(CParticleModule_CurlNoiseForce::StaticClass());
            Classes.push_back(CParticleModule_ColorOverLife::StaticClass());
            Classes.push_back(CParticleModule_SizeOverLife::StaticClass());
            Classes.push_back(CParticleModule_Integrate::StaticClass());
        }
        return Classes;
    }

    FParticleSystemEditorTool::FParticleSystemEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
        , ParticleEntity()
        , DirectionalLightEntity()
        , EmitterStack(nullptr)
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
            GetPropertyTable()->DrawTree();
        });

        const FString StackName = "AssetParticleStack";
        EmitterStack = Cast<CParticleEmitterStack>(Asset->GetPackage()->LoadObjectByName(StackName));
        if (EmitterStack == nullptr)
        {
            EmitterStack = NewObject<CParticleEmitterStack>(Asset->GetPackage(), StackName);
        }
        EmitterStack->EnsureDefaultStack();

        // Live preview: recompile on module-input edit finish (mouse-up). System-setting edits
        // (no module selected) feed the sim uniforms directly and need no recompile.
        GetPropertyTable()->SetFinishEditCallback([this](const FPropertyChangedEvent&)
        {
            if (SelectedModule != nullptr)
            {
                Compile();
            }
        });

        // Default the Details panel to the system-wide settings.
        SelectModule(nullptr);
    }

    void FParticleSystemEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        EmitterStack = nullptr;
    }

    void FParticleSystemEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Stack",
            "An emitter is built from a Spawn stack (runs once per particle) and an Update stack "
            "(runs every frame). Add modules with the + buttons and reorder them, order matters.");
        DrawHelpTextRow("Modules",
            "Each module is one behavior (shape, velocity, gravity, color over life, ...). Select a "
            "module to edit its inputs in the Details panel. Toggle the checkbox to disable it.");
        DrawHelpTextRow("Compile",
            "Compile bakes the stacks into the asset's compute shader. Save also compiles. "
            "Place 'Solve Forces and Velocity' last in Update so all forces are applied first.");
        DrawHelpTextRow("Preview",
            "The viewport spawns the system at the origin. System-wide spawn rate, lifetime budget "
            "and render settings live in the Details panel when no module is selected.");
    }

    void FParticleSystemEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        if (ImGui::MenuItem(LE_ICON_RECEIPT_TEXT" Compile"))
        {
            Compile();
        }
    }

    void FParticleSystemEditorTool::SelectModule(CParticleModule* Module)
    {
        SelectedModule = Module;
        if (Module != nullptr)
        {
            GetPropertyTable()->SetObject(Module, Module->GetClass());
        }
        else
        {
            GetPropertyTable()->SetObject(Asset, Asset->GetClass());
        }
    }

    void FParticleSystemEditorTool::DrawStack()
    {
        if (EmitterStack == nullptr)
        {
            return;
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

        // System header selects the asset so its sim / render settings show in Details.
        const bool bSystemSelected = (SelectedModule == nullptr);
        if (ImGui::Selectable(LE_ICON_COG" System Settings", bSystemSelected))
        {
            SelectModule(nullptr);
        }
        ImGui::Spacing();

        DrawStackSection(EParticleModuleStage::Spawn,  "Particle Spawn");
        ImGui::Spacing();
        DrawStackSection(EParticleModuleStage::Update, "Particle Update");

        ImGui::PopStyleVar(2);

        // Recompile once after the UI loop if the stack changed structurally this frame.
        if (bStackDirty)
        {
            bStackDirty = false;
            Compile();
        }
    }

    void FParticleSystemEditorTool::DrawStackSection(EParticleModuleStage Stage, const char* Label)
    {
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
            PendingAddStage = Stage;
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
                bStackDirty = true;
            }
            ImGui::SameLine();

            const uint32 Accent = Module->GetAccentColor();
            ImGui::PushStyleColor(ImGuiCol_Text, bEnabled ? Accent : IM_COL32(120, 120, 125, 255));
            const bool bSelected = (SelectedModule == Module);
            const float Remaining = ImGui::GetContentRegionAvail().x - Cluster;
            const float RowWidth = Remaining > 40.0f ? Remaining : 40.0f;
            if (ImGui::Selectable(Module->GetDisplayName().c_str(), bSelected, 0, ImVec2(RowWidth, 0)))
            {
                SelectModule(Module);
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && !Module->GetTooltip().empty())
            {
                ImGui::SetTooltip("%s", Module->GetTooltip().c_str());
            }

            ImGui::SameLine();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::Button(LE_ICON_ARROW_UP"##up", ImVec2(BtnW, 0))) { EmitterStack->MoveModule(Module, -1); Asset->GetPackage()->MarkDirty(); bStackDirty = true; }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(i == (int32)Stack.size() - 1);
            if (ImGui::Button(LE_ICON_ARROW_DOWN"##down", ImVec2(BtnW, 0))) { EmitterStack->MoveModule(Module, 1); Asset->GetPackage()->MarkDirty(); bStackDirty = true; }
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
                SelectModule(nullptr);
            }
            EmitterStack->RemoveModule(PendingRemove);
            Asset->GetPackage()->MarkDirty();
            bStackDirty = true;
        }

        DrawAddModulePopup(Stage);

        ImGui::PopID();
    }

    void FParticleSystemEditorTool::DrawAddModulePopup(EParticleModuleStage Stage)
    {
        if (PendingAddStage != Stage)
        {
            return;
        }

        if (ImGui::BeginPopup("##AddModule"))
        {
            ImGui::TextDisabled("Add Module");
            ImGui::Separator();

            FString CurrentCategory;
            for (CClass* ModuleClass : GetModuleClasses())
            {
                CParticleModule* CDO = ModuleClass->GetDefaultObject<CParticleModule>();
                if (CDO == nullptr || CDO->GetStage() != Stage)
                {
                    continue;
                }

                if (CDO->GetCategory() != CurrentCategory)
                {
                    CurrentCategory = CDO->GetCategory();
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 150, 155, 255));
                    ImGui::TextUnformatted(CurrentCategory.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::Indent(10.0f);
                if (ImGui::Selectable(CDO->GetDisplayName().c_str()))
                {
                    CParticleModule* Added = EmitterStack->AddModule(ModuleClass);
                    if (Added != nullptr)
                    {
                        SelectModule(Added);
                        Asset->GetPackage()->MarkDirty();
                        bStackDirty = true;
                    }
                }
                if (ImGui::IsItemHovered() && !CDO->GetTooltip().empty())
                {
                    ImGui::SetTooltip("%s", CDO->GetTooltip().c_str());
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
        if (PS == nullptr || EmitterStack == nullptr)
        {
            return;
        }

        FParticleCompiler Compiler;
        EmitterStack->CompileStacks(Compiler);

        if (Compiler.HasErrors())
        {
            for (const EdNodeGraph::FError& Error : Compiler.GetErrors())
            {
                CompilationResult.CompilationLog += "ERROR - [" + Error.Name + "]: " + Error.Description + "\n";
            }
            CompilationResult.bIsError = true;
            return;
        }

        const FString Source = Compiler.BuildShader();
        if (Source.empty())
        {
            CompilationResult.CompilationLog = "Failed to build shader source.";
            CompilationResult.bIsError = true;
            return;
        }

        IShaderCompiler* ShaderCompiler = GShaderCompiler;
        ShaderCompiler->CompilerShaderRaw(Source, {}, [this](const FShaderHeader& Header) mutable
        {
            CParticleSystem* PS = Cast<CParticleSystem>(Asset.Get());
            PS->ComputeShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
        });
        ShaderCompiler->Flush();

        // Route the renderer to the generated module-stack shader.
        PS->ShaderMode = EParticleShaderMode::Custom;
        PS->PostLoad();
        PS->GetPackage()->MarkDirty();
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
