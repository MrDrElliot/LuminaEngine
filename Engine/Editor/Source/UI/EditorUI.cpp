#include "EditorUI.h"
#include <cfloat>
#include <filesystem>
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_internal.h>
#include <Lumina.h>
#include <string.h>
#include <Windows.h>
#include <Assets/AssetRegistry/AssetData.h>
#include <Containers/Array.h>
#include <Containers/Function.h>
#include <Containers/String.h>
#include <Core/UpdateContext.h>
#include <Core/Assertions/Assert.h>
#include <Core/Engine/Engine.h>
#include <Core/Math/Math.h>
#include <Core/Math/Transform.h>
#include <Core/Object/ObjectArray.h>
#include <Core/Object/ObjectCore.h>
#include <Core/Object/ObjectFlags.h>
#include <Core/Templates/LuminaTemplate.h>
#include <EASTL/algorithm.h>
#include <EASTL/fixed_string.h>
#include <EASTL/string.h>
#include <Events/Event.h>
#include <FileSystem/FileSystem.h>
#include <GLFW/glfw3.h>
#include <glm/fwd.hpp>
#include <GUID/GUID.h>
#include <Memory/SmartPtr.h>
#include <Paths/Paths.h>
#include <Platform/GenericPlatform.h>
#include <Renderer/Shader.h>
#include <World/World.h>
#include "implot.h"
#include "LuminaEditor.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Config/Config.h"
#include "Core/Application/Application.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/Profile.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "EASTL/sort.h"
#include "Input/InputProcessor.h"
#include "Memory/Memory.h"
#include "Platform/Process/PlatformProcess.h"
#include "Properties/Customizations/CoreTypeCustomization.h"
#include "Properties/Customizations/ScriptComponentCustomization.h"
#include "Renderer/RenderContext.h"
#include "Renderer/RenderDocImpl.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHIGlobals.h"
#include "Renderer/ShaderCompiler.h"
#include "Scripting/ScriptPath.h"
#include "Scripting/Lua/Scripting.h"
#include "Tools/ConsoleLogEditorTool.h"
#include "Tools/ContentBrowserEditorTool.h"
#include "Tools/EditorTool.h"
#include "Tools/EditorToolModal.h"
#include "Tools/GamePreviewTool.h"
#include "Tools/ToolFlags.h"
#include "Tools/WorldEditorTool.h"
#include "Tools/AssetEditors/Animation/AnimationEditorTool.h"
#include "Tools/AssetEditors/MaterialEditor/MaterialEditorTool.h"
#include "Tools/AssetEditors/MaterialEditor/MaterialInstanceEditorTool.h"
#include "Tools/AssetEditors/MeshEditor/MeshEditorTool.h"
#include "Tools/AssetEditors/MeshEditor/SkeletalMeshEditorTool.h"
#include "Tools/AssetEditors/MeshEditor/SkeletonEditorTool.h"
#include "Tools/AssetEditors/PrefabEditor/PrefabEditorTool.h"
#include "Tools/AssetEditors/TextureEditor/TextureEditorTool.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiRenderer.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/WorldManager.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Scene/RenderScene/RenderScene.h"

namespace Lumina
{
    bool FEditorUI::OnEvent(FEvent& Event)
    {
        for (FEditorTool* Tool : EditorTools)
        {
            if (Tool->OnEvent(Event))
            {
                return true;
            }
        }

        return false;
    }

    void FEditorUI::Initialize(const FUpdateContext& UpdateContext)
    {
        ImGuiContext* Context = GRenderManager->GetImGuiRenderer()->GetImGuiContext();
        ImPlotContext* PlotContext = GRenderManager->GetImGuiRenderer()->GetImPlotContext();
        ImGui::SetCurrentContext(Context);
        ImPlot::SetCurrentContext(PlotContext);
        
        PropertyCustomizationRegistry = Memory::New<FPropertyCustomizationRegistry>();
        PropertyCustomizationRegistry->RegisterPropertyCustomization(TBaseStructure<glm::vec2>::Get()->GetName(), []
        {
            return FVec2PropertyCustomization::MakeInstance();
        });
        PropertyCustomizationRegistry->RegisterPropertyCustomization(TBaseStructure<glm::vec3>::Get()->GetName(), []
        {
            return FVec3PropertyCustomization::MakeInstance();
        });
        PropertyCustomizationRegistry->RegisterPropertyCustomization(TBaseStructure<glm::vec4>::Get()->GetName(), []
        {
            return FVec4PropertyCustomization::MakeInstance();
        });
        PropertyCustomizationRegistry->RegisterPropertyCustomization(TBaseStructure<glm::quat>::Get()->GetName(), []
        {
            return FVec3PropertyCustomization::MakeInstance();
        });
        PropertyCustomizationRegistry->RegisterPropertyCustomization(TBaseStructure<FTransform>::Get()->GetName(), []
        {
            return FTransformPropertyCustomization::MakeInstance();
        });
        PropertyCustomizationRegistry->RegisterPropertyCustomization(SScriptComponent::StaticStruct()->GetName(), []
        {
           return FScriptComponentPropertyCustomization::MakeInstance(); 
        });
        
        
        EditorWindowClass.ClassId                       = ImHashStr("EditorWindowClass");
        EditorWindowClass.DockingAllowUnclassed         = false;
        EditorWindowClass.ViewportFlagsOverrideSet      = ImGuiViewportFlags_NoAutoMerge;
        EditorWindowClass.ViewportFlagsOverrideClear    = ImGuiViewportFlags_NoTaskBarIcon;
        EditorWindowClass.ParentViewportId              = 0; // Top level window
        EditorWindowClass.DockingAlwaysTabBar           = true;

        WorldEditorTool = CreateTool<FWorldEditorTool>(this, NewObject<CWorld>(nullptr, "Transient World", FGuid::New(), OF_Transient));
        ConsoleLogTool = CreateTool<FConsoleLogEditorTool>(this);
        ContentBrowser = CreateTool<FContentBrowserEditorTool>(this);
    }

    void FEditorUI::Deinitialize(const FUpdateContext& UpdateContext)
    {
        while (!EditorTools.empty())
        {
            // Pops internally.
            DestroyTool(UpdateContext, EditorTools[0]);
        }
        
        WorldEditorTool = nullptr;
        ConsoleLogTool = nullptr;
        ImGui::SetCurrentContext(nullptr);
    }

    void FEditorUI::OnStartFrame(const FUpdateContext& UpdateContext)
    {
        LUMINA_PROFILE_SCOPE();
        DEBUG_ASSERT(UpdateContext.GetUpdateStage() == EUpdateStage::FrameStart);
        ImGuizmo::BeginFrame();

        auto TitleBarLeftContents = [this, &UpdateContext] ()
        {
            DrawTitleBarMenu(UpdateContext);
        };

        auto TitleBarRightContents = [this, &UpdateContext] ()
        {
            DrawTitleBarInfoStats(UpdateContext);
        };

        TitleBar.Draw(TitleBarLeftContents, 400, TitleBarRightContents, 230);
        
        const ImGuiID DockspaceID = ImGui::GetID("EditorDockSpace");

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        constexpr ImGuiWindowFlags WindowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("EditorDockSpaceWindow", nullptr, WindowFlags);
        
        ImGui::PopStyleVar(3);
        {
            if (!ImGui::DockBuilderGetNode(DockspaceID))
            {
                ImGui::DockBuilderAddNode(DockspaceID, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(DockspaceID, ImGui::GetContentRegionAvail());

                ImGuiID TopDockID = 0, BottomDockID = 0;
                ImGui::DockBuilderSplitNode(DockspaceID, ImGuiDir_Down, 0.3f, &BottomDockID, &TopDockID);
                
                ImGui::DockBuilderFinish(DockspaceID);

                ImGui::DockBuilderDockWindow(WorldEditorTool->GetToolName().c_str(), TopDockID);
                ImGui::DockBuilderDockWindow(ContentBrowser->GetToolName().c_str(), BottomDockID);
                ImGui::DockBuilderDockWindow(ConsoleLogTool->GetToolName().c_str(), BottomDockID);
            }

            // Create the actual dock space
            ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0);
            ImGui::DockSpace(DockspaceID, viewport->WorkSize, 0, &EditorWindowClass);
            ImGui::PopStyleVar();
        }
        
        
        ImGui::End();
        
        if (ImGui::IsKeyPressed(ImGuiKey_F5))
        {
            GRenderContext->CompileEngineShaders();
            CMaterial::CreateDefaultMaterial();
        }
        

        if (!FocusTargetWindowName.empty())
        {
            ImGuiWindow* Window = ImGui::FindWindowByName(FocusTargetWindowName.c_str());
            if (Window == nullptr || Window->DockNode == nullptr || Window->DockNode->TabBar == nullptr)
            {
                FocusTargetWindowName.clear();
                return;
            }

            ImGuiID TabID = 0;
            for (int i = 0; i < Window->DockNode->TabBar->Tabs.size(); ++i)
            {
                ImGuiTabItem* pTab = &Window->DockNode->TabBar->Tabs[i];
                if (pTab->Window->ID == Window->ID)
                {
                    TabID = pTab->ID;
                    break;
                }
            }

            if (TabID != 0)
            {
                Window->DockNode->TabBar->NextSelectedTabId = TabID;
                ImGui::SetWindowFocus(FocusTargetWindowName.c_str());
            }

            FocusTargetWindowName.clear();
            
        }
        
        if (bShowContributors)
        {
            ImGui::SetNextWindowSize(ImVec2(850, 650), ImGuiCond_FirstUseEver);
            
            if (ImGui::Begin("Contributors", &bShowContributors, ImGuiWindowFlags_NoCollapse))
            {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "Project Contributors");
                
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "The talented people behind this project");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
        
                ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg |ImGuiTableFlags_ScrollY;
                
                if (ImGui::BeginTable("##ContributorsTable", 2, flags))
                {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 200);
                    ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();
                    
                    auto AddContributor = [](const char* Name, const char* Role, ImVec4 NameColor = ImVec4(0.3f, 0.8f, 1.0f, 1.0f))
                    {
                        ImGui::TableNextRow();
                        
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Spacing();
                        ImGui::TextColored(NameColor, "%s", Name);
                        
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Spacing();
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
                        ImGui::TextWrapped("%s", Role);
                        ImGui::PopStyleColor();
                    };
                    
                    // Core Team
                    AddContributor("Bryan Casagrande", "Lead Developer & Engine Architect", ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
                    
                    // Contributors
                    AddContributor("Marzac", "Spark");
                    AddContributor("Tiny Butch", "Spark");
        
                    ImGui::EndTable();
                }
                
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Thank you to everyone who contributed!");
                
                ImGui::End();
            }
        }

        if (bShowLuminaInfo)
        {
            ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));

            if (ImGui::Begin("About Lumina Engine", &bShowLuminaInfo, ImGuiWindowFlags_NoCollapse))
            {
                // Hero Section
                ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
                ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "LUMINA ENGINE");
                ImGui::PopFont();

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::Text("A modern, high-performance game engine built with Vulkan.");
                ImGui::PopStyleColor();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Spacing();

                // Engine Information Section
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "Engine Information");
                ImGui::Spacing();

                if (ImGui::BeginTable("##EngineInfoTable", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX))
                {
                    ImGui::TableSetupColumn("##Property", ImGuiTableColumnFlags_WidthFixed, 180);
                    ImGui::TableSetupColumn("##Value", ImGuiTableColumnFlags_WidthStretch);

                    // Version
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                    ImGui::Text("Version");
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "%s", LUMINA_VERSION);

                    // Rendering API
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                    ImGui::Text("Rendering API");
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("Vulkan 1.3");

                    // Build Configuration
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                    ImGui::Text("Build Configuration");
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
#ifdef LUMINA_DEBUG
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Debug");
#elif defined(LUMINA_RELEASE)
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "Release");
#else
                    ImGui::Text("Development");
#endif

                    // Platform
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                    ImGui::Text("Platform");
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
#ifdef _WIN32
                    ImGui::Text("Windows x64");
#elif defined(__linux__)
                    ImGui::Text("Linux x64");
#elif defined(__APPLE__)
                    ImGui::Text("macOS");
#endif

                    // Compiler
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                    ImGui::Text("Compiler");
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
#ifdef _MSC_VER
                    ImGui::Text("MSVC %d", _MSC_VER);
#elif defined(__clang__)
                    ImGui::Text("Clang %d.%d.%d", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
                    ImGui::Text("GCC %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif

                    ImGui::EndTable();
                }

                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Spacing();

                // Core Features Section
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "Core Features");
                ImGui::Spacing();

                ImGui::BulletText("Physically Based Rendering (PBR)");
                ImGui::BulletText("Advanced Material System");
                ImGui::BulletText("Multi-threaded Asset Pipeline");
                ImGui::BulletText("Hot-reload Shader Compilation");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                ImGui::Text("Licensed under the Apache 2.0 License");
                ImGui::Text("Copyright (c) 2025 Lumina Engine Contributors");
                ImGui::PopStyleColor();

                ImGui::Spacing();

                ImGui::Separator();
                ImGui::Spacing();

                float buttonWidth = 150.0f;
                float availWidth = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX((availWidth - buttonWidth * 2 - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

                if (ImGui::Button("Documentation", ImVec2(buttonWidth, 0)))
                {
                    Platform::LaunchURL(TEXT("https://github.com/MrDrElliot/LuminaEngine"));
                }

                ImGui::SameLine();

                if (ImGui::Button("GitHub", ImVec2(buttonWidth, 0)))
                {
                    Platform::LaunchURL(TEXT("https://github.com/MrDrElliot/LuminaEngine"));
                }
            }

            ImGui::End();
            ImGui::PopStyleVar();
        }

        if (bShowDearImGuiDemoWindow)
        {
            ImGui::ShowDemoWindow(&bShowDearImGuiDemoWindow);
        }
        
        if (bShowImGuiStyleEditor)
        {
            ImGui::ShowStyleEditor();
        }

        if (bShowImPlotDemoWindow)
        {
            ImPlot::ShowDemoWindow(&bShowImPlotDemoWindow);
        }

        if (bShowRenderDebug)
        {
            GRenderManager->GetImGuiRenderer()->DrawRenderDebugInformationWindow(&bShowRenderDebug, UpdateContext);
        }
        
        if (bShowScriptsDebug)
        {
            DrawScripts();
        }

        if (bShowObjectDebug)
        {
            DrawObjectList();
        }

        if (GEngine->IsCloseRequested())
        {
            static bool IsVerifyingPackages = false;
            if (IsVerifyingPackages == false)
            {
                IsVerifyingPackages = true;
                VerifyDirtyPackages();
            }

            if (ModalManager.HasModal())
            {
                GEngine->SetEngineReadyToClose(false);
            }
        }
        
        FEditorTool* ToolToClose = nullptr;
        
        for (FEditorTool* Tool : EditorTools)
        {
            if (!SubmitToolMainWindow(UpdateContext, Tool, DockspaceID))
            {
                ToolToClose = Tool;
            }
        }

        for (FEditorTool* Tool : EditorTools)
        {
            if (Tool == ToolToClose)
            {
                continue;
            }
            
            DrawToolContents(UpdateContext, Tool);
        }

        
        if (ToolToClose)
        {
            ToolsPendingDestroy.push(ToolToClose);
        }

        while (!ToolsPendingDestroy.empty())
        {
            FEditorTool* Tool = ToolsPendingDestroy.front();
            ToolsPendingDestroy.pop();

            DestroyTool(UpdateContext, Tool);
        }
        
        while (!ToolsPendingAdd.empty())
        {
            FEditorTool* NewTool = ToolsPendingAdd.front();
            ToolsPendingAdd.pop();

            EditorTools.push_back(NewTool);
        }

        if (GEditorEngine->GetProjectName().empty())
        {
            OpenProjectManagerDialog(EProjectManagerTab::OpenProject);
        }
        
        ModalManager.DrawDialogue();
    }

    void FEditorUI::OnUpdate(const FUpdateContext& UpdateContext)
    {
        LUMINA_PROFILE_SCOPE();
        
        for (FEditorTool* Tool : EditorTools)
        {
            if (Tool->HasWorld())
            {
                Tool->WorldUpdate(UpdateContext);
            }
        }
    }

    void FEditorUI::OnEndFrame(const FUpdateContext& UpdateContext)
    {
        LUMINA_PROFILE_SCOPE();

        for (FEditorTool* Tool : EditorTools)
        {
            Tool->EndFrame();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_LeftShift) && ImGui::IsKeyPressed(ImGuiKey_Z))
        {
            TransactionManager.Redo();
        }
        
        if (ImGui::IsKeyPressed(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_Z))
        {
            TransactionManager.Undo();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && GamePreviewTool != nullptr)
        {
            WorldEditorTool->GetOnPreviewStopRequestedDelegate().Broadcast();
        }
    }
    
    void FEditorUI::DestroyTool(const FUpdateContext& UpdateContext, FEditorTool* Tool)
    {
        auto Itr = eastl::find(EditorTools.begin(), EditorTools.end(), Tool);
        ASSERT(Itr != EditorTools.end());

        EditorTools.erase(Itr);
        
        for (auto MapItr = ActiveAssetTools.begin(); MapItr != ActiveAssetTools.end(); ++MapItr)
        {
            if (MapItr->second == Tool)
            {
                ActiveAssetTools.erase(MapItr);
                break;
            }
        }

        if (Tool == GamePreviewTool)
        {
            WorldEditorTool->NotifyPlayInEditorStop();
            GamePreviewTool = nullptr;
        }
        
        Tool->Deinitialize(UpdateContext);
        Memory::Delete(Tool);
    }

    void FEditorUI::PushModal(const FString& Title, ImVec2 Size, TMoveOnlyFunction<bool()> DrawFunction)
    {
        ModalManager.CreateDialogue(Title, Size, Move(DrawFunction));
    }

    void FEditorUI::OpenScriptEditor(FStringView ScriptPath)
    {
        Platform::LaunchURL(StringUtils::ToWideString(ScriptPath.data()).c_str());
    }

    void FEditorUI::OpenAssetEditor(const FGuid& AssetGUID)
    {
        CObject* Asset = LoadObject<CObject>(AssetGUID);
        
        if (Asset == nullptr)
        {
            return;
        }
        
        auto Itr = ActiveAssetTools.find(Asset);
        if (Itr != ActiveAssetTools.end())
        {
            const char* Name = Itr->second->GetToolName().c_str();
            ImGui::SetWindowFocus(Name);
            return;
        }

        if (WorldEditorTool->GetWorld() == Asset)
        {
            const char* Name = WorldEditorTool->GetToolName().c_str();
            ImGui::SetWindowFocus(Name);
            return;
        }
        
        FEditorTool* NewTool = nullptr;
        if (Asset->IsA<CMaterial>())
        {
            NewTool = CreateTool<FMaterialEditorTool>(this, Asset);
        }
        else if (Asset->IsA<CTexture>())
        {
            NewTool = CreateTool<FTextureEditorTool>(this, Asset);
        }
        else if (Asset->IsA<CStaticMesh>())
        {
            NewTool = CreateTool<FStaticMeshEditorTool>(this, Asset);
        }
        else if (Asset->IsA<CSkeleton>())
        {
            NewTool = CreateTool<FSkeletonEditorTool>(this, Asset);
        }
        else if (Asset->IsA<CAnimation>())
        {
            NewTool = CreateTool<FAnimationEditorTool>(this, Asset);
        }
        else if (Asset->IsA<CSkeletalMesh>())
        {
            NewTool = CreateTool<FSkeletalMeshEditorTool>(this, Asset);
        }
        else if (Asset->IsA<CMaterialInstance>())
        {
            NewTool = CreateTool<FMaterialInstanceEditorTool>(this, Asset);
        }
        else if (Asset->IsA<CPrefab>())
        {
            NewTool = CreateTool<FPrefabEditorTool>(this, Asset);
        }
        else if (Asset->IsA<CWorld>())
        {
            if (WorldEditorTool->HasSimulatingWorld())
            {
                WorldEditorTool->StopAllSimulations();
            }
            
            WorldEditorTool->SetWorld(Cast<CWorld>(Asset));
        }

        if (NewTool)
        {
            ActiveAssetTools.insert_or_assign(Asset, NewTool);
        }
    }

    void FEditorUI::OnDestroyAsset(CObject* InAsset)
    {
        if (ActiveAssetTools.find(InAsset) != ActiveAssetTools.end())
        {
            ToolsPendingDestroy.push(ActiveAssetTools.at(InAsset));
        }
    }

    void FEditorUI::EditorToolLayoutCopy(FEditorTool* SourceTool)
    {
        LUMINA_PROFILE_SCOPE();

        ImGuiID sourceToolID = SourceTool->GetPrevDockspaceID();
        ImGuiID destinationToolID = SourceTool->GetCurrDockspaceID();
        ASSERT(sourceToolID != 0 && destinationToolID != 0);
        
        // Helper to build an array of strings pointer into the same contiguous memory buffer.
        struct ContiguousStringArrayBuilder
        {
            void AddEntry(const char* data, size_t dataLength)
            {
                const int32 bufferSize = (int32_t) m_buffer.size();
                m_offsets.push_back( bufferSize );
                const int32 offset = bufferSize;
                m_buffer.resize( bufferSize + (int32_t) dataLength );
                memcpy( m_buffer.data() + offset, data, dataLength );
            }

            void BuildPointerArray( ImVector<const char*>& outArray )
            {
                outArray.resize( (int32_t) m_offsets.size() );
                for (int32 n = 0; n < (int32) m_offsets.size(); n++)
                {
                    outArray[n] = m_buffer.data() + m_offsets[n];
                }
            }

            TFixedVector<char, 100>       m_buffer;
            TFixedVector<int32, 100>    m_offsets;
        };

        ContiguousStringArrayBuilder namePairsBuilder;

        for (auto& Window : SourceTool->ToolWindows)
        {
            const FFixedString sourceToolWindowName = FEditorTool::GetToolWindowName(Window->Name.c_str(), sourceToolID);
            const FFixedString destinationToolWindowName = FEditorTool::GetToolWindowName(Window->Name.c_str(), destinationToolID);
            namePairsBuilder.AddEntry( sourceToolWindowName.c_str(), sourceToolWindowName.length() + 1 );
            namePairsBuilder.AddEntry( destinationToolWindowName.c_str(), destinationToolWindowName.length() + 1 );
        }

        // Perform the cloning
        if (ImGui::DockContextFindNodeByID( ImGui::GetCurrentContext(), sourceToolID))
        {
            // Build the same array with char* pointers at it is the input of DockBuilderCopyDockspace() (may change its signature?)
            ImVector<const char*> windowRemapPairs;
            namePairsBuilder.BuildPointerArray(windowRemapPairs);

            ImGui::DockBuilderCopyDockSpace(sourceToolID, destinationToolID, &windowRemapPairs);
            ImGui::DockBuilderFinish(destinationToolID);
        }
    }

    bool FEditorUI::SubmitToolMainWindow(const FUpdateContext& UpdateContext, FEditorTool* EditorTool, ImGuiID TopLevelDockspaceID)
    {
        LUMINA_PROFILE_SCOPE();
        ASSERT(EditorTool != nullptr);
        ASSERT(TopLevelDockspaceID != 0);

        bool bIsToolStillOpen = true;
        bool* bIsToolOpen = (EditorTool == WorldEditorTool) ? nullptr : &bIsToolStillOpen; // Prevent closing the map-editor editor tool
        
        // Top level editors can only be docked with each others
        ImGui::SetNextWindowClass(&EditorWindowClass);
        if (EditorTool->GetDesiredDockID() != 0)
        {
            ImGui::SetNextWindowDockID(EditorTool->GetDesiredDockID());
            EditorTool->DesiredDockID = 0;
        }
        else
        {
            ImGui::SetNextWindowDockID(TopLevelDockspaceID, ImGuiCond_FirstUseEver);
        }
        
        ImGuiWindowFlags WindowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar;
        if (EditorTool->IsUnsavedDocument())
        {
            WindowFlags |= ImGuiWindowFlags_UnsavedDocument;
        }
        
        ImGuiWindow* CurrentWindow = ImGui::FindWindowByName(EditorTool->GetToolName().c_str());
        const bool bVisible = CurrentWindow != nullptr && !CurrentWindow->Hidden;
        
        ImVec4 VisibleColor   = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        ImVec4 NotVisibleColor = ImVec4(0.4f, 0.4f, 0.4f, 1.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, bVisible ? VisibleColor : NotVisibleColor);
        ImGui::SetNextWindowSizeConstraints(ImVec2(128, 128), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(1024, 768), ImGuiCond_FirstUseEver);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.5f);
        ImGui::Begin(EditorTool->GetToolName().c_str(), bIsToolOpen, WindowFlags);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_DockHierarchy))
        {
            LastActiveTool = EditorTool;
        }
        
        // Set WindowClass based on per-document ID, so tabs from Document A are not dockable in Document B etc. We could be using any ID suiting us, e.g. &doc
        // We also set ParentViewportId to request the platform back-end to set parent/child relationship at the windowing level
        EditorTool->ToolWindowsClass.ClassId = EditorTool->GetID();
        EditorTool->ToolWindowsClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoTaskBarIcon | ImGuiViewportFlags_NoDecoration;
        EditorTool->ToolWindowsClass.ParentViewportId = ImGui::GetWindowViewport()->ID;
        EditorTool->ToolWindowsClass.DockingAllowUnclassed = true;

        // Track LocationID change so we can fork/copy the layout data according to where the window is going + reference count
        // LocationID ~~ (DockId != 0 ? DockId : DocumentID) // When we are in a loose floating window we use our own document id instead of the dock id
        EditorTool->CurrDockID = ImGui::GetWindowDockID();
        EditorTool->PrevLocationID = EditorTool->CurrLocationID;
        EditorTool->CurrLocationID = EditorTool->CurrDockID != 0 ? EditorTool->CurrDockID : EditorTool->GetID();

        // Dockspace ID ~~ Hash of LocationID + DocType
        // So all editors of a same type inside a same tab-bar will share the same layout.
        // We will also use this value as a suffix to create window titles, but we could perfectly have an indirection to allocate and use nicer names for window names (e.g. 0001, 0002).
        EditorTool->PrevDockspaceID = EditorTool->CurrDockspaceID;
        EditorTool->CurrDockspaceID = EditorTool->CalculateDockspaceID();
        ASSERT(EditorTool->CurrDockspaceID != 0);
        

        ImGui::End();

        return bIsToolStillOpen;
    }

    void FEditorUI::DrawToolContents(const FUpdateContext& UpdateContext, FEditorTool* Tool)
    {
        LUMINA_PROFILE_SCOPE();

        // This is the second Begin(), as SubmitToolMainWindow() has already done one
        // (Therefore only the p_open and flags of the first call to Begin() applies)
        ImGui::Begin(Tool->GetToolName().c_str());
        
        ASSERT(ImGui::GetCurrentWindow()->BeginCount == 2);
        
        const ImGuiID dockspaceID = Tool->GetCurrentDockspaceID();
        const ImVec2 DockspaceSize = ImGui::GetContentRegionAvail();

        if (Tool->PrevLocationID != 0 && Tool->PrevLocationID != Tool->CurrLocationID)
        {
            int PrevDockspaceRefCount = 0;
            int CurrDockspaceRefCount = 0;
            for (FEditorTool* OtherTool : EditorTools)
            {
                if (OtherTool->CurrDockspaceID == Tool->PrevDockspaceID)
                {
                    PrevDockspaceRefCount++;
                }
                else if (OtherTool->CurrDockspaceID == Tool->CurrDockspaceID)
                {
                    CurrDockspaceRefCount++;
                }
            }

            // Fork or overwrite settings
            // FIXME: should be able to do a "move window but keep layout" if CurrDockspaceRefCount > 1.
            // FIXME: when moving, delete settings of old windows
            EditorToolLayoutCopy(Tool);

            if (PrevDockspaceRefCount == 0)
            {
                ImGui::DockBuilderRemoveNode(Tool->PrevDockspaceID);

                // Delete settings of old windows
                // Rely on window name to ditch their .ini settings forever.
                char windowSuffix[16];
                ImFormatString(windowSuffix, IM_ARRAYSIZE(windowSuffix), "##%08X", Tool->PrevDockspaceID);
                size_t windowSuffixLength = strlen(windowSuffix);
                ImGuiContext& g = *GImGui;
                for (ImGuiWindowSettings* settings = g.SettingsWindows.begin(); settings != nullptr; settings = g.SettingsWindows.next_chunk(settings))
                {
                    if ( settings->ID == 0 )
                    {
                        continue;
                    }
                    
                    
                    char const* pWindowName = settings->GetName();
                    size_t windowNameLength = strlen(pWindowName);
                    if (windowNameLength >= windowSuffixLength)
                    {
                        if (strcmp(pWindowName + windowNameLength - windowSuffixLength, windowSuffix) == 0) // Compare suffix
                        {
                            ImGui::ClearWindowSettings(pWindowName);
                        }
                    }
                }
            }
        }
        else if (ImGui::DockBuilderGetNode(Tool->GetCurrentDockspaceID()) == nullptr)
        {
            ImVec2 dockspaceSize = ImGui::GetContentRegionAvail();
            dockspaceSize.x = eastl::max(dockspaceSize.x, 1.0f);
            dockspaceSize.y = eastl::max(dockspaceSize.y, 1.0f);

            ImGui::DockBuilderAddNode(Tool->GetCurrentDockspaceID(), ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(Tool->GetCurrentDockspaceID(), dockspaceSize);
            if (!Tool->IsSingleWindowTool())
            {
                Tool->InitializeDockingLayout(Tool->GetCurrentDockspaceID(), dockspaceSize);
            }
            ImGui::DockBuilderFinish(Tool->GetCurrentDockspaceID());
        }

        // FIXME-DOCK: This is a little tricky to explain, but we currently need this to use the pattern of sharing a same dockspace between tabs of a same tab bar
        bool bVisible = true;
        if (ImGui::GetCurrentWindow()->Hidden)
        {
            bVisible = false;
        }
        
        const bool bIsLastFocusedTool = (LastActiveTool == Tool);
        
        Tool->Update(UpdateContext);
        Tool->bViewportFocused = false;
        Tool->bViewportHovered = false;

        if (Tool->HasWorld())
        {
            Tool->GetWorld()->SetActive(bVisible);
        }
        
        if (!bVisible)
        {
            if (!Tool->IsSingleWindowTool())
            {
                // Keep alive document dockspace so windows that are docked into it but which visibility are not linked to the dockspace visibility won't get undocked.
                ImGui::DockSpace(dockspaceID, DockspaceSize, ImGuiDockNodeFlags_KeepAliveOnly, &Tool->ToolWindowsClass);
            }
            
            ImGui::End();
            
            return;
        }

        
        if (Tool->HasFlag(EEditorToolFlags::Tool_WantsToolbar))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 16));
            if (ImGui::BeginMenuBar())
            {
                Tool->DrawMainToolbar(UpdateContext);
                ImGui::EndMenuBar();
            }
            ImGui::PopStyleVar();
        }

        if (Tool->IsSingleWindowTool())
        {
            ASSERT(Tool->ToolWindows.size() == 1);
            Tool->ToolWindows[0]->DrawFunction(bIsLastFocusedTool);
        }
        else
        {
            ImGui::DockSpace(dockspaceID, DockspaceSize, ImGuiDockNodeFlags_None, &Tool->ToolWindowsClass);
        }
    
        ImGui::End();


        if (!Tool->IsSingleWindowTool())
        {
            for (auto& Window : Tool->ToolWindows)
            {
                LUMINA_PROFILE_SECTION("Setup and Draw Tool Window");

                const FFixedString ToolWindowName = FEditorTool::GetToolWindowName(Window->Name.c_str(), Tool->GetCurrentDockspaceID());

                // When multiple documents are open, floating tools only appear for focused one
                if (!bIsLastFocusedTool)
                {
                    if (ImGuiWindow* pWindow = ImGui::FindWindowByName(ToolWindowName.c_str()))
                    {
                        ImGuiDockNode* pWindowDockNode = pWindow->DockNode;
                        if (pWindowDockNode == nullptr && pWindow->DockId != 0)
                        {
                            pWindowDockNode = ImGui::DockContextFindNodeByID(ImGui::GetCurrentContext(), pWindow->DockId);
                        }
                       
                        if (pWindowDockNode == nullptr || ImGui::DockNodeGetRootNode(pWindowDockNode)->ID != dockspaceID)
                        {
                            continue;
                        }
                    }
                }
            
                if (Window->bViewport)
                {
                    LUMINA_PROFILE_SECTION("Draw Viewport");

                    constexpr ImGuiWindowFlags ViewportWindowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus;
                    ImGui::SetNextWindowClass(&Tool->ToolWindowsClass);
                    
                    ImGui::SetNextWindowSizeConstraints(ImVec2(128, 128), ImVec2(FLT_MAX, FLT_MAX));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                    bool const DrawViewportWindow = ImGui::Begin(ToolWindowName.c_str(), nullptr, ViewportWindowFlags);
                    ImGui::PopStyleVar();
                
                    if (DrawViewportWindow)
                    {
                        IRenderScene* SceneRenderer = Tool->GetWorld()->GetRenderer();
                        ImTextureRef ViewportTexture = ImGuiX::ToImTextureRef(SceneRenderer->GetRenderTarget());
                        
                        Tool->bViewportFocused = ImGui::IsWindowFocused();
                        Tool->bViewportHovered = ImGui::IsWindowHovered();
                        Tool->DrawViewport(UpdateContext, ViewportTexture);
                    }

                    if (Tool->GetWorld()->GetEntityRegistry().valid(Tool->EditorEntity))
                    {
						if (FEditorComponent* EditorComp = Tool->GetWorld()->GetEntityRegistry().try_get<FEditorComponent>(Tool->EditorEntity))
                        {
                            EditorComp->bEnabled = Tool->bViewportFocused;
                        }
                    }
                    
                    ImGui::End();
                }
                else
                {
                    LUMINA_PROFILE_SECTION("Draw Tool Window");

                    ImGuiWindowFlags ToolWindowFlags = ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus;

                    ImGui::SetNextWindowClass(&Tool->ToolWindowsClass);

                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().WindowPadding);
                    bool const DrawToolWindow = ImGui::Begin(ToolWindowName.c_str(), &Window->bOpen, ToolWindowFlags);
                    ImGui::PopStyleVar();

                    if (DrawToolWindow)
                    {
                        const bool bToolWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_DockHierarchy);
                        Window->DrawFunction(bToolWindowFocused);
                    }
                    
                    ImGui::End();
                }
            }
        }
    }

    void FEditorUI::CreateGameViewportTool(const FUpdateContext& UpdateContext)
    {
    }

    void FEditorUI::DestroyGameViewportTool(const FUpdateContext& UpdateContext)
    {
        
    }

    void FEditorUI::DrawScripts()
    {
        ImGui::SetNextWindowSize(ImVec2(1200.0f, 800.0f), ImGuiCond_FirstUseEver);
    
        if (ImGui::Begin("Scripts", &bShowScriptsDebug, ImGuiWindowFlags_MenuBar))
        {
            auto& Context = Scripting::FScriptingContext::Get();
            size_t MemoryUsage = Context.GetScriptMemoryUsage();
            
            ImGui::Text("Loaded Scripts: %zu", Context.GetAllRegisteredScripts().size());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "|");
            ImGui::SameLine();
            ImGui::Text("Memory Usage: %s", ImGuiX::FormatSize(MemoryUsage).c_str());
            
            ImGui::Separator();
            
            static ImGuiTextFilter SearchFilter;
            ImGui::SetNextItemWidth(300.0f);
            SearchFilter.Draw();
            
            ImGui::Separator();
            
            static ImGuiTableFlags TableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | 
                                               ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
            
            if (ImGui::BeginTable("ScriptsTable", 3, TableFlags))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Script Name", ImGuiTableColumnFlags_WidthFixed, 250.0f);
                ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableHeadersRow();
                
                for (TSharedPtr<Scripting::FLuaScript>& Script : Context.GetAllRegisteredScripts())
                {
                    if (!SearchFilter.PassFilter(Script->Path.c_str()))
                    {
                        continue;
                    }
                    
                    ImGui::TableNextRow();
                    
                    ImGui::TableNextColumn();
                    ImGui::PushID(Script.get());
                    
                    bool bNodeOpen = ImGui::TreeNodeEx(Script->Name.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
                    
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s",Script->Path.c_str());
                    
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Reload"))
                    {
                        //Scripting::FScriptingContext::Get().R
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Edit"))
                    {
                        
                    }
                    
                    if (bNodeOpen)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Indent();
                        
                        if (ImGui::TreeNode("Environment"))
                        {
                            ImGui::Text("Global count: %zu", Script->Environment.size());
                            
                            for (const auto& [key, value] : Script->Environment)
                            {
                                sol::type ValueType = value.get_type();
                                ImGui::BulletText("%s: %s", key.as<std::string>().c_str(), sol::type_name(value.lua_state(), ValueType).c_str());
                            }
                            
                            ImGui::TreePop();
                        }
                        
                        if (Script->ScriptTable.valid() && ImGui::TreeNode("Script Table"))
                        {
                            for (const auto& [key, value] : Script->ScriptTable)
                            {
                                sol::type ValueType = value.get_type();
                                ImGui::BulletText("%s: %s",key.as<std::string>().c_str(), sol::type_name(value.lua_state(), ValueType).c_str());
                            }
                            
                            ImGui::TreePop();
                        }
                        
                        ImGui::Unindent();
                        ImGui::TreePop();
                    }
                    
                    ImGui::PopID();
                }
                
                ImGui::EndTable();
            }
            
            ImGui::End();
        }
    }

    void FEditorUI::DrawObjectList()
    {
        // State management
        static FString SearchFilter;
        static FString ClassFilter;
        static bool bSortByName = true;
        static bool bShowOnlyActive = false;
        static int SelectedObjectIndex = -1;
        static FString SelectedPackage;
        static char SearchBuffer[256] = "";
        static char ClassFilterBuffer[256] = "";
        
        ImGui::SetNextWindowSize(ImVec2(1200.0f, 800.0f), ImGuiCond_FirstUseEver);
        
        // Custom styling
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.26f, 0.59f, 0.98f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.26f, 0.59f, 0.98f, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.26f, 0.59f, 0.98f, 0.45f));
        
        FString TitleText = "Object Browser";
        
        if (ImGui::Begin(TitleText.c_str(), &bShowObjectDebug, ImGuiWindowFlags_MenuBar))
        {
            uint32 TotalObjects = GObjectArray.GetNumAliveObjects();
            
            // Menu Bar
            if (ImGui::BeginMenuBar())
            {
                if (ImGui::BeginMenu("View"))
                {
                    ImGui::MenuItem("Sort by Name", nullptr, &bSortByName);
                    ImGui::MenuItem("Show Only Active", nullptr, &bShowOnlyActive);
                    ImGui::EndMenu();
                }
                
                if (ImGui::BeginMenu("Tools"))
                {
                    if (ImGui::MenuItem("Clear Filters"))
                    {
                        SearchBuffer[0] = '\0';
                        ClassFilterBuffer[0] = '\0';
                        SearchFilter = "";
                        ClassFilter = "";
                    }
                    if (ImGui::MenuItem("Collapse All Packages"))
                    {
                        // Reset selection state
                        SelectedPackage = "";
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }
            
            // Top stats bar with gradient background
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.14f, 0.18f, 1.0f));
            ImGui::BeginChild("##StatsBar", ImVec2(0, 70.0f), true, ImGuiWindowFlags_NoScrollbar);
            {
                ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]); // Assume larger font
                
                // Stats display
                ImGui::Columns(3, nullptr, false);
                
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
                ImGui::TextWrapped("TOTAL OBJECTS");
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                ImGui::Text("%d", TotalObjects);
                ImGui::PopStyleColor();
                
                ImGui::NextColumn();
                
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.6f, 1.0f));
                ImGui::TextWrapped("PACKAGES");
                ImGui::PopStyleColor();
                
                // Count packages (will calculate below)
                static int PackageCount = 0;
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                ImGui::Text("%d", PackageCount);
                ImGui::PopStyleColor();
                
                ImGui::NextColumn();
                
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
                ImGui::TextWrapped("FILTERED");
                ImGui::PopStyleColor();
                
                static int FilteredCount = 0;
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                ImGui::Text("%d", FilteredCount);
                ImGui::PopStyleColor();
                
                ImGui::Columns(1);
                ImGui::PopFont();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            
            ImGui::Spacing();
            
            // Filter panel
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.12f, 0.15f, 1.0f));
            ImGui::BeginChild("##FilterPanel", ImVec2(0, 90.0f), true, ImGuiWindowFlags_NoScrollbar);
            {
                ImGui::Text("FILTERS");
                ImGui::Separator();
                
                ImGui::Columns(2, nullptr, false);
                
                // Search filter
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputTextWithHint("##Search", "Search objects...", SearchBuffer, IM_ARRAYSIZE(SearchBuffer)))
                {
                    SearchFilter = SearchBuffer;
                }
                
                ImGui::NextColumn();
                
                // Class filter
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputTextWithHint("##ClassFilter", "Filter by class...", ClassFilterBuffer, IM_ARRAYSIZE(ClassFilterBuffer)))
                {
                    ClassFilter = ClassFilterBuffer;
                }
                
                ImGui::Columns(1);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            
            ImGui::Spacing();
            
            // Main content area - split view
            ImGui::BeginChild("##MainContent", ImVec2(0, 0), false);
            {
                // Left panel - Package tree
                ImGui::BeginChild("##PackageTree", ImVec2(ImGui::GetContentRegionAvail().x * 0.35f, 0), true);
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
                    ImGui::Text("PACKAGES");
                    ImGui::PopStyleColor();
                    ImGui::Separator();
                    
                    // Build package map with filtering
                    THashMap<FString, TVector<CObject*>> PackageToObjects;
                    uint32 FilteredCount = 0;
                    
                    for (TObjectIterator<CObject> It; It; ++It)
                    {
                        CObject* Object = *It;
                        if (Object == nullptr)
                        {
                            continue;
                        }
        
                        // Apply filters
                        FString ObjectName = Object->GetName().ToString();
                        FString ClassName = Object->GetClass() ? Object->GetClass()->GetName().ToString() : "None";
                        
                        bool bPassesFilter = true;
                        
                        if (!SearchFilter.empty())
                        {
                            if (ObjectName.find(SearchFilter) == FString::npos)
                            {
                                bPassesFilter = false;
                            }
                        }
                        
                        if (ClassFilter.empty() && bPassesFilter && ClassName.find(ClassFilter) == FString::npos)
                        {
                            bPassesFilter = false;
                        }
                        
                        if (bShowOnlyActive && bPassesFilter)
                        {
                            // Add your active check logic here
                        }
                        
                        if (bPassesFilter)
                        {
                            FString PackageName = Object->GetPackage() ? Object->GetPackage()->GetName().ToString() : "None";
                            PackageToObjects[PackageName].push_back(Object);
                            FilteredCount++;
                        }
                    }
                    
                    for (auto& Pair : PackageToObjects)
                    {
                        const FString& PackageName = Pair.first;
                        TVector<CObject*>& Objects = Pair.second;
                        
                        if (bSortByName)
                        {
                            eastl::sort(Objects.begin(), Objects.end(), [](CObject* A, CObject* B)
                            {
                                return A->GetName().ToString() < B->GetName().ToString();
                            });
                        }
                        
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f, 0.25f, 0.30f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.35f, 0.45f, 1.0f));
                        
                        FString NodeLabel = PackageName + " (" + eastl::to_string(Objects.size()) + ")";
                        bool bIsSelected = (SelectedPackage == PackageName);
                        
                        if (ImGui::Selectable(NodeLabel.c_str(), bIsSelected, ImGuiSelectableFlags_AllowDoubleClick))
                        {
                            SelectedPackage = PackageName;
                        }
                        
                        ImGui::PopStyleColor(2);
                    }
                }
                ImGui::EndChild();
                
                ImGui::SameLine();
                
                ImGui::BeginChild("##ObjectDetails", ImVec2(0, 0), true);
                {
                    if (!SelectedPackage.empty())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
                        ImGui::Text("OBJECTS IN: %s", SelectedPackage.c_str());
                        ImGui::PopStyleColor();
                        ImGui::Separator();
                        
                        // Build filtered objects for selected package
                        THashMap<FString, TVector<CObject*>> PackageToObjects;
                        
                        for (TObjectIterator<CObject> It; It; ++It)
                        {
                            CObject* Object = *It;
                            if (Object == nullptr)
                            {
                                continue;
                            }
        
                            FString ObjectName = Object->GetName().ToString();
                            FString ClassName = Object->GetClass() ? Object->GetClass()->GetName().ToString() : "None";
                            
                            bool bPassesFilter = true;
                            
                            if (!SearchFilter.empty() && ObjectName.find(SearchFilter) == FString::npos)
                            {
                                bPassesFilter = false;
                            }
                            
                            if (ClassFilter.empty() && bPassesFilter && ClassName.find(ClassFilter) == FString::npos)
                            {
                                bPassesFilter = false;
                            }
                            
                            if (bPassesFilter)
                            {
                                FString PackageName = Object->GetPackage() ? Object->GetPackage()->GetName().ToString() : "None";
                                if (PackageName == SelectedPackage)
                                {
                                    PackageToObjects[PackageName].push_back(Object);
                                }
                            }
                        }
                        
                        if (PackageToObjects.find(SelectedPackage) != PackageToObjects.end())
                        {
                            TVector<CObject*>& Objects = PackageToObjects[SelectedPackage];
                            
                            // Advanced table with alternating colors
                            if (ImGui::BeginTable("##ObjectTable", 4, 
                                ImGuiTableFlags_RowBg | 
                                ImGuiTableFlags_Borders | 
                                ImGuiTableFlags_Resizable | 
                                ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_Sortable |
                                ImGuiTableFlags_SizingStretchProp))
                            {
                                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.20f);
                                ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch, 0.20f);
                                ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthStretch, 0.20f);
                                ImGui::TableSetupColumn("GUID", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                                ImGui::TableSetupScrollFreeze(0, 1);
                                ImGui::TableHeadersRow();
                                
                                ImGuiListClipper Clipper;
                                Clipper.Begin((int)Objects.size());
                                
                                while (Clipper.Step())
                                {
                                    for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; i++)
                                    {
                                        CObject* Object = Objects[i];
                                        ImGui::TableNextRow();
                                        
                                        bool bIsRowSelected = (SelectedObjectIndex == i);
                                        
                                        // Name column
                                        ImGui::TableSetColumnIndex(0);
                                        ImGuiSelectableFlags Flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;
                                        
                                        if (ImGui::Selectable(Object->GetName().ToString().c_str(), bIsRowSelected, Flags))
                                        {
                                            SelectedObjectIndex = i;
                                            
                                            if (ImGui::IsMouseDoubleClicked(0))
                                            {
                                                // Double-click action: copy to clipboard or jump to object
                                                ImGui::SetClipboardText(Object->GetName().ToString().c_str());
                                            }
                                        }
                                        
                                        // Context menu
                                        if (ImGui::BeginPopupContextItem())
                                        {
                                            if (ImGui::MenuItem("Copy Name"))
                                            {
                                                ImGui::SetClipboardText(Object->GetName().ToString().c_str());
                                            }
                                            if (ImGui::MenuItem("Copy GUID"))
                                            {
                                                ImGui::SetClipboardText(Object->GetGUID().ToString().c_str());
                                            }
                                            ImGui::EndPopup();
                                        }
                                        
                                        // Class column
                                        ImGui::TableSetColumnIndex(1);
                                        if (Object->GetClass())
                                        {
                                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.9f, 0.6f, 1.0f));
                                            ImGui::TextUnformatted(Object->GetClass()->GetName().ToString().c_str());
                                            ImGui::PopStyleColor();
                                        }
                                        else
                                        {
                                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                            ImGui::TextUnformatted("None");
                                            ImGui::PopStyleColor();
                                        }
                                        
                                        // Flags column
                                        ImGui::TableSetColumnIndex(2);
                                        FFixedString FlagsStr = ObjectFlagsToString(Object->GetFlags());
                                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.4f, 1.0f));
                                        ImGui::TextUnformatted(FlagsStr.c_str());
                                        ImGui::PopStyleColor();
                                        
                                        // Address column
                                        ImGui::TableSetColumnIndex(3);
                                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                                        ImGui::TextUnformatted(Object->GetGUID().ToString().c_str());
                                        ImGui::PopStyleColor();
                                    }
                                }
                                
                                ImGui::EndTable();
                            }
                        }
                    }
                    else
                    {
                        // No package selected
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                        ImVec2 TextSize = ImGui::CalcTextSize("Select a package to view objects");
                        ImVec2 WindowSize = ImGui::GetWindowSize();
                        ImGui::SetCursorPos(ImVec2((WindowSize.x - TextSize.x) * 0.5f, (WindowSize.y - TextSize.y) * 0.5f));
                        ImGui::Text("Select a package to view objects");
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::EndChild();
            }
            ImGui::EndChild();
        }
        
        ImGui::End();
        
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(3);
    }

    void FEditorUI::DrawMemoryDialog()
    {
        ModalManager.CreateDialogue("Memory Profiler", ImVec2(1000, 900), [this]() -> bool
        {
            // Static state for persistent tracking
            struct MemorySnapshot
            {
                double timestamp;
                size_t processMemory;
                size_t currentMapped;
                size_t cachedMemory;
                size_t hugeAllocs;
            };
            
            static TVector<MemorySnapshot> history;
            static float updateTimer = 0.0f;
            static constexpr int maxHistoryPoints = 60;
            static bool isPaused = false;
            
            // Update history
            updateTimer += GEngine->GetDeltaTime();
            if (!isPaused && updateTimer >= 1.0f)
            {
                updateTimer = 0.0f;
                
                MemorySnapshot snapshot;
                snapshot.timestamp = ImGui::GetTime();
                snapshot.processMemory = Platform::GetProcessMemoryUsageBytes();
                snapshot.currentMapped = Memory::GetCurrentMappedMemory();
                snapshot.cachedMemory = Memory::GetCachedMemory();
                snapshot.hugeAllocs = Memory::GetCurrentHugeAllocMemory();
                
                history.push_back(snapshot);
                
                if (history.size() > maxHistoryPoints)
                {
                    history.erase(history.begin());
                }
            }
            
            // Header
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.5f, 1.0f));
            ImGui::Text(LE_ICON_CHART_LINE " Memory Profiler");
            ImGui::PopStyleColor();
            
            ImGui::SameLine(ImGui::GetWindowWidth() - 330);
            if (ImGui::Button(isPaused ? LE_ICON_PLAY " Resume" : LE_ICON_PAUSE " Pause", ImVec2(100, 0)))
            {
                isPaused = !isPaused;
            }
            
            ImGui::SameLine();
            if (ImGui::Button(LE_ICON_TRASH_CAN " Clear", ImVec2(100, 0)))
            {
                history.clear();
            }
            
            ImGui::Separator();
            ImGui::Spacing();
            
            // View tabs
            if (ImGui::BeginTabBar("MemoryTabs"))
            {
                // Overview Tab
                if (ImGui::BeginTabItem(LE_ICON_VIEW_DASHBOARD " Overview"))
                {
                    // Current stats cards
                    ImGui::BeginGroup();
                    {
                        size_t processMemory = Platform::GetProcessMemoryUsageBytes();
                        size_t currentMapped = Memory::GetCurrentMappedMemory();
                        size_t peakMapped = Memory::GetPeakMappedMemory();
                        size_t cachedMemory = Memory::GetCachedMemory();
                        
                        float cardWidth = (ImGui::GetContentRegionAvail().x - 30) / 4.0f;
                        
                        // Card 1: Process Memory
                        ImGui::BeginChild("Card1", ImVec2(cardWidth, 100), true, ImGuiWindowFlags_NoScrollbar);
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
                            ImGui::Text(LE_ICON_MEMORY " Process Memory");
                            ImGui::PopStyleColor();
                            ImGui::Spacing();
                            ImGui::Text("%s", ImGuiX::FormatSize(processMemory).c_str());
                        }
                        ImGui::EndChild();
                        
                        ImGui::SameLine();
                        
                        // Card 2: Mapped Memory
                        ImGui::BeginChild("Card2", ImVec2(cardWidth, 100), true, ImGuiWindowFlags_NoScrollbar);
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.6f, 1.0f));
                            ImGui::Text(LE_ICON_DATABASE " Mapped");
                            ImGui::PopStyleColor();
                            ImGui::Spacing();
                            ImGui::Text("%s", ImGuiX::FormatSize(currentMapped).c_str());
                            float usage = peakMapped > 0 ? (float)currentMapped / peakMapped : 0.0f;
                            ImGui::ProgressBar(usage, ImVec2(-1, 0), "");
                        }
                        ImGui::EndChild();
                        
                        ImGui::SameLine();
                        
                        // Card 3: Cached Memory
                        ImGui::BeginChild("Card3", ImVec2(cardWidth, 100), true, ImGuiWindowFlags_NoScrollbar);
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
                            ImGui::Text(LE_ICON_LAYERS " Cached");
                            ImGui::PopStyleColor();
                            ImGui::Spacing();
                            ImGui::Text("%s", ImGuiX::FormatSize(cachedMemory).c_str());
                        }
                        ImGui::EndChild();
                        
                        ImGui::SameLine();
                        
                        // Card 4: Huge Allocs
                        ImGui::BeginChild("Card4", ImVec2(cardWidth, 100), true, ImGuiWindowFlags_NoScrollbar);
                        {
                            size_t hugeAllocs = Memory::GetCurrentHugeAllocMemory();
                            size_t peakHuge = Memory::GetPeakHugeAllocMemory();
                            
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                            ImGui::Text(LE_ICON_CUBE " Huge Allocs");
                            ImGui::PopStyleColor();
                            ImGui::Spacing();
                            ImGui::Text("%s", ImGuiX::FormatSize(hugeAllocs).c_str());
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Peak: %s", ImGuiX::FormatSize(peakHuge).c_str());
                        }
                        ImGui::EndChild();
                    }
                    ImGui::EndGroup();
                    
                    ImGui::Spacing();
                    
                    // Main graph
                    if (history.size() > 1 && ImPlot::BeginPlot("Memory Usage Over Time", ImVec2(-1, 450)))
                    {
                        ImPlot::SetupAxes("Time (seconds)", "Memory (bytes)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                        ImPlot::SetupLegend(ImPlotLocation_NorthWest);
                        
                        // Prepare data
                        TVector<double> times;
                        TVector<double> processMem, mappedMem, cachedMem, hugeMem;
                        
                        double baseTime = history[0].timestamp;
                        for (const auto& snap : history)
                        {
                            times.push_back(snap.timestamp - baseTime);
                            processMem.push_back((double)snap.processMemory);
                            mappedMem.push_back((double)snap.currentMapped);
                            cachedMem.push_back((double)snap.cachedMemory);
                            hugeMem.push_back((double)snap.hugeAllocs);
                        }
                        
                        ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), 2.0f);
                        ImPlot::PlotLine("Process Memory", times.data(), processMem.data(), (int)times.size());
                        
                        ImPlot::SetNextLineStyle(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), 2.0f);
                        ImPlot::PlotLine("Mapped Memory", times.data(), mappedMem.data(), (int)times.size());
                        
                        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), 1.5f);
                        ImPlot::PlotLine("Cached Memory", times.data(), cachedMem.data(), (int)times.size());
                        
                        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), 1.5f);
                        ImPlot::PlotLine("Huge Allocs", times.data(), hugeMem.data(), (int)times.size());
                        
                        ImPlot::EndPlot();
                    }
                    else if (history.size() <= 1)
                    {
                        ImGui::BeginChild("NoData", ImVec2(-1, 450), true);
                        ImGui::SetCursorPosY(200);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                        float textWidth = ImGui::CalcTextSize("Collecting data...").x;
                        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) / 2);
                        ImGui::Text("Collecting data...");
                        ImGui::PopStyleColor();
                        ImGui::EndChild();
                    }
                    
                    ImGui::EndTabItem();
                }
                
                // Detailed Statistics Tab
                if (ImGui::BeginTabItem(LE_ICON_LIST_BOX " Detailed Stats"))
                {
                    ImGui::BeginChild("DetailedStats", ImVec2(0, -40), false);
                    {
                        if (ImGui::BeginTable("MemoryStatsDetailed", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
                        {
                            ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Current Value", ImGuiTableColumnFlags_WidthFixed, 150);
                            ImGui::TableSetupColumn("Peak Value", ImGuiTableColumnFlags_WidthFixed, 150);
                            ImGui::TableHeadersRow();
                            
                            auto DetailRow = [](const char* label, size_t current, size_t peak = 0, bool showPeak = true)
                            {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::Text("%s", label);
                                
                                ImGui::TableSetColumnIndex(1);
                                ImGui::Text("%s", ImGuiX::FormatSize(current).c_str());
                                
                                ImGui::TableSetColumnIndex(2);
                                if (showPeak)
                                {
                                    ImGui::Text("%s", ImGuiX::FormatSize(peak).c_str());
                                    if (peak > 0)
                                    {
                                        float usage = (float)current / peak * 100.0f;
                                        ImGui::SameLine();
                                        ImGui::TextColored(
                                            usage > 90 ? ImVec4(1, 0.3f, 0.3f, 1) : ImVec4(0.5f, 0.8f, 0.5f, 1),
                                            "(%.1f%%)", usage
                                        );
                                    }
                                }
                                else
                                {
                                    ImGui::TextDisabled("-");
                                }
                            };
                            
                            DetailRow("Process Memory", Platform::GetProcessMemoryUsageBytes(), 0, false);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Separator();
                            
                            DetailRow("Current Mapped", Memory::GetCurrentMappedMemory(), Memory::GetPeakMappedMemory());
                            DetailRow("Cached (Small/Medium)", Memory::GetCachedMemory(), 0, false);
                            DetailRow("Current Huge Allocs", Memory::GetCurrentHugeAllocMemory(), Memory::GetPeakHugeAllocMemory());
                            
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Separator();
                            
                            DetailRow("Total Mapped", Memory::GetTotalMappedMemory(), 0, false);
                            DetailRow("Total Unmapped", Memory::GetTotalUnmappedMemory(), 0, false);
                            
                            ImGui::EndTable();
                        }
                        
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        
                        // Memory efficiency metrics
                        ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), LE_ICON_CHART_BAR " Memory Efficiency");
                        ImGui::Spacing();
                        
                        size_t totalMapped = Memory::GetTotalMappedMemory();
                        size_t totalUnmapped = Memory::GetTotalUnmappedMemory();
                        size_t totalOps = totalMapped + totalUnmapped;
                        
                        if (totalOps > 0)
                        {
                            float mappedRatio = (float)totalMapped / totalOps;
                            float unmappedRatio = (float)totalUnmapped / totalOps;
                            
                            ImGui::Text("Allocation Efficiency:");
                            ImGui::SameLine(200);
                            ImGui::ProgressBar(mappedRatio, ImVec2(300, 0), FString().sprintf("Mapped: %.1f%%", mappedRatio * 100).c_str());
                            
                            ImGui::Text("Deallocation Activity:");
                            ImGui::SameLine(200);
                            ImGui::ProgressBar(unmappedRatio, ImVec2(300, 0), FString().sprintf("Unmapped: %.1f%%", unmappedRatio * 100).c_str());
                        }
                        
                        ImGui::Spacing();
                        ImGui::TextDisabled("Note: All values reported by rpmalloc_global_statistics");
                    }
                    ImGui::EndChild();
                    
                    ImGui::EndTabItem();
                }
                
                // Memory Distribution Tab
                if (ImGui::BeginTabItem(LE_ICON_CHART_PIE " Distribution"))
                {
                    size_t processMemory = Platform::GetProcessMemoryUsageBytes();
                    size_t currentMapped = Memory::GetCurrentMappedMemory();
                    size_t cachedMemory = Memory::GetCachedMemory();
                    size_t hugeAllocs = Memory::GetCurrentHugeAllocMemory();
                    
                    // Pie chart
                    if (ImPlot::BeginPlot("Memory Distribution", ImVec2(-1, 350), ImPlotFlags_Equal))
                    {
                        const char* labels[] = { "Mapped", "Cached", "Huge Allocs", "Other" };
                        size_t other = processMemory > (currentMapped + cachedMemory + hugeAllocs) ?
                            processMemory - (currentMapped + cachedMemory + hugeAllocs) : 0;
                        double data[] = { 
                            (double)currentMapped, 
                            (double)cachedMemory, 
                            (double)hugeAllocs,
                            (double)other
                        };
                        
                        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
                        ImPlot::SetupAxesLimits(-1, 1, -1, 1);
                        
                        ImPlot::PlotPieChart(labels, data, 4, 0.5, 0.5, 0.4, "%.1f%%", 90);
                        
                        ImPlot::EndPlot();
                    }
                    
                    ImGui::Spacing();
                    
                    // Bar chart comparison
                    if (ImPlot::BeginPlot("Memory Comparison", ImVec2(-1, 250)))
                    {
                        const char* categories[] = { "Mapped", "Peak Mapped", "Cached", "Huge", "Peak Huge" };
                        double values[] = {
                            (double)currentMapped,
                            (double)Memory::GetPeakMappedMemory(),
                            (double)cachedMemory,
                            (double)hugeAllocs,
                            (double)Memory::GetPeakHugeAllocMemory()
                        };
                        
                        ImPlot::SetupAxes("Category", "Bytes", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                        
                        ImPlot::PlotBars("Memory", values, 5, 0.67, 0);
                        
                        ImPlot::EndPlot();
                    }
                    
                    ImGui::EndTabItem();
                }
                
                ImGui::EndTabBar();
            }
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            // Footer buttons
            if (ImGui::Button("Close", ImVec2(120, 0)))
            {
                return true;
            }
            
            ImGui::SameLine();
            if (ImGui::Button("Export Data", ImVec2(120, 0)))
            {
                // Export history to CSV or similar
            }
            
            return false;
        }, false);
    }

    void FEditorUI::HandleUserInput(const FUpdateContext& UpdateContext)
    {
        
    }

    void FEditorUI::VerifyDirtyPackages()
    {
        TVector<CPackage*> DirtyPackages;
        DirtyPackages.reserve(4);
        for (TObjectIterator<CPackage> Itr; Itr; ++Itr)
        {
            CPackage* Package = *Itr;

            if (Package->IsDirty())
            {
                DirtyPackages.push_back(Package);
            }
        }

        if (DirtyPackages.empty())
        {
            return;
        }
        
        TVector<bool> PackageSelection;
        PackageSelection.resize(DirtyPackages.size(), true);
        
        enum class ESaveState { Idle, Saving, Success, Failed };
        TVector<ESaveState> SaveStates;
        SaveStates.resize(DirtyPackages.size(), ESaveState::Idle);
        
        ModalManager.CreateDialogue("Save Modified Packages", ImVec2(450, 600), [&, Packages = Move(DirtyPackages), PackageSelection, SaveStates] () mutable
        {
            bool bShouldClose = false;
            
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 12));
            
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), LE_ICON_EXCLAMATION_THICK " Unsaved Changes Detected");
            
            ImGui::Spacing();
            ImGui::TextWrapped("The following packages have unsaved changes. Select which packages you would like to save before continuing.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            ImGui::BeginGroup();
            {
                if (ImGui::Button(LE_ICON_SELECT_ALL " Select All", ImVec2(140, 0)))
                {
                    for (bool& Selected : PackageSelection)
                    {
                        Selected = true;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(LE_ICON_SQUARE_OUTLINE " Deselect All", ImVec2(140, 0)))
                {
                    for (bool& Selected : PackageSelection)
                    {
                        Selected = false;
                    }
                }
                
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                
                int32 SelectedCount = 0;
                for (bool Selected : PackageSelection)
                {
                    if (Selected)
                    {
                        SelectedCount++;
                    }
                }

                ImGui::Text("%d of %d selected", SelectedCount, (int32)Packages.size());
            }
            ImGui::EndGroup();
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            ImVec2 ListSize = ImVec2(-1, -80);
            if (ImGui::BeginChild("PackageList", ListSize, true, ImGuiWindowFlags_AlwaysVerticalScrollbar))
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
                
                for (size_t i = 0; i < Packages.size(); ++i)
                {
                    CPackage* Package = Packages[i];
                    
                    ImGui::PushID((int)i);
                    
                    ImVec2 ItemStart = ImGui::GetCursorScreenPos();
                    ImVec2 ItemSize = ImVec2(ImGui::GetContentRegionAvail().x, 64);
                    
                    bool bIsHovered = ImGui::IsMouseHoveringRect(ItemStart, ImVec2(ItemStart.x + ItemSize.x, ItemStart.y + ItemSize.y));
                    
                    ImU32 BgColor = bIsHovered ? 
                        IM_COL32(50, 50, 55, 180) : 
                        IM_COL32(35, 35, 40, 180);
                    
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        ItemStart, 
                        ImVec2(ItemStart.x + ItemSize.x, ItemStart.y + ItemSize.y),
                        BgColor,
                        4.0f
                    );
                    
                    ImGui::BeginGroup();
                    {
                        ImGui::Dummy(ImVec2(0, 4));
                        
                        ImGui::Checkbox("##select", &PackageSelection[i]);
                        ImGui::SameLine();
                        
                        ImGui::BeginGroup();
                        {
                            ImGui::Text(LE_ICON_FILE " %s", Package->GetName().c_str());
                            
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", Package->GetPackagePath().c_str());
                            
                            switch (SaveStates[i])
                            {
                                case ESaveState::Saving:
                                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), LE_ICON_WATCH_VIBRATE " Saving...");
                                    break;
                                case ESaveState::Success:
                                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), LE_ICON_CHECK " Saved");
                                    break;
                                case ESaveState::Failed:
                                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), LE_ICON_EXCLAMATION_THICK " Failed to save");
                                    break;
                            }
                        }
                        ImGui::EndGroup();
                        
                        ImGui::Dummy(ImVec2(0, 4));
                    }
                    ImGui::EndGroup();
                    
                    ImGui::PopID();
                    
                    ImGui::Spacing();
                }
                
                ImGui::PopStyleVar();
            }
            ImGui::EndChild();
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            ImGui::BeginGroup();
            {
                float ButtonWidth = 150.0f;
                float Spacing = 8.0f;
                float TotalWidth = (ButtonWidth * 2) + (Spacing * 2);
                float OffsetX = (ImGui::GetContentRegionAvail().x - TotalWidth) * 0.5f;
                
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + OffsetX);
                
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.5f, 0.15f, 1.0f));
                
                if (ImGui::Button(LE_ICON_CONTENT_SAVE " Save Selected", ImVec2(ButtonWidth, 35)))
                {
                    for (size_t i = 0; i < Packages.size(); ++i)
                    {
                        if (PackageSelection[i])
                        {
                            SaveStates[i] = ESaveState::Saving;
                            
                            bool bSaveSuccess = CPackage::SavePackage(Packages[i], Packages[i]->GetPackagePath());
                            SaveStates[i] = bSaveSuccess ? ESaveState::Success : ESaveState::Failed;
                        }
                    }
                    
                    bShouldClose = true;
                }
                ImGui::PopStyleColor(3);
                
                ImGui::SameLine();
                
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.4f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.5f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.3f, 0.15f, 1.0f));
                
                if (ImGui::Button(LE_ICON_SQUARE " Don't Save", ImVec2(ButtonWidth, 35)))
                {
                    bShouldClose = true;
                }
                ImGui::PopStyleColor(3);
                
                //ImGui::SameLine();
                //
                //if (ImGui::Button(LE_ICON_CANCEL " Cancel", ImVec2(ButtonWidth, 35)))
                //{
                //    
                //    bShouldClose = true;
                //}
            }
            ImGui::EndGroup();
            
            ImGui::PopStyleVar();
            
            return bShouldClose;
        });
    }
    
    void FEditorUI::DrawTitleBarMenu(const FUpdateContext& UpdateContext)
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
        ImGui::Image(ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/Lumina.png"), ImVec2(24.0f, 24.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
    
        // Styled menu bar
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.08f, 0.08f, 0.1f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.2f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.25f, 0.25f, 0.27f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.92f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.35f, 0.45f, 0.8f));
    
        ImGui::SetNextWindowSizeConstraints(ImVec2(220, 1), ImVec2(280, 1000));
    
        DrawFileMenu();
        DrawProjectMenu();
        DrawToolsMenu();
        DrawHelpMenu();
    
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);

    }
    
    void FEditorUI::DrawTitleBarInfoStats(const FUpdateContext& UpdateContext)
    {
        ImGui::SameLine();

        float CurrentFPS = UpdateContext.GetFPS();
        float CurrentFrameTime = UpdateContext.GetDeltaTime() * 1000.0f;
        
        SmoothedFPS = SmoothedFPS + (CurrentFPS - SmoothedFPS) * FPSSmoothingFactor;
        SmoothedFrameTime = SmoothedFrameTime + (CurrentFrameTime - SmoothedFrameTime) * FPSSmoothingFactor;

        const TFixedString<100> PerfStats(TFixedString<100>::CtorSprintf(), "FPS: %3.0f / %.2f ms", SmoothedFPS, SmoothedFrameTime);
        ImGui::TextUnformatted(PerfStats.c_str());

        ImGui::SameLine();

        const TFixedString<100> ObjectStats(TFixedString<100>::CtorSprintf(), "CObjects: %i", GObjectArray.GetNumAliveObjects());
        ImGui::TextUnformatted(ObjectStats.c_str());
    }

    void FEditorUI::DrawFileMenu()
    {
        if (!ImGui::BeginMenu(LE_ICON_FILE " File"))
        {
            return;
        }
        if (ImGui::MenuItem(LE_ICON_ZIP_DISK " Save", "Ctrl+S"))
        {
            // Save action
        }

        if (ImGui::MenuItem(LE_ICON_ZIP_DISK " Save All", "Ctrl+Shift+S"))
        {
            // Save all action
        }

        ImGui::Separator();

        if (ImGui::MenuItem(LE_ICON_SETTINGS_HELPER " Config Settings"))
        {
            ConfigSettingsDialog();
        }
        
        if (ImGui::BeginMenu(LE_ICON_ROTATE_LEFT " Recent"))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.62f, 1.0f));
         
            auto Recents = GConfig->Get<std::vector<std::string>>("Editor.RecentProjects");
            for (const auto& Item : Recents)
            {
                ImGui::TextUnformatted(Item.c_str());
            }
            ImGui::PopStyleColor();
            
            ImGui::EndMenu();
        }

        ImGui::Separator();
        

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.6f, 0.4f, 1.0f));
        if (ImGui::BeginMenu(LE_ICON_HAMMER " Shaders"))
        {
            if (ImGui::MenuItem(LE_ICON_HAMMER " Recompile All", "F5"))
            {
                GRenderContext->CompileEngineShaders();
                CMaterial::CreateDefaultMaterial();
            }
            
            if (ImGui::MenuItem(LE_ICON_MATERIAL_DESIGN " Recompile Default Material"))
            {
                CMaterial::CreateDefaultMaterial();
            }

            if (ImGui::MenuItem(LE_ICON_FOLDER " Open Shaders Directory", "F6"))
            {
                Platform::LaunchURL(StringUtils::ToWideString(Paths::GetEngineShadersDirectory()).c_str());
            }

            ImGui::Separator();

            for (auto& Directory : std::filesystem::recursive_directory_iterator(Paths::GetEngineShadersDirectory().c_str()))
            {
                if (Directory.is_regular_file())
                {
                    FString FileName = Directory.path().filename().string().c_str();
                    if (ImGui::BeginMenu(FileName.c_str()))
                    {
                        if (ImGui::MenuItem(LE_ICON_HAMMER " Recompile"))
                        {
                            GRenderContext->GetShaderCompiler()->CompileShaderPath(Directory.path().string().c_str(), {}, [&](const FShaderHeader& Header)
                            {
                                GRenderContext->GetShaderLibrary()->CreateAndAddShader(Header.DebugName, Header, true);
                            });
                        }

                        if (ImGui::MenuItem(LE_ICON_FOLDER " Open"))
                        {
                            Platform::LaunchURL(Directory.path().c_str());
                        }

                        ImGui::EndMenu();
                    }
                }
            }

            ImGui::EndMenu();
        }
        ImGui::PopStyleColor();

        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.4f, 0.4f, 1.0f));
        if (ImGui::MenuItem(LE_ICON_DOOR_OPEN " Exit", "Alt+F4"))
        {
            // ...
			FApplication::RequestExit();
        }
        ImGui::PopStyleColor();

        ImGui::EndMenu();
    }

    void FEditorUI::DrawProjectMenu()
    {
        if (!ImGui::BeginMenu(LE_ICON_FOLDER " Project"))
        {
            return;
        }
    
        if (ImGui::MenuItem(LE_ICON_FOLDER_PLUS " New Project...", "Ctrl+N"))
        {
            OpenProjectManagerDialog(EProjectManagerTab::NewProject);
        }
        if (ImGui::MenuItem(LE_ICON_FOLDER_OPEN " Open Project...", "Ctrl+"))
        {
            OpenProjectManagerDialog(EProjectManagerTab::OpenProject);
        }
    
        ImGui::Separator();
    
        if (ImGui::MenuItem(LE_ICON_DATABASE " Asset Registry"))
        {
            AssetRegistryDialog();
        }
    
        ImGui::Separator();
    
        if (ImGui::BeginMenu(LE_ICON_HAMMER " Build"))
        {
            if (ImGui::MenuItem(LE_ICON_PLAY " Build Project", "Ctrl+B"))
            {
                // Build project
            }
        
            if (ImGui::MenuItem(LE_ICON_BROOM " Clean Build", "Ctrl+Shift+B"))
            {
                // Clean build
            }
        
            ImGui::Separator();
        
            if (ImGui::MenuItem(LE_ICON_MICROSOFT_WINDOWS " Package for Windows"))
            {
                // Package
            }
        
            ImGui::EndMenu();
        }
    
        ImGui::EndMenu();
    }

    void FEditorUI::DrawToolsMenu()
    {
        if (!ImGui::BeginMenu(LE_ICON_WRENCH " Tools"))
        {
            return;
        }

        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.62f, 1.0f), "Debug Windows");
        ImGui::Separator();
        
        ImGui::MenuItem(LE_ICON_LANGUAGE_LUA " Scripts Info", nullptr, &bShowScriptsDebug);
        
        ImGui::MenuItem(LE_ICON_CHART_LINE " Renderer Info", nullptr, &bShowRenderDebug);
        
        if (ImGui::MenuItem(LE_ICON_MEMORY " Memory Info", nullptr, nullptr))
        {
            DrawMemoryDialog();
        }
        
        ImGui::MenuItem(LE_ICON_LIST_BOX " CObject List", nullptr, &bShowObjectDebug);
        
        ImGui::Spacing();
        
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.62f, 1.0f), "ImGui Tools");
        ImGui::Separator();
        
        ImGui::MenuItem(LE_ICON_WINDOW_OPEN " ImGui Style Editor", nullptr, &bShowImGuiStyleEditor);
        ImGui::MenuItem(LE_ICON_WINDOW_OPEN " ImGui Demo", nullptr, &bShowDearImGuiDemoWindow);
        ImGui::MenuItem(LE_ICON_CHART_BAR " ImPlot Demo", nullptr, &bShowImPlotDemoWindow);
        
        ImGui::Spacing();
        
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.62f, 1.0f), "External Tools");
        ImGui::Separator();
        
        if (ImGui::MenuItem(LE_ICON_WATCH" Tracy Profiler", "Ctrl+P"))
        {
            FString LuminaDirEnv = std::getenv("LUMINA_DIR");
            FString FullPath = LuminaDirEnv + "/External/Tracy/tracy-profiler.exe";
            
            Platform::LaunchURL(StringUtils::ToWideString(FullPath).c_str());
        }
        
        if (ImGui::MenuItem(LE_ICON_CAMERA " RenderDoc Capture", "F11"))
        {
            FRenderDoc::Get().TriggerCapture();
        }
        
        ImGui::Spacing();
        
        // Settings
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.62f, 1.0f), "Settings");
        ImGui::Separator();
        
        bool bVSyncEnabled = GRenderContext->IsVSyncEnabled();
        if (ImGui::MenuItem(LE_ICON_DISC_PLAYER " V-Sync", nullptr, bVSyncEnabled))
        {
            GRenderContext->SetVSyncEnabled(!bVSyncEnabled);
        }
        
        if (ImGui::BeginMenu(LE_ICON_PALETTE " Theme"))
        {
            if (ImGui::MenuItem("Dark", nullptr, true))  // Currently selected
            {
                // Apply dark theme
            }
            
            if (ImGui::MenuItem("Light", nullptr, false))
            {
                // Apply light theme
            }
            
            if (ImGui::MenuItem("Custom...", nullptr, false))
            {
                // Open theme editor
            }
            
            ImGui::EndMenu();
        }
        
        ImGui::EndMenu();
    }

    void FEditorUI::DrawHelpMenu()
    {
        if (!ImGui::BeginMenu(LE_ICON_HELP " Help"))
        {
            return;
        }

        if (ImGui::MenuItem(LE_ICON_GROUP " Discord"))
        {
            Platform::LaunchURL(TEXT("https://discord.gg/UhTmzB8UdY"));
        }

        if (ImGui::MenuItem(LE_ICON_BOOK " Documentation", "F1"))
        {
            Platform::LaunchURL(TEXT("https://discord.gg/UhTmzB8UdY"));
        }
    
        if (ImGui::MenuItem(LE_ICON_ACCOUNT_QUESTION " Tutorials"))
        {
            Platform::LaunchURL(TEXT("https://discord.gg/UhTmzB8UdY"));
        }
    
        ImGui::Separator();
    
        if (ImGui::MenuItem(LE_ICON_GITHUB " GitHub Repository"))
        {
            Platform::LaunchURL(TEXT("https://github.com/MrDrElliot/LuminaEngine"));
        }
    
        if (ImGui::MenuItem(LE_ICON_BUG " Report Issue"))
        {
            Platform::LaunchURL(TEXT("https://github.com/MrDrElliot/LuminaEngine/issues"));
        }
    
        ImGui::Separator();
        
        if (ImGui::MenuItem(LE_ICON_GROUP " Contributors"))
        {
            bShowContributors = !bShowContributors;
        }
    
        if (ImGui::MenuItem(LE_ICON_CIRCLE " About Lumina"))
        {
            bShowLuminaInfo = !bShowLuminaInfo;
        }
    
        ImGui::EndMenu();
    }

    void FEditorUI::OpenProjectManagerDialog(EProjectManagerTab DefaultTab)
    {
        
        bool bInitTabState = true;

        ModalManager.CreateDialogue("Project Manager", ImVec2(1000, 650), [this, DefaultTab, bInitTabState] () mutable -> bool
        {
            bool bShouldClose = false;

            ImGuiTabItemFlags openTabFlags = (DefaultTab == EProjectManagerTab::OpenProject && bInitTabState)
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;

            ImGuiTabItemFlags newTabFlags = (DefaultTab == EProjectManagerTab::NewProject && bInitTabState)
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            
            if (ImGui::BeginTabBar("ProjectManager"))
            {
               
                if (ImGui::BeginTabItem("Open Project",nullptr, openTabFlags))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
                    ImGui::TextWrapped(LE_ICON_FOLDER_OPEN " Select a project to open or browse for an existing project");
                    ImGui::PopStyleColor();

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::BeginChild("ProjectContent", ImVec2(0, -50), false);
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
                        ImGui::Text(LE_ICON_FOLDER_OPEN " Example Projects");
                        ImGui::PopStyleColor();
                        ImGui::Spacing();

                        ImGui::BeginChild("ProjectCards", ImVec2(0, 0), false);
                        {
                            const float CardWidth = 280.0f;
                            const float CardHeight = 200.0f;
                            const float Padding = 16.0f;

                            float availWidth = ImGui::GetContentRegionAvail().x;
                            int CardsPerRow = Math::Max(1, (int)((availWidth + Padding) / (CardWidth + Padding)));

                            ImGui::BeginGroup();
                            {
                                ImVec2 CursorPos = ImGui::GetCursorScreenPos();
                                ImDrawList* drawList = ImGui::GetWindowDrawList();

                                ImVec4 cardBgColor = ImVec4(0.15f, 0.15f, 0.16f, 1.0f);
                                ImVec4 cardBgHoverColor = ImVec4(0.18f, 0.18f, 0.19f, 1.0f);
                                ImVec4 accentColor = ImVec4(0.3f, 0.6f, 1.0f, 1.0f);

                                ImGui::PushStyleColor(ImGuiCol_Button, cardBgColor);
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, cardBgHoverColor);
                                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.2f, 0.21f, 1.0f));
                                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
                                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18, 18));

                                if (ImGui::Button("##SandboxCard", ImVec2(CardWidth, CardHeight)))
                                {
                                    FString SandboxProjectDirectory = Paths::GetEngineDirectory() + "/Sandbox/Sandbox.lproject";
                                    GEditorEngine->LoadProject(SandboxProjectDirectory);
                                    OnProjectLoaded();
                                    bShouldClose = true;
                                }

                                ImGui::PopStyleVar(2);
                                ImGui::PopStyleColor(3);

                                drawList->AddRectFilled(
                                    CursorPos,
                                    ImVec2(CursorPos.x + CardWidth, CursorPos.y + 4),
                                    ImGui::GetColorU32(accentColor)
                                );

                                ImGui::SetCursorScreenPos(ImVec2(CursorPos.x + 16, CursorPos.y + 20));
                                ImGui::Dummy(ImVec2(0, 0));

                                ImGui::BeginGroup();
                                {
                                    ImVec2 iconPos = ImGui::GetCursorScreenPos();
                                    drawList->AddCircleFilled(
                                        ImVec2(iconPos.x + 20, iconPos.y + 20),
                                        20.0f,
                                        ImGui::GetColorU32(ImVec4(0.3f, 0.6f, 1.0f, 0.2f))
                                    );

                                    ImGui::PushStyleColor(ImGuiCol_Text, accentColor);
                                    ImGui::SetCursorScreenPos(ImVec2(iconPos.x + 10, iconPos.y + 10));
                                    ImGui::Text(LE_ICON_FOLDER_OPEN);
                                    ImGui::PopStyleColor();

                                    ImGui::SetCursorScreenPos(ImVec2(CursorPos.x + 16, iconPos.y + 50));

                                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                                    ImGui::Text("Sandbox Project");
                                    ImGui::PopStyleColor();

                                    ImGui::Spacing();

                                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                                    ImGui::BeginChild("##SandboxDesc", ImVec2(CardWidth - 32, 60), false, ImGuiWindowFlags_NoScrollbar);
                                    ImGui::TextWrapped("A basic sandbox environment for testing and experimentation. Perfect for learning the engine basics.");
                                    ImGui::EndChild();
                                    ImGui::PopStyleColor();

                                    ImGui::SetCursorScreenPos(ImVec2(CursorPos.x + 16, CursorPos.y + CardHeight - 30));

                                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 0.5f, 1.0f));
                                    ImGui::Text("Example Project");
                                    ImGui::PopStyleColor();

                                    ImGui::EndGroup();
                                }
                            }
                            ImGui::EndGroup();


                            ImGui::EndChild();
                        }

                        ImGui::EndChild();
                    }

                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::BeginGroup();
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.5f, 0.9f, 1.0f));

                        if (ImGui::Button(LE_ICON_FOLDER_OPEN " Browse for Project...", ImVec2(200, 32)))
                        {
                            FFixedString Project;
                            if (Platform::OpenFileDialogue(
                                Project,
                                "Open Project",
                                "Lumina Project (*.lproject)\0*.lproject\0All Files (*.*)\0*.*\0",
                                nullptr
                            ))
                            {
                                GEditorEngine->LoadProject(Project);
                                OnProjectLoaded();
                                bShouldClose = true;
                            }
                        }

                        ImGui::PopStyleColor(3);

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120);

                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));

                        if (ImGui::Button("Cancel", ImVec2(120, 32)))
                        {
                            bShouldClose = true;
                        }

                        ImGui::PopStyleColor(3);

                        ImGui::EndGroup();
                    }
                    ImGui::EndTabItem();
                }
                if (DefaultTab == EProjectManagerTab::NewProject) {
                    ImGui::SetNextItemOpen(true);
                }
                if (ImGui::BeginTabItem("New Project", nullptr, newTabFlags))
                {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), LE_ICON_FOLDER_PLUS " Create a new Lumina project");
                    ImGui::Separator();
                    ImGui::Spacing();

                    static char NewProjectName[256] = "MyProject";
                    static char NewProjectPath[512] = "";

                    ImGui::Text("Project Name:");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputText("##ProjectName", NewProjectName, sizeof(NewProjectName));

                    ImGui::Spacing();

                    ImGui::Text("Project Location:");
                    ImGui::SetNextItemWidth(-120);
                    ImGui::InputText("##ProjectPath", NewProjectPath, sizeof(NewProjectPath));
                    ImGui::SameLine();
                    if (ImGui::Button("Browse...", ImVec2(110, 0)))
                    {
                        FFixedString File;
                        if (Platform::OpenFileDialogue(File, "Browse..."))
                        {
                            strncpy_s(NewProjectPath, sizeof(NewProjectPath), File.c_str(), _TRUNCATE);
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Project Template:");
                    ImGui::BeginChild("Templates", ImVec2(0, -50), true);
                    {
                        if (ImGui::Selectable(LE_ICON_CUBE " Blank Project"))
                        {

                        }
                    }
                    ImGui::EndChild();

                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.3f, 1.0f));  
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.65f, 0.36f, 1.0f)); 
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.48f, 0.26f, 1.0f));

                    if (ImGui::Button(LE_ICON_CHECK " Create Project", ImVec2(200, 32)))
                    {
                        GEditorEngine->CreateProject(NewProjectName, NewProjectPath);
                        ImGui::PopStyleColor();

                        ImGuiX::Notifications::NotifySuccess("Successfully created project, please close the engine and relaunch");
                        return true;
                    }
                    ImGui::PopStyleColor(3);

                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120);

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));

                    if (ImGui::Button("Cancel", ImVec2(120, 32)))
                    {
                        bShouldClose = true;
                    }

                    ImGui::PopStyleColor(3);

                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();

                bInitTabState = false;
            }
            return bShouldClose;
        }, true, false);
    }

    void FEditorUI::ConfigSettingsDialog()
    {
        ModalManager.CreateDialogue("Config Settings", ImVec2(1000, 700), [this] () -> bool
        {
            if (ImGui::BeginTable("##SettingsTable", 2, 
                ImGuiTableFlags_Borders | 
                ImGuiTableFlags_RowBg | 
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, 200.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
            
                TFunction<void(const nlohmann::json&, const FString&, int)> RenderValue;
                RenderValue = [&](const nlohmann::json& V, const FString& Path, int Depth)
                {
                    ImGui::PushID(Path.c_str());
                    
                    if (V.is_string())
                    {
                        FFixedString Buffer(V.get<std::string>().c_str());
                        if (ImGui::InputText("##Val", Buffer.data(), Buffer.max_size()))
                        {
                            GConfig->Set(Path, Buffer.c_str());
                        }
                    }
                    else if (V.is_boolean())
                    {
                        bool BoolValue = V.get<bool>();
                        if (ImGui::Checkbox("##Val", &BoolValue))
                        {
                            GConfig->Set(Path, BoolValue);
                        }
                    }
                    else if (V.is_number_integer())
                    {
                        int IntValue = V.get<int>();
                        if (ImGui::InputInt("##Val", &IntValue))
                        {
                            GConfig->Set(Path, IntValue);
                        }
                    }
                    else if (V.is_number_float())
                    {
                        float FloatValue = V.get<float>();
                        if (ImGui::InputFloat("##Val", &FloatValue))
                        {
                            GConfig->Set(Path, FloatValue);
                        }
                    }
                    else if (V.is_array())
                    {
                        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8, 4));
                        
                        FString TableID = std::format("##{}_nested_{}", Path, Depth).c_str();
                        if (ImGui::BeginTable(TableID.c_str(), 2,
                            ImGuiTableFlags_Borders |
                            ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable))
                        {
                            ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableHeadersRow();
    
                            for (size_t i = 0; i < V.size(); ++i)
                            {
                                const nlohmann::json& ArrayValue = V[i];

                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
            
                                ImGui::Text("[%zu]", i);

                                ImGui::TableNextColumn();
                                FString ArrayPath = std::format("{}[{}]", Path, i).c_str();
                                RenderValue(ArrayValue, ArrayPath, Depth + 1);
                            }

    
                            ImGui::EndTable();
                        }
                        
                        ImGui::PopStyleVar();
                    }
                    else if (V.is_object())
                    {
                        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8, 4));
                        
                        FString TableID = std::format("##{}_nested_{}", Path, Depth).c_str();
                        if (ImGui::BeginTable(TableID.c_str(), 2,
                            ImGuiTableFlags_Borders |
                            ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable))
                        {
                            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 200.0f);
                            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableHeadersRow();
    
                            for (auto It = V.begin(); It != V.end(); ++It)
                            {
                                FString NestedKey = It.key().c_str();
                                const nlohmann::json& NestedValue = It.value();
    
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(NestedKey.c_str());
    
                                ImGui::TableNextColumn();
                                FString NestedPath = std::format("{}.{}", Path, NestedKey).c_str();
                                RenderValue(NestedValue, NestedPath, Depth + 1);
                            }
    
                            ImGui::EndTable();
                        }
                        
                        ImGui::PopStyleVar();
                    }
                    else
                    {
                        ImGui::TextDisabled("Unknown type");
                    }
                    
                    ImGui::PopID();
                };
                
                GConfig->ForEach([&](const FString& Key, const nlohmann::json& Value)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(Key.c_str());
                    
                    ImGui::TableNextColumn();
                    RenderValue(Value, Key, 0);
                });
            
                ImGui::EndTable();
            }
            
            if (ImGui::Button("Close", ImVec2(120, 0)))
            {
                return true;
            }
            
            return false;
        }, false);
    }

    void FEditorUI::OnProjectLoaded()
    {
        ContentBrowser->RefreshContentBrowser();
        
        //@TODO TEMP, maybe just wait until finished to load startup.
        GTaskSystem->WaitForAll();
        
        FString EditorStartupMap = GConfig->Get<std::string>("Project.EditorStartupMap").c_str();
        if (FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(EditorStartupMap))
        {
            OpenAssetEditor(Data->AssetGUID);
        }
        
        auto Recents = GConfig->Get<std::vector<std::string>>("Editor.RecentProjects");
        bool bDoesNotContains = eastl::none_of(Recents.begin(), Recents.end(), [&](const std::string& Item)
        {
            return Item == std::string(GEngine->GetProjectName().data());
        });
        
        if (bDoesNotContains)
        {
            Recents.emplace_back(GEngine->GetProjectName().data());
            GConfig->Set("Editor.RecentProjects", Recents);
        }
    }

    void FEditorUI::AssetRegistryDialog()
    {
        struct FAssetDialogueState
        {
            FAssetData* SelectedData = nullptr;
        };
        
        auto DialogueState = MakeUnique<FAssetDialogueState>();
        
        ModalManager.CreateDialogue("Asset Registry", ImVec2(1000, 700), [DialogueState = Move(DialogueState)] () -> bool
        {
            ImGui::BeginChild("SettingsCategories", ImVec2(200, 0), true);
            {
                ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "Assets");
                ImGui::Separator();
                ImGui::Spacing();
                
                const FAssetDataMap& Assets = FAssetRegistry::Get().GetAssets();
                
                if (ImGui::BeginTable("##AssetList", 1, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY))
                {
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableHeadersRow();
    
                    for (const TUniquePtr<FAssetData>& Asset : Assets)
                    {
                        ImGui::TableNextRow();
                        
                        ImGui::TableNextColumn();
                        bool bIsSelected = (DialogueState->SelectedData == Asset.get());
                        if (ImGui::Selectable(Asset->AssetName.c_str(), bIsSelected))
                        {
                            DialogueState->SelectedData = Asset.get();
                        }
                    }
                    
                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();
            
            ImGui::SameLine();
            
            ImGui::BeginChild("SettingsContent", ImVec2(0, -40), true);
            {
                if (DialogueState->SelectedData)
                {
                    FAssetData* Asset = DialogueState->SelectedData;
                    
                    ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "Asset Details");
                    ImGui::Separator();
                    ImGui::Spacing();
                    
                    ImGui::Columns(2, nullptr, false);
                    ImGui::SetColumnWidth(0, 100);
                    
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Name:");
                    ImGui::NextColumn();
                    ImGui::TextUnformatted(Asset->AssetName.c_str());
                    ImGui::NextColumn();
                    ImGui::Spacing();
                    
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Class:");
                    ImGui::NextColumn();
                    ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "%s", Asset->AssetClass.c_str());
                    ImGui::NextColumn();
                    ImGui::Spacing();
                    
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Path:");
                    ImGui::NextColumn();
                    ImGui::TextWrapped("%s", Asset->Path.c_str());
                    ImGui::NextColumn();
                    ImGui::Spacing();
                    
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "GUID:");
                    ImGui::NextColumn();
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", Asset->AssetGUID.ToString().c_str());
                    ImGui::NextColumn();
                    ImGui::Spacing();

                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Size On Disk:");
                    ImGui::NextColumn();
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", ImGuiX::FormatSize(VFS::Size(Asset->Path)).c_str());
                    ImGui::NextColumn();
                    
                    ImGui::Columns(1);
                }
                else
                {
                    ImGui::TextDisabled("Select an asset to view details");
                }
            }
            ImGui::EndChild();
            
            ImGui::Spacing();
            
            if (ImGui::Button("Close", ImVec2(120, 0)))
            {
                return true;
            }
            
            return false;
        }, false);
    }
    
}
