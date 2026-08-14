#include "EditorUI.h"
#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_internal.h>
#include <Lumina.h>
#include <string.h>
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
#include "Core/Math/Math.h"
#include <GUID/GUID.h>
#include <Memory/SmartPtr.h>
#include <Paths/Paths.h>
#include <Platform/GenericPlatform.h>
#include <Renderer/Shader.h>
#include <TaskSystem/ThreadedCallback.h>
#include <Tools/Screenshot/ScreenshotCapture.h>
#include <World/World.h>
#include "UI/Tools/NodeGraph/Material/MaterialGraphCompile.h"
#include "Config/EngineSettings.h"
#include "Input/Key.h"
#include "implot.h"
#include "LuminaEditor.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Assets/AssetTypes/Blackboard/Blackboard.h"
#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Assets/AssetTypes/DataAsset/DataAsset.h"
#include "Assets/AssetTypes/Font/Font.h"
#include "Assets/AssetTypes/PhysicsMaterial/PhysicsMaterial.h"
#include "Assets/AssetTypes/GeometryCollection/GeometryCollection.h"
#include "Assets/AssetEvents.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/MaterialFunction/MaterialFunction.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Config/Config.h"
#include "Core/Application/Application.h"
#include "Core/Module/ModuleManager.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/Profile.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "EASTL/sort.h"
#include "Input/InputProcessor.h"
#include "Input/InputViewport.h"
#include "Memory/Memory.h"
#include "Platform/CrashReporter.h"
#include "Platform/Process/PlatformProcess.h"
#include "Properties/Customizations/CoreTypeCustomization.h"
#include "Properties/Customizations/EntityScriptComponentCustomization.h"
#include "Properties/Customizations/CurveGradientCustomization.h"
#include "Properties/Customizations/CustomPrimitiveDataCustomization.h"
#include "Tools/AssetEditors/ParticleSystemEditor/ParticleParamCustomization.h"
#include "Tools/AssetEditors/ParticleSystemEditor/ParticleParameterCustomization.h"
#include "Properties/Customizations/AssetRefPropertyCustomization.h"
#include "Properties/Customizations/GameplayTagPropertyCustomization.h"
#include "Properties/Customizations/DataTableRowHandleCustomization.h"
#include "GameplayTags/GameplayTag.h"
#include "Scripting/EntityScript.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Renderer/CustomPrimitiveData.h"
#include "Renderer/RenderDocImpl.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHI.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "Thumbnails/ThumbnailManager.h"
#include "Tools/ConsoleLogEditorTool.h"
#include "Tools/ContentBrowserEditorTool.h"
#include "Tools/EditorAssetActions.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/EditorTool.h"
#include "Tools/EditorToolRegistry.h"
#include "Tools/ToolsMenuRegistry.h"
#include "Tools/CPUProfilerEditorTool.h"
#include "Tools/TaskSystemProfilerEditorTool.h"
#include "Tools/GameplayInsightsEditorTool.h"
#include "Tools/PluginBrowserEditorTool.h"
#include "Tools/ShadowAtlasEditorTool.h"
#include "Tools/EditorToolModal.h"
#include "Tools/GamePreviewTool.h"
#include "Tools/ToolFlags.h"
#include "Tools/WorldEditorTool.h"
#include "World/WorldManager.h"
#include "Tools/Debug/AboutEditorTool.h"
#include "Tools/Debug/AssetRegistryEditorTool.h"
#include "Tools/Debug/ConsoleVariableEditorTool.h"
#include "Tools/Debug/MemoryProfilerEditorTool.h"
#include "Tools/Debug/ScriptDiagnosticsEditorTool.h"
#include "Tools/Debug/TextureHeapEditorTool.h"
#include "Tools/Debug/TextureStreamingEditorTool.h"
#include "Tools/Debug/ObjectBrowserEditorTool.h"
#include "Tools/Debug/ProjectPackagerEditorTool.h"
#include "Tools/Debug/SettingsEditorTool.h"
#include "Settings/EditorSettings.h"
#include "Config/EngineSettings.h"
#include "Tools/AssetEditors/Animation/AnimationEditorTool.h"
#include "Tools/AssetEditors/AnimationGraph/AnimationGraphEditorTool.h"
#include "Tools/AssetEditors/Blackboard/BlackboardEditorTool.h"
#include "Tools/AssetEditors/CurveEditor/CurveAssetEditorTool.h"
#include "Tools/AssetEditors/DataAsset/DataAssetEditorTool.h"
#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Tools/AssetEditors/DataTable/DataTableEditorTool.h"
#include "Thumbnails/AssetTilePainters.h"
#include "Tools/EditorEntityUtils.h"
#include "Tools/AssetEditors/AudioStream/AudioStreamEditorTool.h"
#include "Tools/AssetEditors/BlendSpace/BlendSpaceEditorTool.h"
#include "Tools/AssetEditors/CollisionShape/CollisionShapeEditorTool.h"
#include "Tools/AssetEditors/PhysicsAsset/PhysicsAssetEditorTool.h"
#include "Tools/AssetEditors/PhysicsMaterial/PhysicsMaterialEditorTool.h"
#include "Tools/AssetEditors/GeometryCollection/GeometryCollectionEditorTool.h"
#include "Tools/AssetEditors/MaterialEditor/MaterialEditorTool.h"
#include "Tools/AssetEditors/MaterialEditor/MaterialInstanceEditorTool.h"
#include "Tools/AssetEditors/MaterialFunctionEditor/MaterialFunctionEditorTool.h"
#include "Tools/AssetEditors/MeshEditor/MeshEditorTool.h"
#include "Tools/AssetEditors/MeshEditor/SkeletalMeshEditorTool.h"
#include "Tools/AssetEditors/MeshEditor/SkeletonEditorTool.h"
#include "Tools/AssetEditors/ParticleSystemEditor/ParticleSystemEditorTool.h"
#include "Tools/AssetEditors/PrefabEditor/PrefabEditorTool.h"
#include "Tools/AssetEditors/RmlUiEditor/RmlUiEditorTool.h"
#include "Tools/AssetEditors/TextureEditor/TextureEditorTool.h"
#include "Tools/AssetEditors/FontEditor/FontEditorTool.h"
#include "Tools/UI/ImGui/ImGuiAllocator.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiRenderer.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "Log/Log.h"

namespace Lumina
{
    // Project dialog palette + row primitives, mirroring ContentBrowserEditorTool's
    // constexpr ImVec4 + ImDrawList-painted rows so the dialogs match the editor.
    namespace
    {
        constexpr ImVec4 kProjDialogPanelBg     = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        constexpr ImVec4 kProjDialogRowBg       = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
        constexpr ImVec4 kProjDialogRowBgHover  = ImVec4(0.19f, 0.20f, 0.24f, 1.00f);
        constexpr ImVec4 kProjDialogRowBgActive = ImVec4(0.16f, 0.17f, 0.21f, 1.00f);
        constexpr ImVec4 kProjDialogTextPrimary = ImVec4(0.90f, 0.90f, 0.93f, 1.00f);
        constexpr ImVec4 kProjDialogTextDim     = ImVec4(0.55f, 0.56f, 0.62f, 1.00f);
        constexpr ImVec4 kProjDialogTextMuted   = ImVec4(0.42f, 0.42f, 0.47f, 1.00f);
        constexpr ImVec4 kProjDialogTextSection = ImVec4(0.50f, 0.58f, 0.72f, 1.00f);
        constexpr ImVec4 kProjDialogAccentBlue  = ImVec4(0.36f, 0.66f, 1.00f, 1.00f);
        constexpr ImVec4 kProjDialogAccentGold  = ImVec4(1.00f, 0.78f, 0.40f, 1.00f);
        constexpr ImVec4 kProjDialogAccentSoft  = ImVec4(0.45f, 0.48f, 0.55f, 1.00f);
        constexpr ImVec4 kProjDialogDanger      = ImVec4(0.96f, 0.36f, 0.38f, 1.00f);

        // Small uppercase section label in the section-text color.
        void DrawSectionHeader(const char* Label)
        {
            ImGui::Spacing();
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::TinyBold);
            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextSection);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
            ImGui::TextUnformatted(Label);
            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();
            ImGui::Spacing();
        }

        // Two-line accent row (bCompact = single-line, de-emphasized); returns true on
        // left-click. OutCloseClicked set when the trailing close button is hit (if bShowClose).
        bool DrawProjectRow(
            const char*     Icon,
            const char*     Title,
            const char*     Subtitle,
            const ImVec4&   Accent,
            bool            bCompact      = false,
            bool            bShowClose    = false,
            bool*           OutCloseClicked = nullptr)
        {
            if (OutCloseClicked)
            {
                *OutCloseClicked = false;
            }

            const float Avail     = ImGui::GetContentRegionAvail().x;
            const float Height    = bCompact ? 30.0f : 50.0f;
            const float CloseW    = bShowClose ? 28.0f : 0.0f;
            const ImVec2 P0       = ImGui::GetCursorScreenPos();
            const ImVec2 P1       = ImVec2(P0.x + Avail, P0.y + Height);

            // Seed the ID scope from title AND subtitle: two rows can share a title (the "Sandbox"
            // recent vs the "Sandbox" example, or two same-named projects in different folders), which
            // would otherwise collide on the "##row"/"##close" IDs. Subtitle is the unique path for recents.
            ImGui::PushID(Title);
            ImGui::PushID(Subtitle ? Subtitle : "");

            // Invisible button covers the full row area (minus the close column).
            ImGui::SetCursorScreenPos(P0);
            const bool bRowClicked = ImGui::InvisibleButton(
                "##row",
                ImVec2(eastl::max(Avail - CloseW, 1.0f), Height));   // floor: InvisibleButton asserts on zero width
            const bool bHovered    = ImGui::IsItemHovered();
            const bool bActive     = ImGui::IsItemActive();

            ImDrawList* DL = ImGui::GetWindowDrawList();
            const ImU32  BgCol = ImGui::ColorConvertFloat4ToU32(
                bActive ? kProjDialogRowBgActive : (bHovered ? kProjDialogRowBgHover : kProjDialogRowBg));
            DL->AddRectFilled(P0, P1, BgCol, 4.0f);
            DL->AddRectFilled(P0, ImVec2(P0.x + 3.0f, P1.y),
                ImGui::ColorConvertFloat4ToU32(Accent), 4.0f);

            // Icon column.
            const float IconCol  = 30.0f;
            const ImVec2 IconPos = ImVec2(P0.x + 12.0f, P0.y + (Height - ImGui::GetFontSize()) * 0.5f);
            ImGui::SetCursorScreenPos(IconPos);
            ImGui::PushStyleColor(ImGuiCol_Text, Accent);
            ImGui::TextUnformatted(Icon);
            ImGui::PopStyleColor();

            // Title (+ optional subtitle stacked beneath).
            const float TextX = P0.x + 12.0f + IconCol;
            if (bCompact || Subtitle == nullptr || Subtitle[0] == '\0')
            {
                ImGui::SetCursorScreenPos(ImVec2(TextX, P0.y + (Height - ImGui::GetFontSize()) * 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextPrimary);
                ImGui::TextUnformatted(Title);
                ImGui::PopStyleColor();
                if (Subtitle && Subtitle[0])
                {
                    ImGui::SameLine(0.0f, 12.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextDim);
                    ImGui::TextUnformatted(Subtitle);
                    ImGui::PopStyleColor();
                }
            }
            else
            {
                ImGui::SetCursorScreenPos(ImVec2(TextX, P0.y + 7.0f));
                ImGuiX::Font::PushFont(ImGuiX::Font::EFont::SmallBold);
                ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextPrimary);
                ImGui::TextUnformatted(Title);
                ImGui::PopStyleColor();
                ImGuiX::Font::PopFont();

                ImGui::SetCursorScreenPos(ImVec2(TextX, P0.y + 27.0f));
                ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Tiny);
                ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextDim);
                ImGui::TextUnformatted(Subtitle);
                ImGui::PopStyleColor();
                ImGuiX::Font::PopFont();
            }

            // Trailing × button.
            if (bShowClose)
            {
                const ImVec2 CloseP0 = ImVec2(P1.x - CloseW, P0.y);
                const ImVec2 CloseP1 = ImVec2(P1.x, P1.y);
                ImGui::SetCursorScreenPos(CloseP0);
                const bool bClose = ImGui::InvisibleButton("##close", ImVec2(CloseW, Height));
                const bool bCloseHover = ImGui::IsItemHovered();
                if (bCloseHover)
                {
                    DL->AddRectFilled(CloseP0, CloseP1,
                        ImGui::ColorConvertFloat4ToU32(kProjDialogDanger), 4.0f);
                }
                ImGui::SetCursorScreenPos(ImVec2(CloseP0.x + 8.0f, CloseP0.y + (Height - ImGui::GetFontSize()) * 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    bCloseHover ? kProjDialogTextPrimary : kProjDialogTextDim);
                ImGui::TextUnformatted(LE_ICON_CLOSE);
                ImGui::PopStyleColor();
                if (OutCloseClicked && bClose)
                {
                    *OutCloseClicked = true;
                }
            }
            
            const ImVec2 NextRow(P0.x, P1.y + 6.0f);
            ImGui::SetCursorScreenPos(NextRow);
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
            ImGui::PopID();
            return bRowClicked;
        }

        // Recent .lproject paths: absolute, most-recent-first, deduped, capped at
        // kMaxRecents. Backed by the CEditorSettings::RecentProjects variable.
        constexpr size_t kMaxRecents = 10;

        void PushRecentProject(FStringView LprojPath)
        {
            if (LprojPath.empty())
            {
                return;
            }
            FString NewEntry(LprojPath);
            auto Recents = GetDefault<CEditorSettings>()->RecentProjects;

            // Drop legacy name-only entries and any prior occurrence of this path.
            
            eastl::erase_if(Recents, [&](const FString& Entry)
            {
                return Entry == LprojPath || Entry.find(".lproject") == FString::npos;
            });

            Recents.insert(Recents.begin(), NewEntry);
            if (Recents.size() > kMaxRecents)
            {
                Recents.resize(kMaxRecents);
            }
            
            GetMutableDefault<CEditorSettings>()->RecentProjects = Recents;
            GConfig->SaveSettings(CEditorSettings::StaticClass());
        }

        void RemoveRecentProject(const FString& LprojPath)
        {
            auto Recents = GetDefault<CEditorSettings>()->RecentProjects;
            eastl::erase(Recents, LprojPath);
            GetMutableDefault<CEditorSettings>()->RecentProjects = Recents;
            GConfig->SaveSettings(CEditorSettings::StaticClass());
        }

        // Drop entries whose .lproject no longer exists; returns the cleaned list and
        // writes it back if anything was pruned, keeping the menu and dialog in sync.
        TVector<FString> PruneMissingRecents()
        {
            auto Recents = GetDefault<CEditorSettings>()->RecentProjects;
            const size_t Before = Recents.size();

            eastl::erase_if(Recents, [](const FString& Entry)
            {
                if (Entry.find(".lproject") == FString::npos)
                {
                    return true;
                }
                std::error_code Ec;
                return !std::filesystem::exists(Entry.c_str(), Ec);
            });

            if (Recents.size() != Before)
            {
                GetMutableDefault<CEditorSettings>()->RecentProjects = Recents;
                GConfig->SaveSettings(CEditorSettings::StaticClass());
            }
            return Recents;
        }

        // Returns the project display name from an absolute .lproject path
        // (basename without extension). Cheap; no filesystem access.
        FString DisplayNameFromLprojPath(const FString& LprojPath)
        {
            std::filesystem::path P(LprojPath.c_str());
            return FString(P.stem().string().c_str());
        }
    }

    bool FEditorUI::OnEvent(FEvent& Event)
    {
        if (Event.IsA<FKeyPressedEvent>())
        {
            FKeyPressedEvent& Key = Event.As<FKeyPressedEvent>();
            if (Key.GetKeyCode() == EKey::F1 && Key.IsShiftDown() && !Key.IsRepeat()
                && WorldEditorTool != nullptr && WorldEditorTool->HasSimulatingWorld())
            {
                FInputViewportRegistry& Reg = FInputViewportRegistry::Get();
                Reg.SetGameInputFocused(!Reg.IsGameInputFocused());
                return true;
            }
        }

        // Consume input ImGui owns so it doesn't fall through; pass everything
        // else to tools (file drops, etc.).
        const bool bIsMouseEvent =
               Event.IsA<FMouseMovedEvent>()
            || Event.IsA<FMouseButtonPressedEvent>()
            || Event.IsA<FMouseButtonReleasedEvent>()
            || Event.IsA<FMouseScrolledEvent>();

        const bool bIsKeyEvent =
               Event.IsA<FKeyPressedEvent>()
            || Event.IsA<FKeyReleasedEvent>()
            || Event.IsA<FCharInputEvent>();

        if (bIsMouseEvent || bIsKeyEvent)
        {
            const ImGuiIO& IO = ImGui::GetIO();
            if (bIsMouseEvent && IO.WantCaptureMouse)
            {
                return true;
            }
            if (bIsKeyEvent && IO.WantCaptureKeyboard)
            {
                return true;
            }
        }

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
        ImGuiContext* Context = Render().GetImGuiRenderer()->GetImGuiContext();
        ImPlotContext* PlotContext = Render().GetImGuiRenderer()->GetImPlotContext();
        ImGui::SetCurrentContext(Context);
        ImPlot::SetCurrentContext(PlotContext);

        // Editor links its own ImGui copy; install allocator here because StartupModule never runs (editor links directly, not LoadModule'd).
        ImGuiX::InstallImGuiAllocator();

        // Plugin DLLs each link their own ImGui copy too; hand the contexts to the module manager so it
        // syncs every already-loaded plugin that opted in via LUMINA_MODULE_IMGUI() (and ones loaded later).
        FModuleManager::Get().NotifyImGuiReady(Context, PlotContext);

        // Init ThumbnailManager before world load so engine primitive meshes are in the transient package before deserialization.
        (void)CThumbnailManager::Get();

        // Content-browser tiles that draw their own body (curves, etc) instead of a rendered thumbnail.
        AssetTilePainters::RegisterBuiltin();

        // A reimported texture keeps its bindless slot (RHI::Textures::Recreate repoints it), so materials
        // sampling it are already correct. What is NOT correct is a material whose block was baked while
        // that texture had no valid ResourceID -- it holds the fallback and nothing would ever rewrite it.
        AssetDataChangedHandle = AssetEvents::OnAssetDataChanged().AddLambda([](CObject* Changed)
        {
            if (const CTexture* Texture = Cast<CTexture>(Changed))
            {
                const uint32 Refreshed = RefreshMaterialsReferencingTexture(Texture);
                if (Refreshed > 0)
                {
                    LOG_INFO("Refreshed texture bindings on {} material(s) after '{}' changed.",
                             Refreshed, Texture->GetName().c_str());
                }
            }
        });

        // Editor owns input until the user hits Play (the registry flag defaults true so packaged builds work).
        FInputViewportRegistry::Get().SetGameInputFocused(false);

        RegisterBuiltinEditorTools();
        RegisterBuiltinAssetActions();

        PropertyCustomizationRegistry = Memory::New<FPropertyCustomizationRegistry>();
        PropertyCustomizationRegistry->RegisterPropertyCustomization(TBaseStructure<FVector2>::Get()->GetName(), []
        {
            return FVec2PropertyCustomization::MakeInstance();
        });
        PropertyCustomizationRegistry->RegisterPropertyCustomization(TBaseStructure<FVector3>::Get()->GetName(), []
        {
            return FVec3PropertyCustomization::MakeInstance();
        });
        PropertyCustomizationRegistry->RegisterPropertyCustomization(TBaseStructure<FVector4>::Get()->GetName(), []
        {
            return FVec4PropertyCustomization::MakeInstance();
        });
        PropertyCustomizationRegistry->RegisterPropertyCustomization(TBaseStructure<FQuat>::Get()->GetName(), []
        {
            return FVec3PropertyCustomization::MakeInstance();
        });
        PropertyCustomizationRegistry->RegisterPropertyCustomization(SEntityScriptComponent::StaticStruct()->GetName(), []
        {
            return FEntityScriptComponentCustomization::MakeInstance();
        });
        PropertyCustomizationRegistry->RegisterPropertyCustomization(TBaseStructure<FTransform>::Get()->GetName(), []
        {
            return FTransformPropertyCustomization::MakeInstance();
        });

        PropertyCustomizationRegistry->RegisterPropertyCustomization(FAssetRef::StaticStruct()->GetName(), []
        {
           return FAssetRefPropertyCustomization::MakeInstance();
        });
        
        PropertyCustomizationRegistry->RegisterPropertyCustomization(SCustomPrimitiveData::StaticStruct()->GetName(), []
        {
           return FCustomPrimDataPropertyCustomization::MakeInstance();
        });

        PropertyCustomizationRegistry->RegisterPropertyCustomization(FParticleParameter::StaticStruct()->GetName(), []
        {
           return FParticleParameterCustomization::MakeInstance();
        });

        PropertyCustomizationRegistry->RegisterPropertyCustomization(SParticleParam::StaticStruct()->GetName(), []
        {
           return FParticleParamCustomization::MakeInstance();
        });

        PropertyCustomizationRegistry->RegisterPropertyCustomization(SCurve::StaticStruct()->GetName(), []
        {
           return FCurvePropertyCustomization::MakeInstance();
        });

        PropertyCustomizationRegistry->RegisterPropertyCustomization(SGradient::StaticStruct()->GetName(), []
        {
           return FGradientPropertyCustomization::MakeInstance();
        });

        PropertyCustomizationRegistry->RegisterPropertyCustomization(SKey::StaticStruct()->GetName(), []
        {
           return FKeyPropertyCustomization::MakeInstance();
        });

        PropertyCustomizationRegistry->RegisterPropertyCustomization(FGameplayTag::StaticStruct()->GetName(), []
        {
           return FGameplayTagPropertyCustomization::MakeInstance();
        });

        PropertyCustomizationRegistry->RegisterPropertyCustomization(SDataTableRowHandle::StaticStruct()->GetName(), []
        {
           return FDataTableRowHandlePropertyCustomization::MakeInstance();
        });

        EditorWindowClass.ClassId                       = ImHashStr("EditorWindowClass");
        EditorWindowClass.DockingAllowUnclassed         = false;
        EditorWindowClass.ViewportFlagsOverrideSet      = ImGuiViewportFlags_NoAutoMerge;
        EditorWindowClass.ViewportFlagsOverrideClear    = ImGuiViewportFlags_NoTaskBarIcon;
        EditorWindowClass.ParentViewportId              = 0; // Top level window
        EditorWindowClass.DockingAlwaysTabBar           = true;

        // Same starting scene a newly created world asset gets, so the editor's opening view is not a
        // different world from the one File > New produces. Created here but POPULATED below, after
        // the project has had its chance to load -- see the comment at the PopulateDefaultScene call.
        //
        // Held through a TObjectPtr, not a raw pointer: the world editor is this world's only other owner,
        // and the project load below can hand that tool a different world, which frees this one on the
        // spot (FEditorTool::SetWorld). A strong ref here keeps it alive to the end of Initialize so the
        // check at the populate call is an identity test and not a compare against a freed address that
        // the replacement world may have been allocated into.
        TObjectPtr<CWorld> World = NewObject<CWorld>(nullptr, "Transient World", FGuid::New(), OF_Transient);

        WorldEditorTool = CreateTool<FWorldEditorTool>(this, World);
        ConsoleLogTool = CreateTool<FConsoleLogEditorTool>(this);
        ContentBrowser = CreateTool<FContentBrowserEditorTool>(this);

        // Multiplayer PIE: spawn/destroy extra-player Game Preview tools when the world editor starts/stops play.
        (void)WorldEditorTool->GetOnPreviewStartRequestedDelegate().AddLambda([this]() { CreateGameViewportTool(); });
        (void)WorldEditorTool->GetOnPreviewStopRequestedDelegate().AddLambda([this]() { DestroyGameViewportTool(); });

        // Footer drawers: Content Browser (Ctrl+Space, UE-style) and Output Log.
        // They start undocked, living in the bottom status bar instead of a dock split.
        FooterDrawers.push_back({ ContentBrowser, LE_ICON_FOLDER,       "Content Browser", ImGuiMod_Ctrl | ImGuiKey_Space });
        FooterDrawers.push_back({ ConsoleLogTool, LE_ICON_CONSOLE_LINE, "Output Log",       ImGuiMod_Ctrl | ImGuiKey_J });
        
        if (GEditorEngine->GetProjectName().empty())
        {
            // No --Project at startup: try the last-opened Editor.StartupProject,
            // falling through to the Open dialog if that's also missing/stale.
            const std::string StartupPath = GetDefault<CEditorSettings>()->StartupProject.c_str();
            if (!StartupPath.empty() && StartupPath != "NULL")
            {
                std::error_code Ec;
                if (std::filesystem::exists(StartupPath, Ec))
                {
                    GEditorEngine->LoadProject(FStringView(StartupPath.c_str(), StartupPath.size()));
                    OnProjectLoaded();
                }
            }

            if (GEditorEngine->GetProjectName().empty())
            {
                OpenProjectDialog();
            }
        }
        else
        {
            OnProjectLoaded();
        }

        // Populated only now, because the default scene references engine content by path and the
        // asset registry is not discovered until a project loads. With no --Project on the command
        // line that load happens in the block above, so building the scene any earlier means every
        // asset lookup runs against an empty registry and silently falls back.
        //
        // But that same load can open the project's EditorStartupMap, which retargets the world editor and
        // frees this world. Skip in that case: the user is looking at their own map, and populating a world
        // nothing owns any more writes through freed memory.
        if (WorldEditorTool->GetWorld() == World)
        {
            EditorEntityUtils::PopulateDefaultScene(World);
        }
    }

    void FEditorUI::Deinitialize(const FUpdateContext& UpdateContext)
    {
        if (AssetDataChangedHandle.IsValid())
        {
            AssetEvents::OnAssetDataChanged().Remove(AssetDataChangedHandle);
            AssetDataChangedHandle = {};
        }

        // Shutting down is not the user closing their tabs; see bTearingDownTools.
        bTearingDownTools = true;

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
        ImGuizmo::BeginFrame();

        // ImGui stands down (game owns mouse + keyboard) while playing AND game-input focus is on. Driven
        // centrally off the single registry flag so it's correct regardless of which preview window is active.
        {
            ImGuiIO& IO = ImGui::GetIO();

            const ImGuiConfigFlags Mask = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard | ImGuiConfigFlags_NoMouseCursorChange;

            FInputViewportRegistry& Reg = FInputViewportRegistry::Get();
            const FInputViewport* Active = Reg.GetActiveViewport();
            const bool bGameOwnsInput = Reg.IsGameInputFocused()
                                      && Active != nullptr
                                      && Active->GetWorld() != nullptr
                                      && Active->GetWorld()->IsGameWorld();
            // Reassert while the game owns input; clear ONCE on the way out. Never clear per-frame: the editor
            // camera sets NoMouse itself during RMB-look (via FInputProcessor::SetMouseMode), and clobbering it
            // every frame makes ImGui fight the capture (cursor flicker / camera lock).
            if (bGameOwnsInput)            { IO.ConfigFlags |= Mask; }
            else if (bWasGameOwningInput)  { IO.ConfigFlags &= ~Mask; }
            bWasGameOwningInput = bGameOwnsInput;
        }

        auto TitleBarMenuContents = [this, &UpdateContext] ()
        {
            DrawTitleBarMenu(UpdateContext);
        };

        // Composed and MEASURED before layout. The right-hand region was a hard-coded 230px, so adding a
        // stat -- or just a wider frame time -- silently clipped the text off the end of the bar.
        const FTitleBarStats TitleStats = BuildTitleBarStats(UpdateContext);

        auto TitleBarInfoContents = [this, &TitleStats] ()
        {
            DrawTitleBarInfoStats(TitleStats);
        };

        // The menu section is not sized here: it takes whatever the info section and window controls
        // leave, so the project name is bounded by the window rather than by a constant.
        TitleBar.Draw(TitleBarMenuContents, TitleBarInfoContents, TitleStats.Width);

        // Reserve the bottom status bar before the dockspace reads the viewport work area.
        DrawStatusBar(UpdateContext);

        // "V2": renamed from "EditorDockSpace" so pre-footer-drawer layouts (with a
        // permanent bottom split) are orphaned and rebuilt with the world editor filling it.
        const ImGuiID DockspaceID = ImGui::GetID("EditorDockSpaceV2");
        MainDockspaceID = DockspaceID;

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        // NoDocking is not cosmetic. Without it this window registers as a second, full-viewport dockable
        // drop target sitting on top of the dockspace's own one, and -- worse -- it can become the drag
        // PAYLOAD: BeginDockableDragDropSource resolves the moving window to its RootWindowDockTree, which
        // for the DockSpace child host is this window. On the frame the drag is released, g.MovingWindow is
        // already null while the payload is still live, so Begin's "don't target the window being moved"
        // guard passes and imgui queues a dock of this window into itself -> IM_ASSERT(target != payload).
        // Real docking is unaffected: drops into the dockspace go through DockNodeUpdate against the
        // "EditorDockSpaceWindow/DockSpace_XXXXXXXX" child that DockSpace() creates, not through this window.
        constexpr ImGuiWindowFlags WindowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking;

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
                ImGui::DockBuilderFinish(DockspaceID);

                // World editor fills the whole dockspace; the Content Browser and Output
                // Log default to footer drawers rather than eating a permanent bottom split.
                ImGui::DockBuilderDockWindow(WorldEditorTool->GetToolName().c_str(), DockspaceID);
            }

            // A drawer asked to be docked last frame. Resolved here, before DockSpace() consumes the node
            // for this frame, and consumed by SubmitToolMainWindow further down the same frame.
            if (PendingBottomDockTool != nullptr)
            {
                PendingBottomDockTool->DesiredDockID = GetOrCreateBottomDockID(PendingBottomDockHeightFrac);
                PendingBottomDockTool = nullptr;
            }

            // Create the actual dock space
            ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0);
            ImGui::DockSpace(DockspaceID, viewport->WorkSize, 0, &EditorWindowClass);
            ImGui::PopStyleVar();
        }
        
        
        ImGui::End();

        // One-time startup notices, raised on the first live UI frame so the toasts actually render (boot
        // itself is pre-UI, so a modal can't show there). Debug builds warn about the perf hit.
        static bool bStartupNoticesShown = false;
        if (!bStartupNoticesShown)
        {
            bStartupNoticesShown = true;
#if defined(LE_DEBUG)
            LOG_WARN("Running a DEBUG build. Performance is greatly reduced; use Development for normal work.");
            ImGuiX::Notifications::NotifyWarning("Debug build: performance is greatly reduced. Build in Development for normal editing and play.");
#endif
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F5))
        {
            CMaterial::CreateDefaultMaterial();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F9, false))
        {
            const auto Source = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)
                ? Screenshot::ECaptureSource::SceneHDR
                : Screenshot::ECaptureSource::FinalLDR;
            Screenshot::CaptureActiveWorld(Source);
        }

        // Tracy sits on F8 next to the other external-tool keys (F9 screenshot, F11 RenderDoc). It used to
        // be advertised as Ctrl+P, which quick-open has taken -- and which never had a handler regardless,
        // so the menu was the only way to launch it.
        if (ImGui::IsKeyPressed(ImGuiKey_F8, false))
        {
            LaunchTracyProfiler();
        }

        // Recompile + hot-swap all C# script assemblies (every Scripts/ across game, plugins, engine). The
        // chord is rebindable in Editor Settings > General > Hotkeys (default Ctrl+Shift+R).
        {
            // EKey holds GLFW keycodes, which run contiguously for letters / digits / function keys -- the
            // realistic hotkey set -- so they map onto the matching ImGuiKey ranges by offset.
            auto EKeyToImGuiKey = [](EKey Key) -> ImGuiKey
            {
                const int Code = (int)Key;
                if (Key >= EKey::A  && Key <= EKey::Z)   { return (ImGuiKey)(ImGuiKey_A  + (Code - (int)EKey::A)); }
                if (Key >= EKey::D0 && Key <= EKey::D9)  { return (ImGuiKey)(ImGuiKey_0  + (Code - (int)EKey::D0)); }
                if (Key >= EKey::F1 && Key <= EKey::F12) { return (ImGuiKey)(ImGuiKey_F1 + (Code - (int)EKey::F1)); }
                return ImGuiKey_None;
            };

            const SKey& Bind = GetDefault<CEditorSettings>()->ReloadScriptsHotkey;
            const ImGuiKey ReloadKey = Bind.IsKeyboard() ? EKeyToImGuiKey(Bind.Key) : ImGuiKey_None;
            if (ReloadKey != ImGuiKey_None)
            {
                ImGuiKeyChord Chord = ReloadKey;
                if (Bind.bCtrl)  { Chord |= ImGuiMod_Ctrl; }
                if (Bind.bShift) { Chord |= ImGuiMod_Shift; }
                if (Bind.bAlt)   { Chord |= ImGuiMod_Alt; }

                if (ImGui::IsKeyChordPressed(Chord))
                {
                    DotNet::ReloadScripts();
                }
            }
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

        if (GEngine->IsCloseRequested())
        {
            if (!bVerifyingDirtyPackages)
            {
                bVerifyingDirtyPackages = true;
                VerifyDirtyPackages();
            }

            // Keep the engine alive while the prompt is up; Cancel re-arms the guard
            // via FApplication::CancelExit in VerifyDirtyPackages's callback.
            if (ModalManager.HasModal())
            {
                GEngine->SetEngineReadyToClose(false);
            }
        }
        
        FEditorTool* ToolToClose = nullptr;

        for (FEditorTool* Tool : EditorTools)
        {
            // Undocked drawer tools render in the footer drawer, not the dock layout;
            // still tick them so their per-frame logic keeps running.
            if (FFooterDrawer* Drawer = FindDrawerForTool(Tool); Drawer != nullptr && !Drawer->bDocked)
            {
                Tool->Update(UpdateContext);
                continue;
            }

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

            if (FFooterDrawer* Drawer = FindDrawerForTool(Tool); Drawer != nullptr && !Drawer->bDocked)
            {
                continue;
            }

            DrawToolContents(UpdateContext, Tool);
        }

        // A docked drawer tool closed via its tab returns to drawer mode instead of
        // being destroyed.
        if (ToolToClose)
        {
            if (FFooterDrawer* Drawer = FindDrawerForTool(ToolToClose); Drawer != nullptr)
            {
                Drawer->bDocked = false;
                ToolToClose = nullptr;
            }
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
        
        DrawFooterDrawer(UpdateContext);

        ModalManager.DrawDialogue();

        // Run any dialog queued last frame (e.g. Open -> New); FEditorModalManager
        // rejects CreateDialogue while a modal is active, so chains defer a frame.
        if (PendingDialogAction && !ModalManager.HasModal())
        {
            TFunction<void()> Action = std::move(PendingDialogAction);
            PendingDialogAction = nullptr;
            Action();
        }
    }

    void FEditorUI::OnUpdate(const FUpdateContext& UpdateContext)
    {
        LUMINA_PROFILE_SCOPE();

        // Deferred to the first update rather than Initialize: the asset registry has to be populated
        // before a GUID can resolve, and opening a tool wants a live ImGui frame for the focus call.
        if (bSessionRestorePending)
        {
            bSessionRestorePending = false;
            RestoreSessionTabs();
        }

        // Save All is global, unlike Ctrl+S which the focused tool claims. Handled once here rather
        // than in the per-tool loop, which would fire it for every open tool.
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            SaveAllDirtyPackages();
        }

        // Ctrl+P quick-open. Gated on no active text input so it cannot fire out of a rename field or
        // the search box of another panel.
        if (ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !ImGui::GetIO().WantTextInput
            && ImGui::IsKeyPressed(ImGuiKey_P, false))
        {
            OpenAssetSearchModal();
        }

        // Drain a budget of pending thumbnail renders before the UI draws this frame's tiles. The render
        // itself is extract-phase only (World::Extract + GPU readback); keeping the budget small avoids a
        // hitch while cold thumbnails fill in a few per frame.
        CThumbnailManager::Get().ProcessRenderQueue();

        // Recompile one stale-template material per frame (queued in CMaterial::PostLoad when an asset's
        // baked shaders predate the current shader templates).
        ProcessStaleMaterialRecompiles();

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

        if (ImGui::IsKeyPressed(ImGuiKey_LeftShift) && ImGui::IsKeyPressed(ImGuiKey_F1))
        {
            FInputProcessor::Get().SetMouseMode(EMouseMode::Normal);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && GamePreviewTool != nullptr)
        {
            WorldEditorTool->GetOnPreviewStopRequestedDelegate().Broadcast();
        }
    }
    
    double FEditorUI::GetToolWorldUpdateInterval(const CWorld* ToolWorld, bool bFocused) const
    {
        // The tool being worked in always runs live. "Focused" is the active TOOL rather than the viewport
        // specifically: editing a material's node graph is still working in that material, and its preview
        // should keep up with every edit.
        if (bFocused || ToolWorld == nullptr)
        {
            return 0.0;
        }

        // Editor preview worlds only. A Game/Simulation world is PIE -- throttling one because focus moved
        // to another panel would stutter the thing being played, and on a net session it would stutter every
        // client with it.
        if (ToolWorld->GetWorldType() != EWorldType::Editor)
        {
            return 0.0;
        }

        const int32 BackgroundFPS = GetDefault<CEditorSettings>()->MaxBackgroundFPS;
        if (BackgroundFPS <= 0)
        {
            return 0.0;
        }

        return 1.0 / (double)BackgroundFPS;
    }

    void FEditorUI::DestroyTool(const FUpdateContext& UpdateContext, FEditorTool* Tool)
    {
        auto Itr = eastl::find(EditorTools.begin(), EditorTools.end(), Tool);
        ASSERT(Itr != EditorTools.end());

        EditorTools.erase(Itr);

        // Closing a tab is as much a session change as opening one, and is written through the same way.
        ForgetSessionTab(Tool);

        for (auto MapItr = ActiveAssetTools.begin(); MapItr != ActiveAssetTools.end(); ++MapItr)
        {
            if (MapItr->second == Tool)
            {
                ActiveAssetTools.erase(MapItr);
                break;
            }
        }

        for (auto MapItr = ActiveFileTools.begin(); MapItr != ActiveFileTools.end(); ++MapItr)
        {
            if (MapItr->second == Tool)
            {
                ActiveFileTools.erase(MapItr);
                break;
            }
        }

        if (Tool == GamePreviewTool)
        {
            WorldEditorTool->NotifyPlayInEditorStop();
            GamePreviewTool = nullptr;
        }

        // Keep extra-player preview bookkeeping consistent when a preview tab is closed directly.
        for (auto PreviewItr = ExtraGamePreviews.begin(); PreviewItr != ExtraGamePreviews.end(); ++PreviewItr)
        {
            if (PreviewItr->Tool == Tool)
            {
                ExtraGamePreviews.erase(PreviewItr);
                break;
            }
        }

        // Nothing else drops this reference, so a closed tool would leave File > Save and Ctrl+S
        // calling OnSave on freed memory.
        if (Tool == LastActiveTool)
        {
            LastActiveTool = nullptr;
        }

        Tool->Deinitialize(UpdateContext);
        Memory::Delete(Tool);
    }

    void FEditorUI::PushModal(const FString& Title, ImVec2 Size, TMoveOnlyFunction<bool()> DrawFunction)
    {
        ModalManager.CreateDialogue(Title, Size, Move(DrawFunction));
    }

    namespace
    {
        // Resolve devenv.exe for a VS major-version range via vswhere; canonical Microsoft recipe.
        FString FindDevEnv(const char* VersionRange)
        {
            const char* VsWhereCandidates[] =
            {
                R"(C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe)",
                R"(C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe)",
            };

            for (const char* Candidate : VsWhereCandidates)
            {
                std::error_code Ec;
                if (!std::filesystem::exists(Candidate, Ec))
                {
                    continue;
                }

                FString Args = "-latest -products * -version \"";
                Args += VersionRange;
                Args += "\" -property productPath -nologo";

                FString FirstLine;
                const int ExitCode = Platform::RunProcessAndWaitCapture(
                    UTF8_TO_TCHAR(Candidate), UTF8_TO_TCHAR(Args.c_str()), nullptr,
                    [&FirstLine](FStringView Line)
                    {
                        if (FirstLine.empty() && !Line.empty())
                        {
                            FirstLine.assign(Line.data(), Line.size());
                        }
                    });

                if (ExitCode == 0 && !FirstLine.empty())
                {
                    return FirstLine;
                }
            }
            return FString();
        }

        FString FindVSCode()
        {
            TVector<FString> Candidates;
            if (const char* LocalAppData = std::getenv("LOCALAPPDATA"))
            {
                Candidates.emplace_back(FString(LocalAppData) + R"(\Programs\Microsoft VS Code\Code.exe)");
            }
            Candidates.emplace_back(R"(C:\Program Files\Microsoft VS Code\Code.exe)");

            for (const FString& Candidate : Candidates)
            {
                std::error_code Ec;
                if (std::filesystem::exists(Candidate.c_str(), Ec))
                {
                    return Candidate;
                }
            }
            return FString();
        }

        FString FindRider()
        {
            // JetBrains Toolbox shim first, then classic installs (versioned folders) under Program Files.
            if (const char* LocalAppData = std::getenv("LOCALAPPDATA"))
            {
                FString Toolbox = FString(LocalAppData) + R"(\Programs\Rider\bin\rider64.exe)";
                std::error_code Ec;
                if (std::filesystem::exists(Toolbox.c_str(), Ec))
                {
                    return Toolbox;
                }
            }

            FString Best;
            std::error_code Ec;
            for (const auto& Entry : std::filesystem::directory_iterator(R"(C:\Program Files\JetBrains)", Ec))
            {
                const std::string Folder = Entry.path().filename().string();
                if (Folder.rfind("JetBrains Rider", 0) != 0)
                {
                    continue;
                }
                const std::string Exe = (Entry.path() / "bin" / "rider64.exe").string();
                std::error_code ExeEc;
                if (std::filesystem::exists(Exe, ExeEc) && (Best.empty() || Best.comparei(Exe.c_str()) < 0))
                {
                    Best = Exe.c_str();
                }
            }
            return Best;
        }
    }

    void FEditorUI::OpenScriptInExternalEditor(FStringView ScriptPath)
    {
        const FString File(ScriptPath.data(), ScriptPath.size());
        const CScriptEditorSettings* Settings = GetDefault<CScriptEditorSettings>();

        FString Exe;
        bool bVisualStudio = false;
        if (!Settings->CustomEditorPath.empty())
        {
            Exe = Settings->CustomEditorPath;
        }
        else
        {
            // Cached per editor choice; disk probing / the vswhere subprocess don't change in-session.
            switch (Settings->ScriptEditor)
            {
            case EScriptEditor::VisualStudio2022: { static FString Found = FindDevEnv("[17.0,18.0)"); Exe = Found; bVisualStudio = true; break; }
            case EScriptEditor::VisualStudio2026: { static FString Found = FindDevEnv("[18.0,19.0)"); Exe = Found; bVisualStudio = true; break; }
            case EScriptEditor::VSCode:           { static FString Found = FindVSCode(); Exe = Found; break; }
            case EScriptEditor::Rider:            { static FString Found = FindRider(); Exe = Found; break; }
            case EScriptEditor::SystemDefault:    break;
            }
        }

        std::error_code Ec;
        if (!Exe.empty() && std::filesystem::exists(Exe.c_str(), Ec))
        {
            FString NativeFile = File;
        #if defined(LE_PLATFORM_WINDOWS)
            eastl::replace(NativeFile.begin(), NativeFile.end(), '/', '\\');
        #endif

            // /Edit reuses a running Visual Studio instance instead of spawning a new one.
            FString Params = bVisualStudio ? FString("/Edit \"") : FString("\"");
            Params += NativeFile;
            Params += "\"";

            FString ExeQuoted = "\"";
            ExeQuoted += Exe;
            ExeQuoted += "\"";

            if (Platform::LaunchProcess(UTF8_TO_TCHAR(ExeQuoted.c_str()), UTF8_TO_TCHAR(Params.c_str())) == 0)
            {
                return;
            }
            LOG_WARN("Failed to launch script editor '{}'; falling back to the OS file association.", Exe.c_str());
        }
        else if (Settings->ScriptEditor != EScriptEditor::SystemDefault || !Settings->CustomEditorPath.empty())
        {
            LOG_WARN("Configured script editor was not found; falling back to the OS file association.");
        }

        Platform::LaunchURL(UTF8_TO_TCHAR(File.c_str()));
    }

    void FEditorUI::OpenScriptEditor(FStringView ScriptPath)
    {
        OpenScriptInExternalEditor(ScriptPath);
    }

    namespace
    {
        constexpr const char* GSessionAssetPrefix = "asset:";
        constexpr const char* GSessionFilePrefix  = "file:";

        bool SessionKeyHasPrefix(const FString& Key, const char* Prefix)
        {
            return Key.find(Prefix) == 0;
        }
    }

    void FEditorUI::RecordSessionTab(FEditorTool* Tool, FString Key)
    {
        if (Tool == nullptr || Key.empty())
        {
            return;
        }

        // Tracked even when the entry already exists, so closing a RESTORED tab still finds its key.
        ToolSessionKeys.insert_or_assign(Tool, Key);

        CEditorSessionSettings* Settings = GetMutableDefault<CEditorSessionSettings>();
        if (Settings == nullptr)
        {
            return;
        }

        // Already listed means this open is the restore replaying the list -- appending would duplicate
        // it, and saving would rewrite the file mid-read.
        if (eastl::find(Settings->OpenTabs.begin(), Settings->OpenTabs.end(), Key) != Settings->OpenTabs.end())
        {
            return;
        }

        Settings->OpenTabs.push_back(Move(Key));
        GConfig->SaveSettings(CEditorSessionSettings::StaticClass());
    }

    void FEditorUI::ForgetSessionTab(FEditorTool* Tool)
    {
        // Teardown closes every tool; treating that as "the user closed these tabs" is what emptied the
        // list on every clean shutdown. The list only shrinks on a deliberate close.
        if (bTearingDownTools)
        {
            return;
        }

        auto KeyItr = ToolSessionKeys.find(Tool);
        if (KeyItr == ToolSessionKeys.end())
        {
            return;
        }

        const FString Key = KeyItr->second;
        ToolSessionKeys.erase(KeyItr);

        CEditorSessionSettings* Settings = GetMutableDefault<CEditorSessionSettings>();
        if (Settings == nullptr)
        {
            return;
        }

        auto TabItr = eastl::find(Settings->OpenTabs.begin(), Settings->OpenTabs.end(), Key);
        if (TabItr == Settings->OpenTabs.end())
        {
            return;
        }

        // Ordered erase, not swap-and-pop: the order of this list is the whole point.
        Settings->OpenTabs.erase(TabItr);
        GConfig->SaveSettings(CEditorSessionSettings::StaticClass());
    }

    void FEditorUI::RestoreSessionTabs()
    {
        const CEditorSessionSettings* Settings = GetDefault<CEditorSessionSettings>();
        if (Settings == nullptr || !Settings->bRestoreOpenTabs || Settings->OpenTabs.empty())
        {
            return;
        }

        // Copy: opening a tab writes back into the live list.
        const TVector<FString> Tabs = Settings->OpenTabs;

        // Entries whose asset the registry has not scanned yet. The retry below is another pass over the
        // same list, skipping whatever an earlier pass already restored.
        int32 NumUnscanned = 0;

        TVector<FString> Survivors;
        Survivors.reserve(Tabs.size());

        for (const FString& Tab : Tabs)
        {
            // Restoring twice is not free: OpenAssetEditor reloads the asset graph and focuses the tool.
            // Since the retry fires on every registry change, an asset import was enough to steal focus
            // and dismiss the footer drawer mid-click.
            if (RestoredSessionTabs.find(Tab) != RestoredSessionTabs.end())
            {
                Survivors.push_back(Tab);
                continue;
            }

            if (SessionKeyHasPrefix(Tab, GSessionAssetPrefix))
            {
                const FString GuidText = Tab.substr(strlen(GSessionAssetPrefix));
                const TOptional<FGuid> Guid = FGuid::TryParse(FStringView(GuidText.c_str(), GuidText.size()));
                if (!Guid.has_value())
                {
                    continue;   // unparseable: drop it
                }

                // Distinguish "deleted" from "not scanned yet". Dropping an entry the registry simply
                // has not reached would silently lose tabs on a cold start, so only a miss AFTER the
                // registry has assets in it counts as gone.
                if (FAssetRegistry::Get().GetAssetByGUID(*Guid) == nullptr)
                {
                    Survivors.push_back(Tab);
                    ++NumUnscanned;
                    continue;
                }

                Survivors.push_back(Tab);
                RestoredSessionTabs.insert(Tab);
                OpenAssetEditor(*Guid);
            }
            else if (SessionKeyHasPrefix(Tab, GSessionFilePrefix))
            {
                const FString Path = Tab.substr(strlen(GSessionFilePrefix));
                if (!VFS::Exists(FStringView(Path.c_str(), Path.size())))
                {
                    continue;   // file is genuinely gone: drop it
                }

                Survivors.push_back(Tab);
                RestoredSessionTabs.insert(Tab);
                OpenFileEditor(FStringView(Path.c_str(), Path.size()));
            }
        }

        if (Survivors.size() != Tabs.size())
        {
            LOG_INFO("Editor session: dropped {} tab(s) that no longer resolve.", Tabs.size() - Survivors.size());

            CEditorSessionSettings* MutableSettings = GetMutableDefault<CEditorSessionSettings>();
            MutableSettings->OpenTabs = Move(Survivors);
            GConfig->SaveSettings(CEditorSessionSettings::StaticClass());
        }

        // Same pattern as the startup map: the registry scan runs asynchronously, so retry on each
        // update until every tab has resolved, then unhook.
        if (NumUnscanned > 0)
        {
            if (!SessionRestoreRetryHandle.IsValid())
            {
                SessionRestoreRetryHandle = FAssetRegistry::Get().GetOnAssetRegistryUpdated().AddLambda([this]
                {
                    RestoreSessionTabs();
                });
            }
        }
        else if (SessionRestoreRetryHandle.IsValid())
        {
            FAssetRegistry::Get().GetOnAssetRegistryUpdated().Remove(SessionRestoreRetryHandle);
            SessionRestoreRetryHandle = {};
        }
    }

    FEditorTool* FEditorUI::FinalizeNewTool(FEditorTool* Tool)
    {
        if (Tool != nullptr)
        {
            Tool->Initialize();
            ToolsPendingAdd.push(Tool);
        }

        return Tool;
    }

    void FEditorUI::RegisterBuiltinEditorTools()
    {
        FEditorToolRegistry& Registry = FEditorToolRegistry::Get();
        const FName Owner = FEditorToolRegistry::BuiltInOwner();

        // Asset editors, keyed by asset class. Lookup walks the class hierarchy
        // most-derived first, so sibling/override registrations resolve cleanly.
        Registry.RegisterAssetEditor<CParticleSystem,     FParticleSystemEditorTool>(Owner);
        Registry.RegisterAssetEditor<CMaterial,           FMaterialEditorTool>(Owner);
        Registry.RegisterAssetEditor<CMaterialInstance,   FMaterialInstanceEditorTool>(Owner);
        Registry.RegisterAssetEditor<CMaterialFunction,   FMaterialFunctionEditorTool>(Owner);
        Registry.RegisterAssetEditor<CAnimationGraph,     FAnimationGraphEditorTool>(Owner);
        Registry.RegisterAssetEditor<CBlackboard,         FBlackboardEditorTool>(Owner);
        Registry.RegisterAssetEditor<CDataAsset,          FDataAssetEditorTool>(Owner);
        Registry.RegisterAssetEditor<CDataTable,          FDataTableEditorTool>(Owner);
        Registry.RegisterAssetEditor<CPhysicsMaterial,    FPhysicsMaterialEditorTool>(Owner);
        Registry.RegisterAssetEditor<CPhysicsAsset,       FPhysicsAssetEditorTool>(Owner);
        Registry.RegisterAssetEditor<CBlendSpace,         FBlendSpaceEditorTool>(Owner);
        Registry.RegisterAssetEditor<CCollisionShape,     FCollisionShapeEditorTool>(Owner);
        Registry.RegisterAssetEditor<CCurveAsset,         FCurveAssetEditorTool>(Owner);
        Registry.RegisterAssetEditor<CAudioStream,        FAudioStreamEditorTool>(Owner);
        Registry.RegisterAssetEditor<CGeometryCollection, FGeometryCollectionEditorTool>(Owner);
        Registry.RegisterAssetEditor<CTexture,            FTextureEditorTool>(Owner);
        Registry.RegisterAssetEditor<CFont,               FFontEditorTool>(Owner);
        Registry.RegisterAssetEditor<CStaticMesh,         FStaticMeshEditorTool>(Owner);
        Registry.RegisterAssetEditor<CSkeleton,           FSkeletonEditorTool>(Owner);
        Registry.RegisterAssetEditor<CAnimation,          FAnimationEditorTool>(Owner);
        Registry.RegisterAssetEditor<CSkeletalMesh,       FSkeletalMeshEditorTool>(Owner);
        Registry.RegisterAssetEditor<CPrefab,             FPrefabEditorTool>(Owner);

        // File editors, keyed by extension (CObject-less, raw content).
        Registry.RegisterFileEditor<FRmlUiEditorTool>({ ".rml", ".rcss" }, Owner);
    }

    void FEditorUI::OpenAssetEditor(const FGuid& AssetGUID)
    {
        // Phased parallel load: opening a world/large asset fans its whole Hard/Owned dependency closure
        // across worker threads instead of the inline depth-first walk. Falls back to a plain load inside
        // LoadAssetGraph if this isn't a registered asset.
        CObject* Asset = CPackage::LoadAssetGraph(AssetGUID);

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

        // While PIE/Simulate is running the tool's World is the transient duplicate, and the world actually
        // being edited is stashed in ProxyWorld. Comparing against GetWorld() would never match the asset
        // on disk, so re-opening the level you are already playing would fall through and stop the session
        // just to re-open the same world. GetPIESourceWorld() is null whenever no session is live.
        CWorld* PIESourceWorld = WorldEditorTool->GetPIESourceWorld();
        const CWorld* EditedWorld = (PIESourceWorld != nullptr) ? PIESourceWorld : WorldEditorTool->GetWorld();

        if (EditedWorld == Asset)
        {
            const char* Name = WorldEditorTool->GetToolName().c_str();
            ImGui::SetWindowFocus(Name);
            return;
        }

        // Worlds are special: rather than spawning a dedicated tool we retarget
        // the singleton WorldEditorTool, so this stays outside the registry.
        if (Asset->IsA<CWorld>())
        {
            // Switching worlds mid-session kills PIE/Simulate, which is destructive enough to confirm.
            // The same-world case never reaches here -- it focuses the tool above.
            if (WorldEditorTool->HasSimulatingWorld())
            {
                PromptOpenWorldDuringPlay(AssetGUID, Asset->GetName().ToString());
                return;
            }

            WorldEditorTool->SetWorld(Cast<CWorld>(Asset));
            return;
        }

        // The registry hands ownership over; FinalizeNewTool puts it into EditorTools, which is
        // what DestroyTool later frees, so the pointer is released into that model here.
        FEditorToolPtr CreatedTool = FEditorToolRegistry::Get().CreateAssetEditor(this, Asset);

        if (FEditorTool* NewTool = FinalizeNewTool(CreatedTool.release()))
        {
            ActiveAssetTools.insert_or_assign(Asset, NewTool);
            RecordSessionTab(NewTool, FString(GSessionAssetPrefix) + AssetGUID.ToString());
        }
    }

    void FEditorUI::PromptOpenWorldDuringPlay(const FGuid& AssetGUID, const FString& WorldName)
    {
        // The source world, not WorldEditorTool->GetWorld() -- the latter is the transient PIE duplicate,
        // whose package dirty flag says nothing about the edits the user would be walking away from.
        const CWorld* SourceWorld = WorldEditorTool->GetPIESourceWorld();

        const bool bSourceDirty = SourceWorld != nullptr
            && SourceWorld->GetPackage() != nullptr
            && SourceWorld->GetPackage()->IsDirty();

        const FString CurrentName = (SourceWorld != nullptr) ? SourceWorld->GetName().ToString() : FString("the current world");

        ModalManager.CreateDialogue("Stop Play In Editor?", ImVec2(560, 280),
            [this, AssetGUID, WorldName, CurrentName, bSourceDirty]() -> bool
        {
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::MediumBold);
            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogAccentGold);
            ImGui::TextUnformatted(LE_ICON_ALERT_CIRCLE_OUTLINE "  Stop Play In Editor?");
            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextDim);
            ImGui::TextWrapped("A play session is running in '%s'. Opening '%s' will stop it.",
                CurrentName.c_str(), WorldName.c_str());
            ImGui::PopStyleColor();

            if (bSourceDirty)
            {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogDanger);
                ImGui::TextWrapped(LE_ICON_ALERT "  '%s' has unsaved changes. Cancel and save it first if you want to keep them.",
                    CurrentName.c_str());
                ImGui::PopStyleColor();
            }

            const float BtnH = 34.0f;
            const float Remaining = ImGui::GetContentRegionAvail().y - BtnH;
            if (Remaining > 0.0f)
            {
                ImGui::Dummy(ImVec2(0.0f, Remaining));
            }

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

            const float Gap  = 8.0f;
            const float BtnW = (ImGui::GetContentRegionAvail().x - Gap) * 0.5f;

            bool bShouldClose = false;

            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.22f, 0.26f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.30f, 0.34f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.18f, 0.20f, 1.00f));
            if (ImGui::Button("Cancel", ImVec2(BtnW, BtnH)))
            {
                bShouldClose = true;
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine(0.0f, Gap);

            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.52f, 0.20f, 0.21f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.66f, 0.26f, 0.27f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.44f, 0.16f, 0.17f, 1.00f));
            if (ImGui::Button(LE_ICON_STOP "  Stop && Open", ImVec2(BtnW, BtnH)))
            {
                // Deferred a frame: stopping play can destroy the extra game-preview tools through the
                // stop delegate, and tearing down editor tools from inside a BeginPopupModal scope would
                // free ImGui windows the current frame is still walking.
                MainThread::Enqueue([this, AssetGUID]()
                {
                    CWorld* NewWorld = Cast<CWorld>(CPackage::LoadAssetGraph(AssetGUID));
                    if (NewWorld == nullptr)
                    {
                        ImGuiX::Notifications::NotifyError("Failed to load world.");
                        return;
                    }

                    WorldEditorTool->StopAllSimulations();
                    WorldEditorTool->SetWorld(NewWorld);
                });

                bShouldClose = true;
            }
            ImGui::PopStyleColor(3);

            ImGui::PopStyleVar();

            return bShouldClose;
        });
    }

    void FEditorUI::OpenFileEditor(FStringView VirtualPath)
    {
        if (VirtualPath.empty())
        {
            return;
        }

        FString Key(VirtualPath.data(), VirtualPath.size());

        auto Itr = ActiveFileTools.find(Key);
        if (Itr != ActiveFileTools.end())
        {
            const char* Name = Itr->second->GetToolName().c_str();
            ImGui::SetWindowFocus(Name);
            return;
        }

        FEditorToolPtr CreatedTool = FEditorToolRegistry::Get().CreateFileEditor(this, VirtualPath);
        FEditorTool* NewTool = FinalizeNewTool(CreatedTool.release());

        if (NewTool == nullptr)
        {
            // No registered editor for this extension; fall back to OS default.
            Platform::LaunchURL(UTF8_TO_TCHAR(Key.c_str()));
            return;
        }

        RecordSessionTab(NewTool, FString(GSessionFilePrefix) + Key);
        ActiveFileTools.insert_or_assign(Move(Key), NewTool);
    }

    void FEditorUI::BrowseToAsset(FStringView VirtualPath)
    {
        if (ContentBrowser == nullptr || VirtualPath.empty())
        {
            return;
        }

        if (FFooterDrawer* Drawer = FindDrawerForTool(ContentBrowser))
        {
            ShowDrawer(*Drawer);
        }
        else
        {
            FocusTargetWindowName = ContentBrowser->GetToolName().c_str();
        }

        ContentBrowser->BrowseToAsset(VirtualPath);
    }

    const FAssetData* FEditorUI::GetContentBrowserSelectedAsset() const
    {
        return ContentBrowser != nullptr ? ContentBrowser->GetSelectedAsset() : nullptr;
    }

    void FEditorUI::OnDestroyAsset(CObject* InAsset)
    {
        if (ActiveAssetTools.find(InAsset) != ActiveAssetTools.end())
        {
            ToolsPendingDestroy.push(ActiveAssetTools.at(InAsset));
        }
    }

    FEditorTool* FEditorUI::FindToolByTypeID(uint32 TypeID) const
    {
        // Linear scan is fine, the tool list is small (< ~20 in normal use)
        // and lookups happen at menu-draw frequency, not per-frame hot paths.
        for (FEditorTool* Tool : EditorTools)
        {
            if (Tool->GetUniqueTypeID() == TypeID)
            {
                return Tool;
            }
        }
        return nullptr;
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

        if (ImGui::DockContextFindNodeByID( ImGui::GetCurrentContext(), sourceToolID))
        {
            // Build the same array with char* pointers at it is the input of DockBuilderCopyDockspace() (may change its signature?)
            ImVector<const char*> windowRemapPairs;
            namePairsBuilder.BuildPointerArray(windowRemapPairs);

            ImGui::DockBuilderCopyDockSpace(sourceToolID, destinationToolID, &windowRemapPairs);
            ImGui::DockBuilderFinish(destinationToolID);
        }
    }

    FEditorUI::FFooterDrawer* FEditorUI::FindDrawerForTool(const FEditorTool* Tool)
    {
        for (FFooterDrawer& Drawer : FooterDrawers)
        {
            if (Drawer.Tool == Tool)
            {
                return &Drawer;
            }
        }
        return nullptr;
    }

    void FEditorUI::ShowDrawer(FFooterDrawer& Drawer)
    {
        bDrawerActivatedThisFrame = true;

        // Already pinned into the layout, just focus its tab instead of opening a drawer.
        if (Drawer.bDocked)
        {
            FocusTargetWindowName = Drawer.Tool->GetToolName().c_str();
            return;
        }

        if (OpenDrawer != Drawer.Tool)
        {
            OpenDrawer = Drawer.Tool;
            DrawerOpenAmount = 0.0f;   // restart the slide
        }
    }

    void FEditorUI::ActivateDrawer(FFooterDrawer& Drawer)
    {
        // Toggle only applies to a genuinely open drawer; everything else is a plain show.
        if (!Drawer.bDocked && OpenDrawer == Drawer.Tool)
        {
            bDrawerActivatedThisFrame = true;
            OpenDrawer = nullptr;
            return;
        }

        ShowDrawer(Drawer);
    }

    void FEditorUI::DrawStatusBar(const FUpdateContext& UpdateContext)
    {
        bDrawerActivatedThisFrame = false;

        // Global drawer shortcuts (suppressed while typing into a text field).
        if (!ImGui::GetIO().WantTextInput)
        {
            for (FFooterDrawer& Drawer : FooterDrawers)
            {
                if (Drawer.Shortcut != 0 && ImGui::IsKeyChordPressed(Drawer.Shortcut))
                {
                    ActivateDrawer(Drawer);
                }
            }
        }

        const float Scale = ImGuiX::GetUIScale();
        const float BarHeight = ImGui::GetFrameHeight() + 6.0f * Scale;

        ImGuiViewport* Viewport = ImGui::GetMainViewport();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * Scale, 2.0f * Scale));
        if (ImGui::BeginViewportSideBar("##EditorStatusBar", Viewport, ImGuiDir_Down, BarHeight,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav))
        {
            for (FFooterDrawer& Drawer : FooterDrawers)
            {
                const bool bActive = (OpenDrawer == Drawer.Tool) || Drawer.bDocked;
                if (bActive)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                }

                const FFixedString Label(FFixedString::CtorSprintf(), "%s  %s", Drawer.Icon, Drawer.Label);
                // Trigger on press, not release: the drawer's auto-dismiss runs on mouse-down,
                // so a release-triggered toggle would re-open a drawer the press just closed.
                if (ImGui::ButtonEx(Label.c_str(), ImVec2(0, 0), ImGuiButtonFlags_PressedOnClick))
                {
                    ActivateDrawer(Drawer);
                }

                if (bActive)
                {
                    ImGui::PopStyleColor();
                }

                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s%s", Drawer.bDocked ? "Focus " : "Toggle ", Drawer.Label);
                }

                ImGui::SameLine();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    ImGuiID FEditorUI::GetOrCreateBottomDockID(float HeightFrac)
    {
        // Still live from an earlier dock: reuse it so the Content Browser and the Output Log end up as
        // tabs in one bottom strip, rather than each carving another slice out of the viewport.
        if (BottomDockID != 0 && ImGui::DockBuilderGetNode(BottomDockID) != nullptr)
        {
            return BottomDockID;
        }

        ImGuiDockNode* Root = ImGui::DockBuilderGetNode(MainDockspaceID);
        if (Root == nullptr)
        {
            // No layout to split yet. Tabbing into the root is wrong, but it is what this did before and
            // it keeps the panel reachable instead of dropping it on the floor.
            return MainDockspaceID;
        }

        // A restored imgui.ini brings the split back without the id we cached, so adopt an existing bottom
        // strip before making a second one. Only the simple case is claimed -- a root split along Y, whose
        // lower child IS that strip. Anything the user rearranged by hand gets a fresh split, because at
        // that point there is no way to tell which node they consider "the bottom".
        if (Root->IsSplitNode() && Root->SplitAxis == ImGuiAxis_Y && Root->ChildNodes[1] != nullptr)
        {
            BottomDockID = Root->ChildNodes[1]->ID;
            return BottomDockID;
        }

        // Seeded from the height the drawer was at, so pinning it in place does not also resize it.
        const float Ratio = Math::Clamp(HeightFrac > 0.0f ? HeightFrac : 0.3f, 0.15f, 0.6f);

        ImGuiID TopID = 0;
        ImGui::DockBuilderSplitNode(MainDockspaceID, ImGuiDir_Down, Ratio, &BottomDockID, &TopID);
        ImGui::DockBuilderFinish(MainDockspaceID);

        // Whatever was in the root (the world editor) rides along into the top child: DockBuilderSplitNode
        // hands the existing windows to the inheritor, which for a Down split is the upper node.
        return BottomDockID;
    }

    void FEditorUI::DrawFooterDrawer(const FUpdateContext& UpdateContext)
    {
        if (OpenDrawer == nullptr)
        {
            DrawerOpenAmount = 0.0f;
            ImGuiX::Notifications::SetBottomInset(0.0f);
            return;
        }

        // Ease the slide toward fully open.
        DrawerOpenAmount = eastl::min(1.0f, DrawerOpenAmount + static_cast<float>(UpdateContext.GetDeltaTime()) * 8.0f);
        const float Eased = DrawerOpenAmount * DrawerOpenAmount * (3.0f - 2.0f * DrawerOpenAmount); // smoothstep

        const float Scale = ImGuiX::GetUIScale();
        ImGuiViewport* Viewport = ImGui::GetMainViewport();
        FFooterDrawer* Drawer = FindDrawerForTool(OpenDrawer);

        // WorkPos/WorkSize already exclude the title bar and status bar, so the bottom of
        // the work area sits just above the status bar, exactly where the drawer anchors.
        const float MaxHeight = Viewport->WorkSize.y * (Drawer ? Drawer->HeightFrac : 0.4f);
        const float Height    = MaxHeight * Eased;

        // Lift notification toasts above the drawer as it slides open.
        ImGuiX::Notifications::SetBottomInset(Height);
        const ImVec2 DrawerPos(Viewport->WorkPos.x, Viewport->WorkPos.y + Viewport->WorkSize.y - Height);
        const ImVec2 DrawerSize(Viewport->WorkSize.x, Height);

        ImGui::SetNextWindowPos(DrawerPos);
        ImGui::SetNextWindowSize(DrawerSize);
        ImGui::SetNextWindowViewport(Viewport->ID);
        ImGui::SetNextWindowBgAlpha(1.0f);
        if (bDrawerActivatedThisFrame)
        {
            ImGui::SetNextWindowFocus();
        }

        constexpr ImGuiWindowFlags Flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::Begin("##EditorFooterDrawer", nullptr, Flags);

        const bool bFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        // Top-edge resize handle: drawer is bottom-anchored, so dragging up grows it.
        // Only active once fully open so it doesn't fight the slide-in animation.
        if (Drawer != nullptr && DrawerOpenAmount >= 1.0f)
        {
            const float HandleH = 5.0f * Scale;
            ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
            ImGui::InvisibleButton("##DrawerResize", ImVec2(eastl::max(ImGui::GetWindowWidth(), 1.0f), HandleH));
            const bool bHandleHovered = ImGui::IsItemHovered();
            const bool bHandleActive  = ImGui::IsItemActive();
            if (bHandleHovered || bHandleActive)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }
            if (bHandleActive)
            {
                const float DeltaFrac = -ImGui::GetIO().MouseDelta.y / eastl::max(1.0f, Viewport->WorkSize.y);
                Drawer->HeightFrac = eastl::clamp(Drawer->HeightFrac + DeltaFrac, 0.15f, 0.85f);
            }

            const ImVec2 WinMin = ImGui::GetWindowPos();
            const ImU32 HandleCol = ImGui::GetColorU32(bHandleActive ? ImGuiCol_SeparatorActive
                : bHandleHovered ? ImGuiCol_SeparatorHovered : ImGuiCol_Border);
            ImGui::GetWindowDrawList()->AddLine(WinMin, ImVec2(WinMin.x + ImGui::GetWindowWidth(), WinMin.y), HandleCol, 2.0f);
        }
        
        const ImGuiStyle& Style = ImGui::GetStyle();
        const float ToolbarHeight = ImGui::GetFrameHeight() + Style.FramePadding.y * 2.0f;

        const char* DockLabel = LE_ICON_DOCK_BOTTOM " Dock";
        const float DockWidth    = ImGui::CalcTextSize(DockLabel).x + Style.FramePadding.x * 2.0f;
        const float CloseWidth   = ImGui::CalcTextSize(LE_ICON_CLOSE).x + Style.FramePadding.x * 2.0f;
        const float ButtonsWidth = DockWidth + CloseWidth + Style.ItemSpacing.x * 3.0f;

        const float ToolbarWidth = eastl::max(ImGui::GetContentRegionAvail().x - ButtonsWidth, 1.0f);
        if (ImGui::BeginChild("##DrawerToolbar", ImVec2(ToolbarWidth, ToolbarHeight), false, ImGuiWindowFlags_MenuBar))
        {
            if (ImGui::BeginMenuBar())
            {
                if (Drawer != nullptr)
                {
                    ImGui::TextUnformatted(Drawer->Icon);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                }

                if (OpenDrawer != nullptr)
                {
                    OpenDrawer->DrawMainToolbar(UpdateContext);
                }

                ImGui::EndMenuBar();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, 0.0f);
        if (ImGui::BeginChild("##DrawerToolbarButtons", ImVec2(ButtonsWidth, ToolbarHeight), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));

            if (ImGui::Button(DockLabel) && Drawer != nullptr)
            {
                Drawer->bDocked = true;

                // Queued, not assigned: the bottom node this wants may not exist yet, and it can only be
                // split off before DockSpace() is submitted. See PendingBottomDockTool.
                PendingBottomDockTool       = Drawer->Tool;
                PendingBottomDockHeightFrac = Drawer->HeightFrac;

                FocusTargetWindowName = Drawer->Tool->GetToolName().c_str();
                OpenDrawer = nullptr;
            }
            ImGuiX::TextTooltip("Dock this panel to the bottom of the main layout");

            ImGui::SameLine();
            if (ImGui::Button(LE_ICON_CLOSE))
            {
                OpenDrawer = nullptr;
            }
            ImGuiX::TextTooltip("Close the drawer");

            ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        if (OpenDrawer != nullptr)
        {
            OpenDrawer->DrawDrawerContent(bFocused);
        }

        // Captured before End(), while the drawer is still the current window.
        const ImVec2 DrawerMin = ImGui::GetWindowPos();
        const ImVec2 DrawerMax = ImVec2(DrawerMin.x + ImGui::GetWindowWidth(), DrawerMin.y + ImGui::GetWindowHeight());

        ImGui::End();
        ImGui::PopStyleVar();

        // Auto-dismiss on click-outside, unless a footer button/shortcut toggled it this
        // frame or one of the drawer's own popups is open (those read as a separate window).
        //
        // The geometric test is not redundant with bFocused. Anything that calls SetWindowFocus elsewhere
        // in the frame leaves the drawer unfocused while the cursor is still over it, and then the user's
        // very next click -- on the drawer -- read as a click-outside and closed it.
        const bool bPopupOpen   = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
        const bool bMouseInside = ImGui::IsMouseHoveringRect(DrawerMin, DrawerMax, false);

        if (!bFocused && !bMouseInside && !bPopupOpen && !bDrawerActivatedThisFrame
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            OpenDrawer = nullptr;
            DrawerOpenAmount = 0.0f;
        }
    }

    bool FEditorUI::SubmitToolMainWindow(const FUpdateContext& UpdateContext, FEditorTool* EditorTool, ImGuiID TopLevelDockspaceID)
    {
        LUMINA_PROFILE_SCOPE();
        ASSERT(EditorTool != nullptr);
        ASSERT(TopLevelDockspaceID != 0);

        bool bIsToolStillOpen = true;
        // The world editor can never be closed. Drawer tools (Content Browser / Output
        // Log) are closable while docked, closing sends them back to the footer drawer.
        bool* bIsToolOpen = (EditorTool == WorldEditorTool) ? nullptr : &bIsToolStillOpen;
        
        // Top level editors can only be docked with each others
        ImGui::SetNextWindowClass(&EditorWindowClass);
        if (EditorTool->GetDesiredDockID() != 0)
        {
            ImGui::SetNextWindowDockID(EditorTool->GetDesiredDockID());
            EditorTool->DesiredDockID = 0;
        }
        else if (EditorTool->ShouldOpenDocked())
        {
            ImGui::SetNextWindowDockID(TopLevelDockspaceID, ImGuiCond_FirstUseEver);
        }
        else if (!EditorTool->bInitialDockApplied)
        {
            // Force floating on first appearance, overriding any dock state restored from imgui.ini by window
            // name. Applied once so the user can still dock the window manually afterwards.
            ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        }

        EditorTool->bInitialDockApplied = true;

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

        // On undock, inherit bounds are often a sliver; reset to a sensible floating size for one frame.
        // CurrDockID is last frame's value; CurrentWindow->DockId is the upcoming frame's assignment.
        const ImGuiID PrevFrameDockID = EditorTool->CurrDockID;
        const ImGuiID NextFrameDockID = CurrentWindow ? CurrentWindow->DockId : 0;
        if (PrevFrameDockID != 0 && NextFrameDockID == 0 && CurrentWindow != nullptr)
        {
            constexpr ImVec2 UndockedSize(1177.6f, 883.2f);
            const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
            const ImVec2 UndockedPos
            {
                MainViewport->Pos.x + (MainViewport->Size.x - UndockedSize.x) * 0.5f,
                MainViewport->Pos.y + (MainViewport->Size.y - UndockedSize.y) * 0.5f,
            };
            ImGui::SetNextWindowSize(UndockedSize, ImGuiCond_Always);
            ImGui::SetNextWindowPos(UndockedPos, ImGuiCond_Always);
        }

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
        EditorTool->ToolWindowsClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoTaskBarIcon | ImGuiViewportFlags_NoDecoration | ImGuiViewportFlags_TopMost;
        EditorTool->ToolWindowsClass.ParentViewportId = ImGui::GetWindowViewport()->ID;
        EditorTool->ToolWindowsClass.DockingAllowUnclassed = true;

        // Track LocationID change so we can fork/copy the layout data according to where the window is going + reference count
        // LocationID ~~ (DockId != 0 ? DockId : DocumentID) // When we are in a loose floating window we use our own document id instead of the dock id
        EditorTool->CurrDockID = ImGui::GetWindowDockID();
        EditorTool->PrevLocationID = EditorTool->CurrLocationID;
        EditorTool->CurrLocationID = EditorTool->CurrDockID != 0 ? EditorTool->CurrDockID : EditorTool->GetID();

        // Dockspace ID ~~ Hash of LocationID + DocType, so same-type editors in a tab bar
        // share a layout; also used as a window-title suffix.
        EditorTool->PrevDockspaceID = EditorTool->CurrDockspaceID;
        EditorTool->CurrDockspaceID = EditorTool->CalculateDockspaceID();
        ASSERT(EditorTool->CurrDockspaceID != 0);

        DrawToolTabContextMenu(EditorTool);

        ImGui::End();

        return bIsToolStillOpen;
    }

    FEditorTool* FEditorUI::FindToolByWindowName(const char* WindowName) const
    {
        if (WindowName == nullptr)
        {
            return nullptr;
        }

        for (FEditorTool* Tool : EditorTools)
        {
            if (Tool->GetToolName() == WindowName)
            {
                return Tool;
            }
        }

        return nullptr;
    }

    bool FEditorUI::CanCloseTool(const FEditorTool* Tool) const
    {
        // Matches SubmitToolMainWindow's rule for whether the tab even gets an X button.
        return Tool != nullptr && Tool != WorldEditorTool;
    }

    void FEditorUI::RequestCloseTool(FEditorTool* Tool)
    {
        if (!CanCloseTool(Tool))
        {
            return;
        }

        // A docked drawer tool goes back to its footer drawer rather than being destroyed, same as when
        // its own X is pressed.
        if (FFooterDrawer* Drawer = FindDrawerForTool(Tool); Drawer != nullptr)
        {
            Drawer->bDocked = false;
            return;
        }

        ToolsPendingDestroy.push(Tool);
    }

    void FEditorUI::DrawToolTabContextMenu(FEditorTool* EditorTool)
    {
        ImGuiWindow*   Window = ImGui::GetCurrentWindow();
        ImGuiDockNode* Node   = Window->DockNode;

        // Hit-tested by hand instead of with BeginPopupContextItem. A docked window whose tab is not the
        // selected one has SkipItems set, and BeginPopupContextItem early-outs on that -- so only the
        // frontmost tab would ever open a menu, which is not how tab strips behave anywhere else. The tab
        // rect itself is filled in for every tab by the dockspace earlier this frame.
        if (Window->DockIsActive && Node != nullptr && Node->HostWindow != nullptr)
        {
            const ImGuiContext& G = *ImGui::GetCurrentContext();

            // Gated on HoveredWindow as well as the rect: the rect alone would fire through a floating
            // window parked over the tab bar.
            const bool bOverTab = G.HoveredWindow == Node->HostWindow
                && Window->DC.DockTabItemRect.Contains(G.IO.MousePos);

            if (bOverTab && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
            {
                ImGui::OpenPopup("##ToolTabContext");
            }
        }

        if (!ImGui::BeginPopup("##ToolTabContext"))
        {
            return;
        }

        // Left-to-right as the user sees them. Node->Windows is explicitly an unordered list; TabBar->Tabs
        // is the display order and is what ImGui rewrites when a tab is dragged.
        TVector<FEditorTool*> Order;
        int32 SelfIndex = -1;

        if (Node != nullptr && Node->TabBar != nullptr)
        {
            for (const ImGuiTabItem& Tab : Node->TabBar->Tabs)
            {
                if (Tab.Window == nullptr)
                {
                    continue;
                }

                if (FEditorTool* Tool = FindToolByWindowName(Tab.Window->Name))
                {
                    if (Tool == EditorTool)
                    {
                        SelfIndex = (int32)Order.size();
                    }
                    Order.push_back(Tool);
                }
            }
        }

        const int32 Count = (int32)Order.size();

        auto AnyClosableIn = [this, &Order, Count](int32 First, int32 Last, int32 Skip) -> bool
        {
            for (int32 i = eastl::max(0, First); i <= Last && i < Count; ++i)
            {
                if (i != Skip && CanCloseTool(Order[i]))
                {
                    return true;
                }
            }
            return false;
        };

        auto CloseRange = [this, &Order, Count](int32 First, int32 Last, int32 Skip)
        {
            for (int32 i = eastl::max(0, First); i <= Last && i < Count; ++i)
            {
                if (i != Skip)
                {
                    RequestCloseTool(Order[i]);
                }
            }
        };

        if (ImGui::MenuItem(LE_ICON_CLOSE " Close", nullptr, false, CanCloseTool(EditorTool)))
        {
            RequestCloseTool(EditorTool);
        }

        if (ImGui::MenuItem("Close Others", nullptr, false, SelfIndex >= 0 && AnyClosableIn(0, Count - 1, SelfIndex)))
        {
            CloseRange(0, Count - 1, SelfIndex);
        }

        if (ImGui::MenuItem("Close to the Right", nullptr, false, SelfIndex >= 0 && AnyClosableIn(SelfIndex + 1, Count - 1, -1)))
        {
            CloseRange(SelfIndex + 1, Count - 1, -1);
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Close All", nullptr, false, AnyClosableIn(0, Count - 1, -1)))
        {
            CloseRange(0, Count - 1, -1);
        }

        ImGui::EndPopup();
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

            // Move the window but keep the destination layout when another same-type tool already
            // shares it (CurrDockspaceRefCount counts ourselves, so > 1 means at least one other).
            // Our identically-named sub-windows map straight onto that shared layout; copying would
            // clobber it. Only fork our layout into a destination that is otherwise empty.
            if (CurrDockspaceRefCount <= 1)
            {
                EditorToolLayoutCopy(Tool);
            }

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

        // Ctrl+S routes to the focused tool only, checking IsKeyPressed inside
        // each tool's Update fires for every open tool simultaneously.
        // Shift excluded: Ctrl+Shift+S is Save All, handled once in OnUpdate.
        if (bIsLastFocusedTool && ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && !ImGui::GetIO().KeyShift
            && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            Tool->OnSave();
        }

        Tool->bIsActiveTool = bIsLastFocusedTool;

        Tool->Update(UpdateContext);

        Tool->bIsActiveTool    = false;
        Tool->bViewportFocused = false;
        Tool->bViewportHovered = false;

        if (Tool->HasWorld())
        {
            // A server world must keep simulating even while its tool is hidden, otherwise tabbing onto a
            // client's Game Preview freezes every player. Not gated on client count: a suspended server never
            // ticks its transport, so it could never accept the first connection -- which deadlocks when a
            // client preview spawns already docked over the server's tool and hides it on frame one.
            CWorld* ToolWorld = Tool->GetWorld();
            const bool bKeepAlive = bVisible || ToolWorld->IsNetServer();
            ToolWorld->SetActive(bKeepAlive);

            ToolWorld->SetUpdateInterval(GetToolWorldUpdateInterval(ToolWorld, bIsLastFocusedTool));
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

                    if (Tool->IsViewportFullscreen())
                    {
                        // Begin/End empty to keep the dock-node slot alive for when fullscreen exits.
                        ImGui::SetNextWindowClass(&Tool->ToolWindowsClass);
                        ImGui::SetNextWindowSizeConstraints(ImVec2(128, 128), ImVec2(FLT_MAX, FLT_MAX));
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                        ImGui::Begin(ToolWindowName.c_str(), nullptr, ViewportWindowFlags);
                        ImGui::PopStyleVar();
                        ImGui::End();

                        // Now draw the actual viewport into a separate fullscreen overlay window.
                        // Different name so the docked window's position/dock state is untouched.
                        const FFixedString FullscreenName(FFixedString::CtorSprintf(), "%s##Fullscreen_%08X", FEditorTool::ViewportWindowName, Tool->GetCurrentDockspaceID());

                        const ImGuiViewport* MainVP = ImGui::GetMainViewport();
                        ImGui::SetNextWindowPos(MainVP->WorkPos);
                        ImGui::SetNextWindowSize(MainVP->WorkSize);
                        ImGui::SetNextWindowViewport(MainVP->ID);

                        constexpr ImGuiWindowFlags FullscreenFlags =
                            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoNavInputs |
                            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoScrollWithMouse;

                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
                        bool const DrawViewportWindow = ImGui::Begin(FullscreenName.c_str(), nullptr, FullscreenFlags);
                        ImGui::PopStyleVar(3);

                        if (DrawViewportWindow)
                        {
                            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

                            IRenderScene* SceneRenderer = Tool->GetWorld()->GetRenderer();

                            // Size the scene to the panel that displays it, not the window. ImGui works in
                            // physical pixels here (Io.DisplaySize is the swapchain extent), so the content
                            // region is already the right unit. Floor matches DrawViewport's.
                            const ImVec2 ViewportAvail = ImGui::GetContentRegionAvail();
                            SceneRenderer->SetPrimaryViewSize(FUIntVector2(
                                (uint32)eastl::max(ViewportAvail.x, 64.0f),
                                (uint32)eastl::max(ViewportAvail.y, 64.0f)));

                            ImTextureRef ViewportTexture = ImGuiX::ToImTextureRef(SceneRenderer->GetDisplayResourceID());

                            Tool->bViewportFocused = ImGui::IsWindowFocused();
                            Tool->bViewportHovered = ImGui::IsWindowHovered();
                            Tool->UpdateViewportInput(UpdateContext);
                            Tool->DrawViewport(UpdateContext, ViewportTexture);
                        }

                        ImGui::End();
                    }
                    else
                    {
                        ImGui::SetNextWindowClass(&Tool->ToolWindowsClass);
                        ImGui::SetNextWindowSizeConstraints(ImVec2(128, 128), ImVec2(FLT_MAX, FLT_MAX));
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                        bool const DrawViewportWindow = ImGui::Begin(ToolWindowName.c_str(), nullptr, ViewportWindowFlags);
                        ImGui::PopStyleVar();

                        if (DrawViewportWindow)
                        {
                            IRenderScene* SceneRenderer = Tool->GetWorld()->GetRenderer();

                            // Size the scene to the panel that displays it, not the window. ImGui works in
                            // physical pixels here (Io.DisplaySize is the swapchain extent), so the content
                            // region is already the right unit. Floor matches DrawViewport's.
                            const ImVec2 ViewportAvail = ImGui::GetContentRegionAvail();
                            SceneRenderer->SetPrimaryViewSize(FUIntVector2(
                                (uint32)eastl::max(ViewportAvail.x, 64.0f),
                                (uint32)eastl::max(ViewportAvail.y, 64.0f)));

                            ImTextureRef ViewportTexture = ImGuiX::ToImTextureRef(SceneRenderer->GetDisplayResourceID());

                            Tool->bViewportFocused = ImGui::IsWindowFocused();
                            Tool->bViewportHovered = ImGui::IsWindowHovered();
                            Tool->UpdateViewportInput(UpdateContext);
                            Tool->DrawViewport(UpdateContext, ViewportTexture);
                        }

                        ImGui::End();
                    }
                }
                else
                {
                    LUMINA_PROFILE_SECTION("Draw Tool Window");

                    ImGuiWindowFlags ToolWindowFlags = ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoCollapse;

                    if (Window->bDisableScrolling)
                    {
                        ToolWindowFlags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
                    }
                    
                    ImGui::SetNextWindowClass(&Tool->ToolWindowsClass);
                    // Floor the size so a panel can't be dragged to a degenerate extent (child widgets assert at zero width).
                    ImGui::SetNextWindowSizeConstraints(ImVec2(100.0f, 80.0f), ImVec2(FLT_MAX, FLT_MAX));

                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().WindowPadding);
                    bool const DrawToolWindow = ImGui::Begin(ToolWindowName.c_str(), nullptr, ToolWindowFlags);
                    ImGui::PopStyleVar();

                    if (DrawToolWindow)
                    {
                        LUMINA_PROFILE_TAG(Window->Name.c_str());
                        const bool bToolWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_DockHierarchy);
                        Window->DrawFunction(bToolWindowFocused);
                    }
                    
                    ImGui::End();
                }
            }
        }
    }

    void FEditorUI::CreateGameViewportTool()
    {
        // Player 1 runs in the main world-editor viewport; players 2..N each get a Game Preview pop-up.
        // CreateTool defers the actual add (ToolsPendingAdd), so this is safe to call mid-draw.
        if (WorldEditorTool == nullptr)
        {
            return;
        }

        CWorld* Source = WorldEditorTool->GetPIESourceWorld();
        if (Source == nullptr)
        {
            return;
        }

        const int32 NumPlayers = WorldEditorTool->GetPIEPlayerCount();
        for (int32 PlayerIndex = 1; PlayerIndex < NumPlayers; ++PlayerIndex)
        {
            // Each call duplicates the editor source world -> an independent, self-contained PIE world.
            CWorld* PreviewWorld = GWorldManager->StartPIE(Source, EWorldType::Game, WorldEditorTool->ResolvePlayerNetMode(PlayerIndex));
            if (PreviewWorld == nullptr)
            {
                LOG_WARN("Failed to start PIE world for player {}", PlayerIndex + 1);
                continue;
            }

            // The tool owns the world: FEditorTool::Deinitialize calls DestroyWorldContext on teardown.
            // PlayerIndex is the client number (player 1 is the main viewport), so previews are "Client: 1"..N.
            FGamePreviewTool* Tool = CreateTool<FGamePreviewTool>(this, PreviewWorld, PlayerIndex);
            ExtraGamePreviews.push_back(FExtraGamePreview{ PreviewWorld, Tool });
        }
    }

    void FEditorUI::DestroyGameViewportTool()
    {
        // Schedule each preview tool for destruction; the tool tears down its own PIE world on Deinitialize.
        // DestroyTool also erases the matching ExtraGamePreviews entry, so just clear our list here.
        for (const FExtraGamePreview& Preview : ExtraGamePreviews)
        {
            if (Preview.Tool != nullptr)
            {
                ToolsPendingDestroy.push(Preview.Tool);
            }
        }
        ExtraGamePreviews.clear();
    }

    void FEditorUI::HandleUserInput(const FUpdateContext& UpdateContext)
    {
        
    }

    void FEditorUI::OpenAssetSearchModal()
    {
        // Snapshotted at open, not re-queried per frame: the list cannot change under the user while a
        // modal is up, and re-walking the registry on every keystroke would be pure waste.
        struct FEntry
        {
            FGuid        GUID;
            FFixedString Name;
            FFixedString Path;
            // Thumbnail lookup key. Precomputed because the alternative is interning the path string on
            // every visible row on every frame the modal is up.
            FName        PathName;
        };

        TVector<FEntry> Entries;
        for (const FAssetData* Asset : FAssetRegistry::Get().FindByPredicate([](const FAssetData&) { return true; }))
        {
            Entries.push_back(FEntry{ Asset->AssetGUID, FFixedString(Asset->AssetName.c_str()), Asset->Path,
                                      FName(Asset->Path.c_str()) });
        }

        if (Entries.empty())
        {
            ImGuiX::Notifications::NotifyWarning("No assets to open.");
            return;
        }

        eastl::sort(Entries.begin(), Entries.end(), [](const FEntry& A, const FEntry& B)
        {
            return strcmp(A.Name.c_str(), B.Name.c_str()) < 0;
        });

        PushModal("Open Asset", ImVec2(720.0f, 520.0f),
            [this, Entries = Move(Entries), Filter = ImGuiTextFilter(), Selected = 0, bFocusPending = true]() mutable -> bool
        {
            // Focus the box on the first frame so the modal is type-to-search with no click.
            if (bFocusPending)
            {
                ImGui::SetKeyboardFocusHere();
                bFocusPending = false;
            }

            ImGui::SetNextItemWidth(-1.0f);
            Filter.Draw("##AssetSearch");
            if (!Filter.IsActive())
            {
                const ImVec2 TextPos = ImGui::GetItemRectMin() + ImGui::GetStyle().FramePadding + ImVec2(2.0f, 0.0f);
                ImGui::GetWindowDrawList()->AddText(TextPos, EditorColors::U32(EditorColors::TextMuted()),
                    LE_ICON_FILE_SEARCH " Search assets...");
            }

            // Rebuilt each frame: the filter changes with every keystroke, and it is a substring test
            // over a few thousand short strings.
            TVector<const FEntry*> Matches;
            Matches.reserve(Entries.size());
            for (const FEntry& Entry : Entries)
            {
                if (ImGuiX::PassSearchFilter(Filter, Entry.Name.c_str()) || ImGuiX::PassSearchFilter(Filter, Entry.Path.c_str()))
                {
                    Matches.push_back(&Entry);
                }
            }

            if (Matches.empty())
            {
                Selected = 0;
            }
            else
            {
                Selected = Math::Clamp(Selected, 0, (int32)Matches.size() - 1);
            }

            // Arrows move the selection while focus stays in the text box, so the whole flow is
            // keyboard-only: type, arrow, Enter.
            bool bScrollToSelected = false;
            if (!Matches.empty())
            {
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
                {
                    Selected = (Selected + 1) % (int32)Matches.size();
                    bScrollToSelected = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
                {
                    Selected = (Selected + (int32)Matches.size() - 1) % (int32)Matches.size();
                    bScrollToSelected = true;
                }
            }

            bool bChosen = !Matches.empty() && ImGui::IsKeyPressed(ImGuiKey_Enter, false);

            ImGui::Separator();

            const float Scale     = ImGuiX::GetUIScale();
            const float ThumbSize = 32.0f * Scale;
            const float RowPad    = 6.0f * Scale;
            const float RowHeight = ThumbSize + RowPad * 2.0f;

            static const FString GenericAssetIcon = Paths::GetEngineResourceDirectory() + "/Textures/Asset.png";

            const float FooterHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
            if (ImGui::BeginChild("##Results", ImVec2(0.0f, -FooterHeight)))
            {
                // Clipped, because asking for a thumbnail is what QUEUES it: walking every match would
                // hand the thumbnail renderer the entire project the moment the modal opens.
                ImGuiListClipper Clipper;
                Clipper.Begin((int32)Matches.size(), RowHeight);
                if (!Matches.empty())
                {
                    // The selection can sit outside the visible range after an arrow keypress, and it has
                    // to be submitted for SetScrollHereY to have an item to scroll to.
                    Clipper.IncludeItemByIndex(Selected);
                }

                while (Clipper.Step())
                {
                    for (int32 i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                    {
                        const FEntry& Entry = *Matches[i];
                        ImGui::PushID(i);

                        const ImVec2 RowMin = ImGui::GetCursorScreenPos();

                        if (ImGui::Selectable("##Row", i == Selected, ImGuiSelectableFlags_AllowDoubleClick,
                                              ImVec2(0.0f, RowHeight)))
                        {
                            Selected = i;
                            bChosen  = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                        }

                        if (i == Selected && bScrollToSelected)
                        {
                            ImGui::SetScrollHereY(0.5f);
                        }

                        // Painted onto the row rather than laid out with SameLine: the row is a single
                        // full-height Selectable, and real widgets on top of it would advance the cursor
                        // and take the hover off it.
                        ImDrawList* DrawList = ImGui::GetWindowDrawList();

                        // A miss starts the async resolve and draws the generic icon until it lands, the
                        // same fallback the content browser tiles use.
                        ImTextureRef Thumbnail = ImGuiX::ToImTextureRef(GenericAssetIcon);
                        if (FPackageThumbnail* Ready = CThumbnailManager::Get().GetThumbnailForPackage(Entry.PathName))
                        {
                            Thumbnail = ImGuiX::ToImTextureRef(Ready->LoadedImage);
                        }

                        const ImVec2 ThumbMin(RowMin.x + RowPad, RowMin.y + RowPad);
                        DrawList->AddImage(Thumbnail, ThumbMin, ImVec2(ThumbMin.x + ThumbSize, ThumbMin.y + ThumbSize));

                        const float LineHeight = ImGui::GetTextLineHeight();
                        const float TextX      = ThumbMin.x + ThumbSize + RowPad * 2.0f;
                        const float TextTop    = RowMin.y + (RowHeight - LineHeight * 2.0f) * 0.5f;

                        DrawList->AddText(ImVec2(TextX, TextTop),
                                          EditorColors::U32(EditorColors::TextPrimary()), Entry.Name.c_str());
                        DrawList->AddText(ImVec2(TextX, TextTop + LineHeight),
                                          EditorColors::U32(EditorColors::TextMuted()), Entry.Path.c_str());

                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();

            ImGuiX::Text("{} asset(s)", (uint32)Matches.size());
            ImGui::SameLine();
            ImGui::TextColored(EditorColors::TextMuted(), "  Enter to open, Esc to cancel");

            if (bChosen)
            {
                OpenAssetEditor(Matches[Selected]->GUID);
                return true;
            }

            return ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        });
    }

    void FEditorUI::LaunchTracyProfiler()
    {
        const FString& EngineRoot = Paths::GetEngineInstallDirectory();
        if (EngineRoot.empty())
        {
            ImGuiX::Notifications::NotifyError("Cannot locate Tracy: the engine install directory is unresolved.");
            return;
        }

        const FString FullPath = EngineRoot + "/External/Tracy/tracy-profiler.exe";

        // Checked before launching so a missing tool names the path it looked at. ShellExecute on a
        // non-existent exe reports nothing useful, which is how this managed to look like it worked.
        std::error_code Ec;
        if (!std::filesystem::exists(FullPath.c_str(), Ec))
        {
            LOG_ERROR("Tracy profiler not found at '{}'.", FullPath.c_str());
            ImGuiX::Notifications::NotifyError("Tracy not found at {0}", FullPath);
            return;
        }

        // LaunchProcess, not LaunchURL: this is an executable, and it returns the system error so a failed
        // launch can say why instead of silently doing nothing.
        const int Result = Platform::LaunchProcess(UTF8_TO_TCHAR(FullPath.c_str()));
        if (Result != 0)
        {
            LOG_ERROR("Failed to launch Tracy at '{}' (system error {}).", FullPath.c_str(), Result);
            ImGuiX::Notifications::NotifyError("Failed to launch Tracy (system error {0}).", Result);
            return;
        }

        ImGuiX::Notifications::NotifySuccess("Tracy profiler launched.");
    }

    void FEditorUI::SaveActiveTool()
    {
        if (LastActiveTool == nullptr)
        {
            ImGuiX::Notifications::NotifyWarning("Nothing to save: no editor is focused.");
            return;
        }

        // The tool's own OnSave reports success or failure, so no notification here -- a second one
        // would just duplicate it.
        LastActiveTool->OnSave();
    }

    void FEditorUI::SaveAllDirtyPackages()
    {
        TVector<CPackage*> DirtyPackages;
        for (TObjectIterator<CPackage> Itr; Itr; ++Itr)
        {
            CPackage* Package = *Itr;
            // Marked for destroy = a deleted asset awaiting the deferred drain; nothing to write.
            if (Package->HasAnyFlag(OF_MarkedDestroy) || !Package->IsDirty())
            {
                continue;
            }
            DirtyPackages.push_back(Package);
        }

        if (DirtyPackages.empty())
        {
            ImGuiX::Notifications::NotifySuccess("Nothing to save; everything is up to date.");
            return;
        }

        // Route through the owning tool where one is open: its OnSave carries type-specific work a raw
        // package write skips -- thumbnail capture, asset-registry notification, material compile-on-save.
        // Those tools report their own result, so only the packages saved directly are counted here.
        THashSet<CPackage*> HandledByTool;
        for (const auto& [Asset, Tool] : ActiveAssetTools)
        {
            if (Asset == nullptr || Tool == nullptr)
            {
                continue;
            }

            CPackage* Package = Asset->GetPackage();
            if (Package != nullptr && Package->IsDirty() && !Package->HasAnyFlag(OF_MarkedDestroy))
            {
                Tool->OnSave();
                HandledByTool.insert(Package);
            }
        }

        uint32 Saved  = 0;
        uint32 Failed = 0;
        for (CPackage* Package : DirtyPackages)
        {
            if (HandledByTool.find(Package) != HandledByTool.end())
            {
                continue;
            }

            if (CPackage::SavePackage(Package, Package->GetPackagePath()))
            {
                ++Saved;
            }
            else
            {
                ++Failed;
            }
        }

        if (Failed > 0)
        {
            ImGuiX::Notifications::NotifyError("Save All: {0} package(s) saved, {1} failed.", Saved, Failed);
        }
        else if (Saved > 0)
        {
            ImGuiX::Notifications::NotifySuccess("Save All: {0} package(s) saved.", Saved);
        }
        // Saved == 0 with no failures means every dirty package belonged to an open tool, and each of
        // those already reported itself.
    }

    void FEditorUI::VerifyDirtyPackages()
    {
        TVector<CPackage*> DirtyPackages;
        DirtyPackages.reserve(4);
        for (TObjectIterator<CPackage> Itr; Itr; ++Itr)
        {
            CPackage* Package = *Itr;

            // A package marked for destruction (a deleted asset awaiting the deferred drain) has nothing to
            // save; never prompt for it.
            if (Package->HasAnyFlag(OF_MarkedDestroy))
            {
                continue;
            }

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
        
        ModalManager.CreateDialogue("Unsaved Changes", ImVec2(620, 540),
            [this, Packages = Move(DirtyPackages), Selection = Move(PackageSelection), States = Move(SaveStates)]() mutable
        {
            // Hero header: matches the Open/New Project dialog opener so the
            // shutdown prompt reads as part of the same family.
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::MediumBold);
            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogAccentGold);
            ImGui::TextUnformatted(LE_ICON_ALERT_CIRCLE_OUTLINE "  Unsaved Changes");
            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();

            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextDim);
            ImGui::TextWrapped("%d package%s ha%s pending edits. Choose what to do before the editor closes.",
                (int32)Packages.size(),
                Packages.size() == 1 ? "" : "s",
                Packages.size() == 1 ? "s" : "ve");
            ImGui::PopStyleColor();

            DrawSectionHeader("PACKAGES");

            // Selection toolbar (compact, palette-aligned).
            int32 SelectedCount = 0;
            for (bool S : Selection) { if (S) ++SelectedCount; }

            ImGui::PushStyleColor(ImGuiCol_Button,        kProjDialogRowBg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kProjDialogRowBgHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kProjDialogRowBgActive);
            if (ImGui::SmallButton(LE_ICON_CHECKBOX_MULTIPLE_OUTLINE " All"))
            {
                for (bool& S : Selection) S = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(LE_ICON_CHECKBOX_BLANK_OUTLINE " None"))
            {
                for (bool& S : Selection) S = false;
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextDim);
            ImGui::Text("%d of %d selected", SelectedCount, (int32)Packages.size());
            ImGui::PopStyleColor();

            ImGui::Spacing();

            // Package list: rows mimic DrawProjectRow (accent bar + hover) plus a
            // leading checkbox and trailing status badge, sharing kProjDialog colors.
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kProjDialogPanelBg);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            if (ImGui::BeginChild("##PackagesBody", ImVec2(0, -68), true, ImGuiWindowFlags_AlwaysVerticalScrollbar))
            {
                for (size_t i = 0; i < Packages.size(); ++i)
                {
                    CPackage*  Package = Packages[i];
                    const bool bSaved  = States[i] == ESaveState::Success;
                    const bool bFailed = States[i] == ESaveState::Failed;

                    const ImVec4 Accent =
                        bFailed ? kProjDialogDanger    :
                        bSaved  ? kProjDialogAccentSoft :
                                  kProjDialogAccentGold;

                    const float Avail   = ImGui::GetContentRegionAvail().x;
                    const float Height  = 50.0f;
                    const ImVec2 P0     = ImGui::GetCursorScreenPos();
                    const ImVec2 P1     = ImVec2(P0.x + Avail, P0.y + Height);

                    ImGui::PushID((int)i);

                    // Hover-only background; click anywhere on the row
                    // toggles the checkbox.
                    ImGui::SetCursorScreenPos(P0);
                    const bool bRowClicked = ImGui::InvisibleButton("##row", ImVec2(eastl::max(Avail, 1.0f), Height));
                    const bool bHovered    = ImGui::IsItemHovered();
                    if (bRowClicked && States[i] == ESaveState::Idle)
                    {
                        Selection[i] = !Selection[i];
                    }

                    ImDrawList* DL = ImGui::GetWindowDrawList();
                    const ImU32 BgCol = ImGui::ColorConvertFloat4ToU32(bHovered ? kProjDialogRowBgHover : kProjDialogRowBg);
                    DL->AddRectFilled(P0, P1, BgCol, 4.0f);
                    DL->AddRectFilled(P0, ImVec2(P0.x + 3.0f, P1.y), ImGui::ColorConvertFloat4ToU32(Accent), 4.0f);

                    // Checkbox (gets click priority over the row-wide
                    // invisible button thanks to ImGui's per-widget rect).
                    ImGui::SetCursorScreenPos(ImVec2(P0.x + 14.0f, P0.y + 16.0f));
                    if (States[i] != ESaveState::Idle)
                    {
                        ImGui::BeginDisabled();
                    }
                    ImGui::Checkbox("##sel", &Selection[i]);
                    if (States[i] != ESaveState::Idle)
                    {
                        ImGui::EndDisabled();
                    }

                    // Title + path stacked.
                    const float TextX = P0.x + 48.0f;
                    ImGui::SetCursorScreenPos(ImVec2(TextX, P0.y + 7.0f));
                    ImGuiX::Font::PushFont(ImGuiX::Font::EFont::SmallBold);
                    ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextPrimary);
                    ImGui::TextUnformatted(Package->GetName().c_str());
                    ImGui::PopStyleColor();
                    ImGuiX::Font::PopFont();

                    ImGui::SetCursorScreenPos(ImVec2(TextX, P0.y + 27.0f));
                    ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Tiny);
                    ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextDim);
                    ImGui::TextUnformatted(Package->GetPackagePath().c_str());
                    ImGui::PopStyleColor();
                    ImGuiX::Font::PopFont();

                    // Trailing status badge (right-aligned).
                    const char* StatusIcon = nullptr;
                    const char* StatusText = nullptr;
                    ImVec4      StatusCol  = kProjDialogTextDim;
                    switch (States[i])
                    {
                        case ESaveState::Saving:
                            StatusIcon = LE_ICON_WATCH_VIBRATE;
                            StatusText = "Saving...";
                            StatusCol  = kProjDialogAccentBlue;
                            break;
                        case ESaveState::Success:
                            StatusIcon = LE_ICON_CHECK_CIRCLE_OUTLINE;
                            StatusText = "Saved";
                            StatusCol  = ImVec4(0.45f, 0.85f, 0.55f, 1.0f);
                            break;
                        case ESaveState::Failed:
                            StatusIcon = LE_ICON_ALERT_CIRCLE_OUTLINE;
                            StatusText = "Failed";
                            StatusCol  = kProjDialogDanger;
                            break;
                        default: break;
                    }
                    if (StatusText)
                    {
                        const ImVec2 LabelSize = ImGui::CalcTextSize(StatusText);
                        const ImVec2 IconSize  = ImGui::CalcTextSize(StatusIcon);
                        const float  StatusW   = LabelSize.x + IconSize.x + 10.0f;
                        ImGui::SetCursorScreenPos(ImVec2(P1.x - StatusW - 12.0f, P0.y + (Height - LabelSize.y) * 0.5f));
                        ImGui::PushStyleColor(ImGuiCol_Text, StatusCol);
                        ImGui::Text("%s %s", StatusIcon, StatusText);
                        ImGui::PopStyleColor();
                    }

                    // End the row on a real (zero-size) item so IsSetPos is cleared; otherwise the last row's
                    // bare trailing SetCursorScreenPos trips ImGui's extend-bounds assert at EndChild.
                    const ImVec2 PkgNextRow(P0.x, P1.y + 6.0f);
                    ImGui::SetCursorScreenPos(PkgNextRow);
                    ImGui::Dummy(ImVec2(0.0f, 0.0f));
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            // Footer: Save & Exit (blue), Discard & Exit (gold), Cancel (soft),
            // right-aligned so the primary action lands at the F-pattern target.
            constexpr float ButtonH = 32.0f;
            constexpr float Gap     = 8.0f;
            constexpr float MinW    = 90.0f;

            // Single '&': ImGui has no mnemonic escaping (only '#' is special), so "&&" reached the screen
            // as a literal "&&".
            const char* SaveLabel    = LE_ICON_CONTENT_SAVE " Save & Exit";
            const char* DiscardLabel = LE_ICON_DELETE " Discard & Exit";
            const char* CancelLabel  = "Cancel";

            // Measured per label rather than one shared constant. The three differ in length, two carry an
            // icon glyph, and all of them scale with font size and DPI -- a fixed 150px fit "Save & Exit"
            // and clipped the longer "Discard & Exit" to "Discard & Exi".
            auto MeasureButton = [](const char* Label)
            {
                const float Text = ImGui::CalcTextSize(Label).x + ImGui::GetStyle().FramePadding.x * 2.0f + 12.0f;
                return Text < MinW ? MinW : Text;
            };

            const float SaveW    = MeasureButton(SaveLabel);
            const float DiscardW = MeasureButton(DiscardLabel);
            const float CancelW  = MeasureButton(CancelLabel);
            const float Total    = SaveW + DiscardW + CancelW + Gap * 2.0f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - Total - 16.0f);

            bool bShouldClose = false;

            // Save & Exit, primary.
            const bool bAnySelected = SelectedCount > 0;
            ImGui::PushStyleColor(ImGuiCol_Button,        kProjDialogAccentBlue);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.46f, 0.74f, 1.00f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.58f, 0.92f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.06f, 0.08f, 0.12f, 1.0f));
            if (!bAnySelected)
            {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(SaveLabel, ImVec2(SaveW, ButtonH)))
            {
                // Synchronous save loop; the dialog stays open this frame so failed
                // entries show a "Failed" badge instead of disappearing silently.
                bool bAllOK = true;
                for (size_t i = 0; i < Packages.size(); ++i)
                {
                    if (!Selection[i])
                    {
                        continue;
                    }
                    States[i] = ESaveState::Saving;
                    const bool bOK = CPackage::SavePackage(Packages[i], Packages[i]->GetPackagePath());
                    States[i] = bOK ? ESaveState::Success : ESaveState::Failed;
                    if (!bOK)
                    {
                        bAllOK = false;
                    }
                }
                // Close (and proceed with exit) only if every selected save succeeded;
                // a failure keeps the dialog up so the user can pick another action.
                bShouldClose = bAllOK;
            }
            if (!bAnySelected)
            {
                ImGui::EndDisabled();
            }
            ImGui::PopStyleColor(4);
            ImGui::SameLine(0.0f, Gap);

            // Discard & Exit, gold accent, makes the consequence visible.
            ImGui::PushStyleColor(ImGuiCol_Button,        kProjDialogRowBg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kProjDialogRowBgHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kProjDialogRowBgActive);
            ImGui::PushStyleColor(ImGuiCol_Text,          kProjDialogAccentGold);
            if (ImGui::Button(DiscardLabel, ImVec2(DiscardW, ButtonH)))
            {
                bShouldClose = true;
            }
            ImGui::PopStyleColor(4);
            ImGui::SameLine(0.0f, Gap);

            // Cancel, soft, abort the exit entirely.
            ImGui::PushStyleColor(ImGuiCol_Button,        kProjDialogRowBg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kProjDialogRowBgHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kProjDialogRowBgActive);
            ImGui::PushStyleColor(ImGuiCol_Text,          kProjDialogAccentSoft);
            if (ImGui::Button(CancelLabel, ImVec2(CancelW, ButtonH)))
            {
                FApplication::CancelExit();
                bVerifyingDirtyPackages = false; // re-arm for the next exit attempt
                bShouldClose = true;
            }
            ImGui::PopStyleColor(4);

            return bShouldClose;
        });
    }
    
    uint32 FEditorUI::CountDirtyPackages() const
    {
        uint32 Count = 0;
        for (TObjectIterator<CPackage> Itr; Itr; ++Itr)
        {
            CPackage* Package = *Itr;

            // Marked for destroy = a deleted asset awaiting the deferred drain; it has nothing to save, so
            // counting it would leave the editor claiming unsaved work that cannot be saved.
            if (Package->HasAnyFlag(OF_MarkedDestroy) || !Package->IsDirty())
            {
                continue;
            }
            ++Count;
        }

        return Count;
    }

    void FEditorUI::DrawTitleBarMenu(const FUpdateContext& UpdateContext)
    {
        DirtyPackageScanCountdown -= UpdateContext.GetDeltaTime();
        if (DirtyPackageScanCountdown <= 0.0f)
        {
            DirtyPackageScanCountdown = DirtyPackageScanInterval;
            DirtyPackageCount = CountDirtyPackages();
        }

        const float Scale     = ImGuiX::GetUIScale();
        const float RowHeight = ImGuiX::FApplicationTitleBar::GetContentRowHeight();

        // The icon is taller than a line of text, so lift it by half the difference to keep it centered on
        // the same row as the menus. The bar restores the row's Y after each item, so put it straight back.
        const float IconSize = ImFloor(RowHeight * 1.5f);
        const float IconLift = ImFloor((IconSize - RowHeight) * 0.5f);

        static const FString LuminaIcon = Paths::GetEngineResourceDirectory() + "/Textures/Lumina.png";
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - IconLift);
        ImGui::Image(ImGuiX::ToImTextureRef(LuminaIcon), ImVec2(IconSize, IconSize));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + IconLift);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * Scale, 10.0f * Scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * Scale, 8.0f * Scale));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * Scale);

        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.08f, 0.08f, 0.1f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.2f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.25f, 0.25f, 0.27f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.92f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.35f, 0.45f, 0.8f));

        ImGui::SetNextWindowSizeConstraints(ImVec2(220 * Scale, 1), ImVec2(280 * Scale, 1000 * Scale));
        DrawFileMenu();
        DrawProjectMenu();
        DrawToolsMenu();
        DrawHelpMenu();

        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);

        // Trails the menus with the rest of the bar to itself. The section is sized from what is left over
        // rather than from a constant, so a long name is clipped only when it genuinely runs out of window.
        if (GEditorEngine->HasLoadedProject())
        {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextDim());
            ImGuiX::Text("{}", GEditorEngine->GetProjectName());
            ImGui::PopStyleColor();

            if (DirtyPackageCount > 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Warning());
                ImGui::TextUnformatted(LE_ICON_CONTENT_SAVE_ALERT " Unsaved Changes");
                ImGui::PopStyleColor();
                ImGuiX::TextTooltip("{} package(s) with unsaved changes. Ctrl+Shift+S saves all of them.", DirtyPackageCount);
            }
        }
    }
    
    FEditorUI::FTitleBarStats FEditorUI::BuildTitleBarStats(const FUpdateContext& UpdateContext)
    {
        FTitleBarStats Stats;

        const float DeltaSeconds     = UpdateContext.GetDeltaTime();
        const float CurrentFrameTime = DeltaSeconds * 1000.0f;

        // Blend weight for this frame's share of the average, from how long the frame took rather than
        // from the frame merely happening.
        auto WeightFor = [DeltaSeconds](float TimeConstantSeconds) -> float
        {
            if (TimeConstantSeconds <= 0.0f || DeltaSeconds <= 0.0f)
            {
                return 1.0f;
            }

            return 1.0f - Math::Exp(-DeltaSeconds / TimeConstantSeconds);
        };

        // Averaging frame time and inverting, never averaging FPS: the mean of a reciprocal is not the
        // reciprocal of the mean, and doing it the other way reads high whenever frame times vary.
        SmoothedFrameTime = (SmoothedFrameTime <= 0.0f)
            ? CurrentFrameTime
            : SmoothedFrameTime + (CurrentFrameTime - SmoothedFrameTime) * WeightFor(FrameTimeSmoothingSeconds);

        SmoothedFPS = (SmoothedFrameTime > 0.0f) ? 1000.0f / SmoothedFrameTime : 0.0f;

        const float MemoryMiB = (float)Platform::GetProcessMemoryUsageBytes() / (1024.0f * 1024.0f);
        SmoothedMemoryMiB = (SmoothedMemoryMiB <= 0.0f)
            ? MemoryMiB
            : SmoothedMemoryMiB + (MemoryMiB - SmoothedMemoryMiB) * WeightFor(MemorySmoothingSeconds);

        // Device-local heaps only: the host-visible ones are system RAM the GPU can see, already counted
        // by the process working set above, and adding them would read as VRAM that is not there.
        RHI::FGPUMemoryStats GPUStats;
        RHI::GetGPUMemoryStats(GPUStats);

        uint64 GPUUsedBytes = 0;
        uint64 GPUBudgetBytes = 0;
        for (const RHI::FGPUMemoryHeapStats& Heap : GPUStats.Heaps)
        {
            if (Heap.bDeviceLocal)
            {
                GPUUsedBytes   += Heap.UsageBytes;
                GPUBudgetBytes += Heap.BudgetBytes;
            }
        }

        constexpr float BytesPerMiB = 1024.0f * 1024.0f;
        const float GPUMemoryMiB = (float)GPUUsedBytes / BytesPerMiB;
        SmoothedGPUMemoryMiB = (SmoothedGPUMemoryMiB <= 0.0f)
            ? GPUMemoryMiB
            : SmoothedGPUMemoryMiB + (GPUMemoryMiB - SmoothedGPUMemoryMiB) * WeightFor(MemorySmoothingSeconds);
        GPUMemoryBudgetMiB = (float)GPUBudgetBytes / BytesPerMiB;

        Stats.Perf.sprintf(LE_ICON_GAUGE " %3.0f FPS / %.2f ms", SmoothedFPS, SmoothedFrameTime);
        Stats.Objects.sprintf(LE_ICON_CUBE_OUTLINE " %i", GObjectArray.GetNumAliveObjects());
        Stats.Memory.sprintf(LE_ICON_MEMORY " %.0f MiB", SmoothedMemoryMiB);
        Stats.GPUMemory.sprintf(LE_ICON_EXPANSION_CARD " %.0f / %.0f MiB", SmoothedGPUMemoryMiB, GPUMemoryBudgetMiB);

        // Four items, so three gaps -- plus one gap of slack, since the strings change width from frame to
        // frame and a section that is a pixel short clips the last character.
        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        Stats.Width = ImGui::CalcTextSize(Stats.Perf.c_str()).x
                    + ImGui::CalcTextSize(Stats.Objects.c_str()).x
                    + ImGui::CalcTextSize(Stats.Memory.c_str()).x
                    + ImGui::CalcTextSize(Stats.GPUMemory.c_str()).x
                    + Spacing * 4.0f;

        return Stats;
    }

    void FEditorUI::DrawTitleBarInfoStats(const FTitleBarStats& Stats)
    {
        // Icons rather than labels: the section competes with the menus for bar width, and every stat has a
        // tooltip spelling out what it is.
        ImGui::TextUnformatted(Stats.Perf.c_str());
        ImGuiX::TextTooltip("{}", "Frame rate and frame time, smoothed.");

        ImGui::SameLine();
        ImGui::TextUnformatted(Stats.Objects.c_str());
        ImGuiX::TextTooltip("{}", "Live CObjects.");

        ImGui::SameLine();
        ImGui::TextUnformatted(Stats.Memory.c_str());
        ImGuiX::TextTooltip("{}", "Process working set (matches Task Manager), smoothed.");

        ImGui::SameLine();
        ImGui::TextUnformatted(Stats.GPUMemory.c_str());
        ImGuiX::TextTooltip("{}", "Device-local GPU memory in use against the driver's budget for this "
                                  "process, smoothed. Includes memory other processes forced us to share.");
    }

    void FEditorUI::DrawFileMenu()
    {
        if (!ImGui::BeginMenu(LE_ICON_FILE " File"))
        {
            return;
        }
        // Mirrors the Ctrl+S the focused tool already handles, so the menu and the shortcut do the
        // same thing rather than one of them silently doing nothing.
        ImGui::BeginDisabled(LastActiveTool == nullptr);
        if (ImGui::MenuItem(LE_ICON_ZIP_DISK " Save", "Ctrl+S"))
        {
            SaveActiveTool();
        }
        ImGui::EndDisabled();

        if (ImGui::MenuItem(LE_ICON_ZIP_DISK " Save All", "Ctrl+Shift+S"))
        {
            SaveAllDirtyPackages();
        }

        ImGui::Separator();

        if (ImGui::MenuItem(LE_ICON_LANGUAGE_CSHARP " Recompile C# Assemblies", "Shift+F11"))
        {
            DotNet::ReloadScripts();
        }

        ImGui::Separator();

        DrawToolMenuItem<FSettingsEditorTool>(LE_ICON_COG " Settings", this);

        if (ImGui::BeginMenu(LE_ICON_ROTATE_LEFT " Recent"))
        {
            auto Recents = PruneMissingRecents();
            bool bAny = false;
            for (const auto& Item : Recents)
            {
                bAny = true;

                const FString DisplayName = DisplayNameFromLprojPath(Item);
                if (ImGui::MenuItem(DisplayName.c_str(), Item.c_str()))
                {
                    GEditorEngine->LoadProject(FStringView(Item.c_str(), Item.size()));
                    OnProjectLoaded();
                }
            }

            if (!bAny)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.62f, 1.0f));
                ImGui::TextUnformatted("(none)");
                ImGui::PopStyleColor();
            }

            ImGui::EndMenu();
        }

        ImGui::Separator();
        

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.6f, 0.4f, 1.0f));
        if (ImGui::BeginMenu(LE_ICON_HAMMER " Shaders"))
        {
            //if (ImGui::MenuItem(LE_ICON_HAMMER " Recompile All", "F5"))
            //{
            //    CMaterial::CreateDefaultMaterial();
            //}
            
            if (ImGui::MenuItem(LE_ICON_MATERIAL_DESIGN " Recompile Default Material"))
            {
                CMaterial::CreateDefaultMaterial();
            }

            if (ImGui::MenuItem(LE_ICON_FOLDER " Open Shaders Directory", "F6"))
            {
                Platform::LaunchURL(UTF8_TO_TCHAR(Paths::GetEngineShadersDirectory().c_str()));
            }
            
            ImGui::EndMenu();
        }
        ImGui::PopStyleColor();

        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.4f, 0.4f, 1.0f));
        if (ImGui::MenuItem(LE_ICON_DOOR_OPEN " Exit", "Alt+F4"))
        {
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
            NewProjectDialog();
        }
    
        ImGui::Separator();

        DrawToolMenuItem<FProjectPackagerEditorTool>(LE_ICON_PACKAGE_VARIANT " Package Project...", this);

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

        DrawToolMenuItem<FAssetRegistryEditorTool>(LE_ICON_DATABASE " Asset Registry", this);
        DrawToolMenuItem<FCPUProfilerEditorTool>(LE_ICON_CHART_BAR " CPU Profiler", this);
        DrawToolMenuItem<FTaskSystemProfilerEditorTool>(LE_ICON_CHART_TIMELINE " Task System", this);
        DrawToolMenuItem<FGameplayInsightsEditorTool>(LE_ICON_CHART_TIMELINE_VARIANT " Gameplay Insights", this);
        DrawToolMenuItem<FShadowAtlasEditorTool>(LE_ICON_GRID " Shadow Atlas", this);
        DrawToolMenuItem<FTextureHeapEditorTool>(LE_ICON_IMAGE_ALBUM " Texture Heap", this);
        DrawToolMenuItem<FTextureStreamingEditorTool>(LE_ICON_SWAP_VERTICAL " Texture Streaming", this);
        DrawToolMenuItem<FMemoryProfilerEditorTool>(LE_ICON_MEMORY " Memory", this);
        DrawToolMenuItem<FScriptDiagnosticsEditorTool>(LE_ICON_LANGUAGE_CSHARP " C# Diagnostics", this);
        DrawToolMenuItem<FObjectBrowserEditorTool>(LE_ICON_LIST_BOX " Object Browser", this);
        DrawToolMenuItem<FConsoleVariableEditorTool>(LE_ICON_TUNE " Console Variables", this);
        DrawToolMenuItem<FPluginBrowserEditorTool>(LE_ICON_PUZZLE " Plugin Browser", this);

        // Plugin-contributed standalone tools (see FToolsMenuRegistry). Section is hidden when empty.
        if (!FToolsMenuRegistry::Get().IsEmpty())
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.62f, 1.0f), "Plugins");
            ImGui::Separator();

            for (const FToolsMenuEntry& Entry : FToolsMenuRegistry::Get().GetEntries())
            {
                const bool bActive = Entry.IsActive ? Entry.IsActive() : false;
                if (ImGui::MenuItem(Entry.Label.c_str(), nullptr, bActive) && Entry.OnToggle)
                {
                    Entry.OnToggle();
                }
            }
        }

        ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.62f, 1.0f), "ImGui Tools");
        ImGui::Separator();
        
        ImGui::MenuItem(LE_ICON_WINDOW_OPEN " ImGui Style Editor", nullptr, &bShowImGuiStyleEditor);
        ImGui::MenuItem(LE_ICON_WINDOW_OPEN " ImGui Demo", nullptr, &bShowDearImGuiDemoWindow);
        ImGui::MenuItem(LE_ICON_CHART_BAR " ImPlot Demo", nullptr, &bShowImPlotDemoWindow);
        
        ImGui::Spacing();
        
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.62f, 1.0f), "External Tools");
        ImGui::Separator();
        
        if (ImGui::MenuItem(LE_ICON_WATCH " Tracy Profiler", "F8"))
        {
            LaunchTracyProfiler();
        }
        
        if (ImGui::MenuItem(LE_ICON_CAMERA " RenderDoc Capture", "F11"))
        {
            FRenderDoc::Get().TriggerCapture();
        }

        if (ImGui::BeginMenu(LE_ICON_CAMERA " Screenshot"))
        {
            if (ImGui::MenuItem("Save PNG (Tonemapped)", "F9"))
            {
                Screenshot::CaptureActiveWorld(Screenshot::ECaptureSource::FinalLDR);
            }
            if (ImGui::MenuItem("Save HDR (Linear)", "Shift+F9"))
            {
                Screenshot::CaptureActiveWorld(Screenshot::ECaptureSource::SceneHDR);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(LE_ICON_FOLDER " Open Screenshots Folder"))
            {
                FString Folder = Paths::GetEngineDirectory() + "/Saved/Screenshots";
                Paths::CreateDirectories(FStringView(Folder.c_str(), Folder.size()));
                Platform::LaunchURL(UTF8_TO_TCHAR(Folder.c_str()));
            }
            ImGui::EndMenu();
        }

        ImGui::Spacing();
        
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.62f, 1.0f), "Settings");
        ImGui::Separator();
        
        const bool bVSyncEnabled = RHI::GetVSync();
        if (ImGui::MenuItem(LE_ICON_DISC_PLAYER " V-Sync", nullptr, bVSyncEnabled))
        {
            const bool bNewVSync = !bVSyncEnabled;
            RHI::SetVSync(bNewVSync);
            Render().RecreatePrimarySwapchain();
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

        if (ImGui::BeginMenu(LE_ICON_BOOK " Documentation"))
        {
            if (ImGui::MenuItem(LE_ICON_GROUP " Lumina"))
            {
                Platform::LaunchURL(TEXT("https://luminagameengine.com/"));
            }

            ImGui::EndMenu();
        }
    
        if (ImGui::MenuItem(LE_ICON_ACCOUNT_QUESTION " Tutorials"))
        {
            Platform::LaunchURL(TEXT("https://luminagameengine.com"));
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
        
        // About + Contributors are now tabs of the same tool, so a single menu entry covers both.
        DrawToolMenuItem<FAboutEditorTool>(LE_ICON_CIRCLE " About Lumina", this);

        ImGui::EndMenu();
    }

    void FEditorUI::OpenProjectDialog()
    {
        ModalManager.CreateDialogue("Open Project", ImVec2(720, 560), [this] () -> bool
        {
            bool bShouldClose = false;

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::MediumBold);
            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextPrimary);
            ImGui::TextUnformatted(LE_ICON_FOLDER_OPEN " Open Project");
            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Hero: Create New Project (primary action).
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.50f, 0.95f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.60f, 1.00f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f, 0.45f, 0.90f, 1.00f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 12));
            if (ImGui::Button(LE_ICON_FOLDER_PLUS "  Create New Project", ImVec2(-1, 0)))
            {
                DeferShowDialog([this] { NewProjectDialog(); });
                bShouldClose = true;
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            ImGui::BeginChild("##ProjectListBody", ImVec2(0, -52), false);
            {
                // Recent projects.
                DrawSectionHeader("RECENT PROJECTS");

                // Prune entries whose .lproject is gone (project folder
                // deleted on disk) and legacy name-only entries in one pass.
                auto Recents = PruneMissingRecents();

                bool bAnyRecent = false;
                FString PendingRemove;
                FFixedString PendingLoad;
                for (const auto& Entry : Recents)
                {
                    if (Entry.find(".lproject") == FString::npos)
                    {
                        continue;
                    }
                    bAnyRecent = true;

                    const FString DisplayName = DisplayNameFromLprojPath(Entry);
                    bool bCloseClicked = false;
                    const bool bClicked = DrawProjectRow(
                        LE_ICON_FOLDER,
                        DisplayName.c_str(),
                        Entry.c_str(),
                        kProjDialogAccentGold,
                        /*bCompact=*/false,
                        /*bShowClose=*/true,
                        &bCloseClicked);

                    if (bClicked)
                    {
                        PendingLoad = Entry.c_str();
                    }
                    if (bCloseClicked)
                    {
                        PendingRemove = Entry;
                    }
                }

                if (!bAnyRecent)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextMuted);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
                    ImGui::TextUnformatted("No recent projects yet.");
                    ImGui::PopStyleColor();
                }

                if (!PendingRemove.empty())
                {
                    RemoveRecentProject(PendingRemove);
                }
                if (!PendingLoad.empty())
                {
                    GEditorEngine->LoadProject(PendingLoad);
                    OnProjectLoaded();
                    bShouldClose = true;
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            // Footer: Browse + Cancel.
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

            if (ImGui::Button(LE_ICON_FOLDER_OPEN "  Browse for project file...", ImVec2(260, 30)))
            {
                FFixedString Project;
                if (Platform::OpenFileDialogue(
                        Project,
                        "Open Project",
                        "Lumina Project (*.lproject)\0*.lproject\0All Files (*.*)\0*.*\0",
                        nullptr))
                {
                    GEditorEngine->LoadProject(Project);
                    OnProjectLoaded();
                    bShouldClose = true;
                }
            }

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 116);

            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.20f, 0.22f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.26f, 0.29f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.16f, 0.16f, 0.18f, 1.00f));
            if (ImGui::Button("Cancel", ImVec2(120, 30)))
            {
                bShouldClose = true;
            }
            ImGui::PopStyleColor(3);

            ImGui::PopStyleVar();

            return bShouldClose;
        }, true, false);
    }

    void FEditorUI::NewProjectDialog()
    {
        ModalManager.CreateDialogue("New Project", ImVec2(720, 600), [this] () -> bool
        {
            static char NewProjectName[256] = "MyProject";
            static char NewProjectPath[512] = "";
            static FString LastError;

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::MediumBold);
            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextPrimary);
            ImGui::TextUnformatted(LE_ICON_FOLDER_PLUS " Create New Project");
            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();
            ImGui::Spacing();
            ImGui::Separator();

            // Scrollable body so the footer stays pinned.
            ImGui::BeginChild("##NewProjBody", ImVec2(0, -52), false);

            DrawSectionHeader("TEMPLATE");
            DrawProjectRow(
                LE_ICON_CUBE,
                "Blank Project (C++)",
                "Empty C++ module + C# scripting. F5 in the generated .slnx launches the editor with the project loaded.",
                kProjDialogAccentBlue,
                /*bCompact=*/false);

            DrawSectionHeader("PROJECT NAME");
            ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.15f, 0.15f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.20f, 0.20f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ProjectName", NewProjectName, sizeof(NewProjectName));
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);

            DrawSectionHeader("LOCATION");
            ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.15f, 0.15f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.20f, 0.20f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::SetNextItemWidth(-120);
            ImGui::InputText("##ProjectPath", NewProjectPath, sizeof(NewProjectPath));
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            // OpenFileDialogue with null filter → folder picker (FOS_PICKFOLDERS).
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            if (ImGui::Button(LE_ICON_FOLDER " Browse", ImVec2(110, 0)))
            {
                FFixedString File;
                if (Platform::OpenFileDialogue(File, "Select project location"))
                {
                    const size_t Count = eastl::min(File.size(), sizeof(NewProjectPath) - 1);
                    memcpy(NewProjectPath, File.c_str(), Count);
                    NewProjectPath[Count] = '\0';
                }
            }
            ImGui::PopStyleVar();

            // Inline error box (red bordered child, matching rename modal).
            if (!LastError.empty())
            {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.30f, 0.10f, 0.10f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.80f, 0.20f, 0.20f, 0.40f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
                ImGui::BeginChild("##NewProjError", ImVec2(-1, 0),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                    ImGuiWindowFlags_NoScrollbar);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
                ImGui::TextUnformatted(LE_ICON_ALERT_OCTAGON);
                ImGui::SameLine();
                ImGui::TextWrapped("%s", LastError.c_str());
                ImGui::PopStyleColor();
                ImGui::EndChild();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
            }

            ImGui::EndChild();

            // Footer: Back + Create.
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

            // Back returns to the Open Project dialog; defer so the modal closes
            // cleanly before the next CreateDialogue.
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.20f, 0.22f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.26f, 0.29f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.16f, 0.16f, 0.18f, 1.00f));
            const bool bBack = ImGui::Button(LE_ICON_ARROW_LEFT "  Back", ImVec2(110, 30));
            ImGui::PopStyleColor(3);

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 156);

            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.50f, 0.95f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.60f, 1.00f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f, 0.45f, 0.90f, 1.00f));
            const bool bCreateClicked = ImGui::Button(LE_ICON_CHECK "  Create Project", ImVec2(160, 30));
            ImGui::PopStyleColor(3);

            ImGui::PopStyleVar();

            if (bBack)
            {
                LastError.clear();
                DeferShowDialog([this] { OpenProjectDialog(); });
                return true;
            }

            if (bCreateClicked)
            {
                FFixedString ProjectFile;
                FString Error;
                if (GEditorEngine->CreateProject(NewProjectName, NewProjectPath, ProjectFile, Error))
                {
                    LastError.clear();

                    GEditorEngine->GenerateProjectFiles(VFS::Parent(ProjectFile));
                    PushRecentProject(ProjectFile.c_str());

                    // Chain into the "Project Created" dialog so the user knows
                    // they need to act (the editor has no project loaded).
                    const FString ProjectFileCopy(ProjectFile.c_str(), ProjectFile.size());
                    DeferShowDialog([this, ProjectFileCopy]
                    {
                        ProjectCreatedDialog(FStringView(ProjectFileCopy.c_str(), ProjectFileCopy.size()));
                    });
                    return true;
                }

                LastError = Error;
            }

            return false;
        });
    }

    void FEditorUI::ProjectCreatedDialog(FStringView ProjectFile)
    {
        const FString ProjectFileCopy(ProjectFile.data(), ProjectFile.size());

        // Derive the .slnx path from the .lproject path (sibling file).
        FString SlnPath = ProjectFileCopy;
        {
            const size_t Dot = SlnPath.find_last_of('.');
            if (Dot != FString::npos)
            {
                SlnPath.erase(Dot);
            }
            SlnPath.append(".slnx");
        }

        ModalManager.CreateDialogue("Project Created", ImVec2(640, 400), [this, ProjectFileCopy, SlnPath] () -> bool
        {
            // Polled each frame; cheap (stat call on local disk).
            std::error_code Ec;
            const bool bSlnReady = std::filesystem::exists(SlnPath.c_str(), Ec);

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::MediumBold);
            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextPrimary);
            ImGui::TextUnformatted(LE_ICON_CHECK_CIRCLE " Project Created");
            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextDim);
            ImGui::TextWrapped(
                "Your project was created. premake is generating its Visual Studio "
                "solution in the background, watch the editor log for output.");
            ImGui::PopStyleColor();

            ImGui::Spacing();

            // Project path callout with a Copy button.
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kProjDialogRowBg);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
            ImGui::BeginChild("##ProjectPath", ImVec2(-1, 38), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogAccentGold);
            ImGui::TextUnformatted(LE_ICON_FOLDER);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextPrimary);
            ImGui::TextUnformatted(ProjectFileCopy.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 26);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextDim);
            if (ImGui::SmallButton(LE_ICON_CONTENT_COPY))
            {
                ImGui::SetClipboardText(ProjectFileCopy.c_str());
                ImGuiX::Notifications::NotifyInfo("Project path copied.");
            }
            ImGui::PopStyleColor(2);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            // Solution-status indicator.
            ImGui::Spacing();
            if (bSlnReady)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.55f, 1.0f));
                ImGui::TextUnformatted(LE_ICON_CHECK " Solution ready.");
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kProjDialogTextMuted);
                ImGui::TextUnformatted(LE_ICON_CLOCK_OUTLINE " Waiting for premake to finish...");
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ImGui::Spacing();

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

            const float Avail = ImGui::GetContentRegionAvail().x;
            const float BtnH  = 36.0f;
            const float Gap   = 8.0f;
            const float BtnW  = (Avail - Gap * 2.0f) / 3.0f;

            // Reveal in Explorer, always available.
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.22f, 0.26f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.30f, 0.34f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.18f, 0.20f, 1.00f));
            if (ImGui::Button(LE_ICON_FOLDER_OPEN "  Reveal in Explorer", ImVec2(BtnW, BtnH)))
            {
                Platform::ShowFileInExplorer(UTF8_TO_TCHAR(ProjectFileCopy.c_str()));
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine(0.0f, Gap);

            // Close Editor, secondary.
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.22f, 0.26f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.30f, 0.34f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.18f, 0.20f, 1.00f));
            bool bCloseEditor = false;
            if (ImGui::Button(LE_ICON_POWER "  Close Editor", ImVec2(BtnW, BtnH)))
            {
                bCloseEditor = true;
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine(0.0f, Gap);

            // Open Solution, primary blue, disabled until premake finishes.
            ImGui::BeginDisabled(!bSlnReady);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.50f, 0.95f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.60f, 1.00f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f, 0.45f, 0.90f, 1.00f));
            bool bOpenSln = false;
            if (ImGui::Button(LE_ICON_PLAY "  Open Solution", ImVec2(BtnW, BtnH)))
            {
                bOpenSln = true;
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();

            ImGui::PopStyleVar();

            if (bOpenSln)
            {
                Platform::LaunchURL(UTF8_TO_TCHAR(SlnPath.c_str()));
                FApplication::RequestExit();
                return true;
            }

            if (bCloseEditor)
            {
                FApplication::RequestExit();
                return true;
            }

            return false;
        }, /*bBlocking=*/true, /*bCloseable=*/false);
    }

    bool FEditorUI::TryOpenEditorStartupMap()
    {
        const FStringView EditorMapView = GetDefault<CProjectSettings>()->EditorStartupMap.GetPath();
        const FString RawEditorStartupMap(EditorMapView.data(), EditorMapView.size());
        if (RawEditorStartupMap.empty())
        {
            LOG_INFO("No Project.EditorStartupMap set; opening no map.");
            return true;   // settled: nothing to wait for
        }

        // Tolerate legacy absolute paths from before the path resolver, same as FEngine::LoadStartupMap.
        const FFixedString EditorStartupMapFixed = VFS::ResolveToVirtualPath(RawEditorStartupMap);
        const FString EditorStartupMap(EditorStartupMapFixed.c_str(), EditorStartupMapFixed.size());

        // Unlike the game path, this one needs a registry hit rather than loading the object directly, so
        // a map that exists on disk is simply not visible until discovery has walked it. Not an error --
        // the caller retries when the registry updates.
        if (FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(EditorStartupMap))
        {
            OpenAssetEditor(Data->AssetGUID);

            // Reported AFTER the fact, and checked rather than assumed. OpenAssetEditor returns silently
            // when the package fails to load, and routes a non-world asset to its own tool instead; both
            // leave the editor on the world it already had. Logging "Opening..." before the call turned
            // either into a success message, which is indistinguishable from the map having opened and
            // simply containing the default scene.
            const CWorld* Opened = WorldEditorTool->GetWorld();
            if (Opened != nullptr && Opened->GetGUID() == Data->AssetGUID)
            {
                LOG_DISPLAY("Opened editor startup map '{}' (resolved '{}').",
                            RawEditorStartupMap.c_str(), EditorStartupMap.c_str());
            }
            else
            {
                LOG_ERROR("Editor startup map '{}' (resolved '{}') did not open; staying on the current world. "
                          "The package failed to load, or that path is not a world asset.",
                          RawEditorStartupMap.c_str(), EditorStartupMap.c_str());
            }

            return true;
        }

        return false;
    }

    void FEditorUI::OnProjectLoaded()
    {
        // Armed here, not at startup: the tab list lives in the project's own /Config, so it isn't
        // readable until the project is mounted. Consumed on the next update.
        bSessionRestorePending = true;
        RestoredSessionTabs.clear();

        ContentBrowser->RefreshContentBrowser();
        
        if (!TryOpenEditorStartupMap())
        {
            if (PendingStartupMapHandle.IsValid())
            {
                FAssetRegistry::Get().GetOnAssetRegistryUpdated().Remove(PendingStartupMapHandle);
                PendingStartupMapHandle = {};
            }

            PendingStartupMapHandle = FAssetRegistry::Get().GetOnAssetRegistryUpdated().AddLambda([this]
            {
                // One-shot: stop retrying once the map opens (or turns out not to be configured at all).
                if (TryOpenEditorStartupMap() && PendingStartupMapHandle.IsValid())
                {
                    FAssetRegistry::Get().GetOnAssetRegistryUpdated().Remove(PendingStartupMapHandle);
                    PendingStartupMapHandle = {};
                }
            });
        }

        // Reconstruct the .lproject path and record it as a recent + Editor.StartupProject
        // (auto-loaded on the next bare launch). Normalize: the join can double the slash.
        const FStringView ProjectDir  = GEngine->GetProjectPath();
        const FStringView ProjectName = GEngine->GetProjectName();
        if (!ProjectDir.empty() && !ProjectName.empty())
        {
            FFixedString LprojPath;
            LprojPath.assign(ProjectDir.data(), ProjectDir.size());
            LprojPath.append("/");
            LprojPath.append(ProjectName.data(), ProjectName.size());
            LprojPath.append(".lproject");
            Paths::Normalize(LprojPath);

            PushRecentProject(FStringView(LprojPath.c_str(), LprojPath.size()));
            GetMutableDefault<CEditorSettings>()->StartupProject = FString(LprojPath.c_str(), LprojPath.size());
            GConfig->SaveSettings(CEditorSettings::StaticClass());
        }
    }

}
