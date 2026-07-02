#include "CSharpScriptComponentCustomization.h"
#include "UI/Properties/PropertyTable.h"
#include "imgui.h"
#include "Lumina.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/ScriptExports.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptValueBridge.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "UI/EditorUI.h"
#include "World/Entity/Components/CSharpScriptComponent.h"
#include "FileSystem/FileSystem.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        using namespace Scripting;

        // Clears a slot's binding and class. Frees any live instance from the current generation first.
        void RebindSlot(SScriptInstance& Slot, const FString& NewClass)
        {
            if (Slot.Instance != nullptr && Slot.Generation == DotNet::GetScriptGeneration())
            {
                DotNet::DestroyEntityScript(Slot.Instance);
            }
            Slot.Instance = nullptr;
            Slot.BindState = ECSharpBindState::Unbound;
            Slot.ScriptClass = NewClass;
            Slot.Generation = -1;

            if (NewClass.empty())
            {
                Slot.Values.Reset();
            }
        }

        FString FindScriptSourceFile(FStringView ScriptClass)
        {
            FStringView ClassName = ScriptClass;
            const size_t Dot = ScriptClass.rfind('.');
            if (Dot != FStringView::npos)
            {
                ClassName = ScriptClass.substr(Dot + 1);
            }
            if (ClassName.empty())
            {
                return FString();
            }

            TVector<FString> Roots;
            Roots.emplace_back("/Game/Scripts");
            Roots.emplace_back("/Engine/Resources/Scripts");
            for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
            {
                if (Plugin != nullptr && Plugin->IsEnabled() && Plugin->IsContentMounted())
                {
                    Roots.emplace_back(Plugin->GetMountAlias() + "/Scripts");
                }
            }

            const FString Needle = FString("class ") + FString(ClassName.data(), ClassName.size());
            FString FoundDisk;
            for (const FString& Root : Roots)
            {
                if (!FoundDisk.empty())
                {
                    break;
                }
                VFS::RecursiveDirectoryIterator(FStringView(Root.c_str(), Root.size()), [&](const VFS::FFileInfo& Info)
                {
                    if (!FoundDisk.empty() || Info.IsDirectory())
                    {
                        return;
                    }
                    if (Info.GetExt() != ".cs")
                    {
                        return;
                    }
                    const FStringView VPath(Info.VirtualPath.c_str(), Info.VirtualPath.size());
                    if (VPath.find("/obj/") != FStringView::npos || VPath.find("/bin/") != FStringView::npos)
                    {
                        return;
                    }
                    FString Text;
                    if (!VFS::ReadFile(Text, VPath))
                    {
                        return;
                    }
                    // Word-boundary "class <ClassName>" so it doesn't match "class <ClassName>Foo".
                    for (size_t Pos = Text.find(Needle.c_str(), 0); Pos != FString::npos; Pos = Text.find(Needle.c_str(), Pos + Needle.size()))
                    {
                        const size_t After = Pos + Needle.size();
                        const char C = (After < Text.size()) ? Text[After] : ' ';
                        const bool bIdent = (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_';
                        if (!bIdent)
                        {
                            FoundDisk.assign(Info.PathSource.c_str(), Info.PathSource.size());
                            return;
                        }
                    }
                });
            }

            return FoundDisk;
        }

        void OpenScriptSource(const FString& ScriptClass)
        {
            if (ScriptClass.empty())
            {
                return;
            }
            const FString Path = FindScriptSourceFile(FStringView(ScriptClass.c_str(), ScriptClass.size()));
            if (Path.empty())
            {
                LOG_WARN("Open script: no .cs file declaring '{}' found under the script roots.", ScriptClass.c_str());
                return;
            }
            FEditorUI::OpenScriptInExternalEditor(FStringView(Path.c_str(), Path.size()));
        }

        // Draws a clickable button per [Button] method on the slot. Invoking is a runtime action on the live
        // instance, so it doesn't participate in the property-change transaction and is disabled until bound.
        void DrawScriptButtons(SScriptInstance& Slot)
        {
            TVector<Scripting::FScriptButton> Buttons;
            DotNet::GatherScriptButtons(FStringView(Slot.ScriptClass.c_str(), Slot.ScriptClass.size()), Buttons);
            if (Buttons.empty())
            {
                return;
            }

            ImGui::SeparatorText("Script Actions");

            const bool bHasInstance = Slot.Instance != nullptr && Slot.Generation == DotNet::GetScriptGeneration();

            ImGui::BeginDisabled(!bHasInstance);
            for (const Scripting::FScriptButton& Button : Buttons)
            {
                FString Label = Button.Label + "##btn_" + Button.Method;
                if (ImGui::Button(Label.c_str(), ImVec2(-FLT_MIN, 0.0f)))
                {
                    DotNet::InvokeScriptButton(Slot.Instance, FStringView(Button.Method.c_str(), Button.Method.size()));
                }
                if (!Button.Tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip("%s", Button.Tooltip.c_str());
                }
            }
            ImGui::EndDisabled();

            if (!bHasInstance)
            {
                ImGui::TextDisabled("Buttons run while the game is playing.");
            }
        }
    }

    TSharedPtr<FCSharpScriptComponentPropertyCustomization> FCSharpScriptComponentPropertyCustomization::MakeInstance()
    {
        return MakeShared<FCSharpScriptComponentPropertyCustomization>();
    }

    EPropertyChangeOp FCSharpScriptComponentPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        bool bWasChanged = false;
        auto* Component = static_cast<SScriptComponent*>(Property->ContainerPtr);

        ImGui::PushID(this);

        // Discover the loaded C# EntityScript types once for every slot's picker.
        TVector<FString> Types;
        DotNet::GatherEntityScriptTypes(Types);

        // Keep the per-slot editor state parallel to the component's slots.
        SlotViews.resize(Component->Scripts.size());

        for (int32 Index = 0; Index < (int32)Component->Scripts.size(); ++Index)
        {
            SScriptInstance& Slot = Component->Scripts[Index];
            FSlotView& View = SlotViews[Index];
            ImGui::PushID(Index);

            // Each script sits under its own collapsible header (its class name), with a remove button on
            // the header line so it works whether the script is expanded or not.
            const FString HeaderLabel = Slot.ScriptClass.empty() ? FString("(empty script)") : Slot.ScriptClass;
            const float ButtonWidth = ImGui::GetFrameHeight();
            const float Spacing = ImGui::GetStyle().ItemSpacing.x;

            // Local X of the row's right edge, captured before the full-width header so the remove button
            // can be right-aligned on the header line.
            const float HeaderRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

            // AllowOverlap so the right-aligned remove button on this header line receives its own clicks
            // instead of the header swallowing them (toggling collapse).
            ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
            const bool bOpen = ImGui::CollapsingHeader((HeaderLabel + "##scripthdr").c_str(), ImGuiTreeNodeFlags_AllowOverlap);

            ImGui::SameLine(HeaderRight - ButtonWidth);
            if (ImGui::SmallButton(LE_ICON_DELETE "##RemoveCSharpScript"))
            {
                PendingMutation = [Component, Index]
                {
                    if (Index < (int32)Component->Scripts.size())
                    {
                        RebindSlot(Component->Scripts[Index], FString());
                        Component->Scripts.erase(Component->Scripts.begin() + Index);
                    }
                };
                bWasChanged = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Remove this script");
            }

            if (bOpen)
            {
                ImGui::Indent();

                int32 Current = INDEX_NONE;
                for (int32 i = 0; i < (int32)Types.size(); ++i)
                {
                    if (Types[i] == Slot.ScriptClass)
                    {
                        Current = i;
                        break;
                    }
                }

                const FString Preview = Slot.ScriptClass.empty() ? FString("Select a script...") : Slot.ScriptClass;

                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 2.0f * ButtonWidth - 2.0f * Spacing);
                const int32 Picked = ImGuiX::SearchableCombo(
                    "##CSharpScript", Preview.c_str(), (int32)Types.size(), Current,
                    [&Types](int32 i) { return FFixedString(Types[i].c_str(), Types[i].size()); },
                    LE_ICON_LANGUAGE_CSHARP);
                ImGui::PopItemWidth();

                if (Picked != INDEX_NONE && Picked != Current)
                {
                    FString NewClass = Types[Picked];
                    PendingMutation = [Component, Index, NewClass]
                    {
                        if (Index < (int32)Component->Scripts.size())
                        {
                            RebindSlot(Component->Scripts[Index], NewClass);
                        }
                    };
                    bWasChanged = true;
                }

                ImGui::SameLine();
                ImGui::BeginDisabled(Slot.ScriptClass.empty());
                if (ImGui::Button(LE_ICON_OPEN_IN_NEW "##OpenCSharpScript", ImVec2(ButtonWidth, 0)))
                {
                    OpenScriptSource(Slot.ScriptClass);
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Open script in your IDE");
                }

                ImGui::SameLine();
                ImGui::BeginDisabled(Slot.ScriptClass.empty());
                if (ImGui::Button(LE_ICON_CLOSE "##ClearCSharpScript", ImVec2(ButtonWidth, 0)))
                {
                    PendingMutation = [Component, Index]
                    {
                        if (Index < (int32)Component->Scripts.size())
                        {
                            RebindSlot(Component->Scripts[Index], FString());
                        }
                    };
                    bWasChanged = true;
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Clear script");
                }

                if (!Slot.ScriptClass.empty())
                {
                    const FStringView ClassName(Slot.ScriptClass.c_str(), Slot.ScriptClass.size());
                    const CScriptStruct* Layout = DotNet::GetScriptStruct(ClassName);
                    if (Layout != nullptr)
                    {
                        Slot.Values.EnsureLayout(Layout);
                        void* Buffer = Slot.Values.GetBuffer();
                        if (Buffer != nullptr)
                        {
                            if (View.ValueTable == nullptr || View.BoundLayout != Layout || View.BoundBuffer != Buffer)
                            {
                                View.BoundLayout = Layout;
                                View.BoundBuffer = Buffer;
                                View.ValueTable = MakeUnique<FPropertyTable>(Buffer, const_cast<CScriptStruct*>(Layout),
                                    const_cast<void*>(Layout->GetDefaults()));
                                View.ValueTable->SetPostEditCallback([this](const FPropertyChangedEvent&) { bValueEdited = true; });
                            }

                            ImGui::SeparatorText("Script Properties");
                            View.ValueTable->DrawTree();

                            if (bValueEdited)
                            {
                                bValueEdited = false;
                                bWasChanged = true;
                                if (Slot.Instance != nullptr && Slot.Generation == DotNet::GetScriptGeneration())
                                {
                                    TVector<Scripting::FScriptPropertyEntry> Values;
                                    Scripting::ReadStructToValues(Layout, Buffer, Values);
                                    DotNet::ApplyScriptProperties(Slot.Instance, Values);
                                }
                            }
                        }
                    }
                    else
                    {
                        // The script type is gone (deleted, or renamed with no [Alias]) or failed to compile.
                        // Release the store's stale layout so the previous generation's minted CScriptStruct
                        // tree (instanced-list candidates and all) is torn down instead of stranded on this
                        // slot; EnsureLayout(nullptr) migrates the live values to tagged bytes first, so they
                        // restore if the type comes back. Drop the property table that aliased the freed buffer.
                        if (Slot.Values.GetLayout() != nullptr)
                        {
                            Slot.Values.EnsureLayout(nullptr);
                        }
                        if (View.ValueTable != nullptr)
                        {
                            View.ValueTable.reset();
                            View.BoundLayout = nullptr;
                            View.BoundBuffer = nullptr;
                        }

                        ImGui::TextDisabled("Script '%s' is not loaded (renamed, removed, or failed to compile).", Slot.ScriptClass.c_str());
                        ImGui::TextDisabled("Saved values are preserved -- pick a script above to remap (matching fields carry over by name).");
                    }

                    DrawScriptButtons(Slot);
                }

                ImGui::Unindent();
            }

            ImGui::PopID();
        }

        if (ImGui::Button(LE_ICON_PLUS " Add Script", ImVec2(-FLT_MIN, 0.0f)))
        {
            PendingMutation = [Component] { Component->Scripts.emplace_back(); };
            bWasChanged = true;
        }

        ImGui::PopID();

        if (bWasChanged)
        {
            if (bFinishPending)
            {
                return EPropertyChangeOp::Updated;
            }
            bFinishPending = true;
            return EPropertyChangeOp::Started;
        }
        if (bFinishPending)
        {
            bFinishPending = false;
            return EPropertyChangeOp::Finished;
        }
        return EPropertyChangeOp::None;
    }

    void FCSharpScriptComponentPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        // Runs after BeginTransaction so the undo snapshot captured the pre-change state.
        if (PendingMutation)
        {
            PendingMutation();
            PendingMutation = {};
        }
    }

    void FCSharpScriptComponentPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
    }
}
