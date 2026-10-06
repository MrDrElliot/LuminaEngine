#include "RuntimePCH.h"
#include "ImGuiNativeFrames.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "ImGuiDesignIcons.h"
#include "Containers/Vector.h"
#include "Core/Windows/GLFWInclude.h"

namespace Lumina::ImGuiX::NativeFrames
{
    namespace
    {
        struct FViewportCaption
        {
            ImGuiID         ViewportID = 0;
            TVector<ImRect> Rects;
        };

        // Rebuilt every frame and read by the OS hit test between frames, both on the thread that pumps messages.
        TVector<FViewportCaption> GCaptions;

        bool WantsNativeFrame(ImGuiViewport* Viewport)
        {
            const ImGuiWindow* Window = static_cast<ImGuiViewportP*>(Viewport)->Window;
            if (Window == nullptr || (Viewport->Flags & ImGuiViewportFlags_NoFocusOnClick))
            {
                return false;
            }

            constexpr ImGuiWindowFlags ShortLived = ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Modal;
            return (Window->Flags & ShortLived) == 0;
        }

        bool CaptionHitTest(ImGuiViewport* Viewport, float ClientX, float ClientY)
        {
            for (const FViewportCaption& Caption : GCaptions)
            {
                if (Caption.ViewportID != Viewport->ID)
                {
                    continue;
                }

                for (const ImRect& Rect : Caption.Rects)
                {
                    if (Rect.Contains(ImVec2(ClientX, ClientY)))
                    {
                        return true;
                    }
                }
                return false;
            }
            return false;
        }

        GLFWwindow* GetFramedWindow(const ImGuiViewport* Viewport)
        {
            if (Viewport == ImGui::GetMainViewport() || Viewport->PlatformHandle == nullptr)
            {
                return nullptr;
            }

            GLFWwindow* Window = static_cast<GLFWwindow*>(Viewport->PlatformHandle);
            const bool bFramed = glfwGetWindowAttrib(Window, GLFW_DECORATED) == GLFW_TRUE && glfwGetWindowAttrib(Window, GLFW_TITLEBAR) == GLFW_FALSE;
            return bFramed ? Window : nullptr;
        }

        float GetTitleRowHeight()
        {
            const ImGuiContext& G = *ImGui::GetCurrentContext();
            return G.FontSize + G.Style.FramePadding.y * 2.0f;
        }

        // Leaf nodes whose tab bar sits on the top edge of the window, where a native caption would be.
        void GatherTopRowNodes(ImGuiDockNode* Node, float TopY, TVector<ImGuiDockNode*>& OutNodes)
        {
            if (Node == nullptr || !Node->IsVisible || Node->Pos.y > TopY + 1.0f)
            {
                return;
            }

            if (Node->IsLeafNode())
            {
                if (Node->TabBar != nullptr && !Node->IsHiddenTabBar() && !Node->IsNoTabBar())
                {
                    OutNodes.push_back(Node);
                }
                return;
            }

            GatherTopRowNodes(Node->ChildNodes[0], TopY, OutNodes);
            GatherTopRowNodes(Node->ChildNodes[1], TopY, OutNodes);
        }

        // Free tab bar space that ImGui only uses to drag the whole node, or the spacer that reserves it.
        ImRect GetCaptionRect(ImGuiDockNode* Node, ImGuiID SpacerID)
        {
            ImGuiTabBar* TabBar = Node->TabBar;
            float MinX = TabBar->BarRect.Min.x;
            float MaxX = TabBar->BarRect.Max.x;
            if (SpacerID != 0)
            {
                const ImGuiTabItem* Spacer = ImGui::TabBarFindTabByID(TabBar, SpacerID);
                if (Spacer == nullptr)
                {
                    return ImRect();
                }
                MinX += Spacer->Offset;
                MaxX = MinX + Spacer->Width;
            }
            else
            {
                for (const ImGuiTabItem& Tab : TabBar->Tabs)
                {
                    const float Scroll = (Tab.Flags & (ImGuiTabItemFlags_Leading | ImGuiTabItemFlags_Trailing)) ? 0.0f : TabBar->ScrollingAnim;
                    MinX = ImMax(MinX, TabBar->BarRect.Min.x + Tab.Offset - Scroll + Tab.Width);
                }
            }

            return ImRect(MinX, Node->Pos.y, MaxX, Node->Pos.y + GetTitleRowHeight());
        }

        // Returns the spacer's tab id, which is zero when the tabs leave no room for one.
        ImGuiID DrawWindowControls(ImGuiDockNode* Node, GLFWwindow* Window, bool bCanMinimize)
        {
            if (!ImGui::DockNodeBeginAmendTabBar(Node))
            {
                return 0;
            }

            const bool bMaximized = glfwGetWindowAttrib(Window, GLFW_MAXIMIZED) == GLFW_TRUE;
            const char* MinimizeLabel = LE_ICON_WINDOW_MINIMIZE "##NativeMinimize";
            const char* MaximizeLabel = bMaximized ? LE_ICON_WINDOW_RESTORE "##NativeMaximize" : LE_ICON_WINDOW_MAXIMIZE "##NativeMaximize";
            constexpr ImGuiTabItemFlags Flags = ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip;

            // ImGui packs trailing items right after the tabs, so a spacer sized from natural widths pushes the buttons to the edge.
            const float Spacing = ImGui::GetStyle().ItemInnerSpacing.x;
            float Used = ImGui::TabItemCalcSize(MaximizeLabel, false).x + Spacing;
            if (bCanMinimize)
            {
                Used += ImGui::TabItemCalcSize(MinimizeLabel, false).x + Spacing;
            }
            for (const ImGuiTabItem& Tab : Node->TabBar->Tabs)
            {
                if (!(Tab.Flags & ImGuiTabItemFlags_Trailing))
                {
                    Used += Tab.ContentWidth + Spacing;
                }
            }

            ImGuiID SpacerID = 0;
            const float SpacerWidth = ImFloor(Node->TabBar->BarRect.GetWidth() - Used);
            if (SpacerWidth >= 1.0f)
            {
                constexpr ImGuiCol HiddenColors[] = { ImGuiCol_Tab, ImGuiCol_TabHovered, ImGuiCol_TabSelected, ImGuiCol_TabSelectedOverline,
                                                      ImGuiCol_TabDimmed, ImGuiCol_TabDimmedSelected, ImGuiCol_TabDimmedSelectedOverline };
                for (ImGuiCol Color : HiddenColors)
                {
                    ImGui::PushStyleColor(Color, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                }
                ImGui::SetNextItemWidth(SpacerWidth);
                ImGui::TabItemButton("##NativeCaption", Flags);
                SpacerID = ImGui::GetItemID();
                ImGui::PopStyleColor(IM_ARRAYSIZE(HiddenColors));
            }

            // The taskbar is the only way back to a minimized window, so one without an icon there never minimizes.
            if (bCanMinimize && ImGui::TabItemButton(MinimizeLabel, Flags))
            {
                glfwIconifyWindow(Window);
            }

            if (ImGui::TabItemButton(MaximizeLabel, Flags))
            {
                if (bMaximized)
                {
                    glfwRestoreWindow(Window);
                }
                else
                {
                    glfwMaximizeWindow(Window);
                }
            }

            ImGui::DockNodeEndAmendTabBar();
            return SpacerID;
        }
    }

    void Install()
    {
        ImGui_ImplGlfw_SetNativeFrameCallbacks(WantsNativeFrame, CaptionHitTest);
    }

    void Update()
    {
        GCaptions.clear();

        ImGuiContext& G = *ImGui::GetCurrentContext();
        if (!(G.IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
        {
            return;
        }

        // ImGui moves the window itself when a tab is dragged, which a maximized window would ignore.
        if (G.MovingWindow != nullptr)
        {
            if (GLFWwindow* Moving = GetFramedWindow(G.MovingWindow->Viewport); Moving != nullptr && glfwGetWindowAttrib(Moving, GLFW_MAXIMIZED) == GLFW_TRUE)
            {
                glfwRestoreWindow(Moving);
            }
        }

        TVector<ImGuiDockNode*> TopNodes;
        for (ImGuiViewportP* Viewport : G.Viewports)
        {
            GLFWwindow* Window = GetFramedWindow(Viewport);
            if (Window == nullptr || Viewport->Window == nullptr || Viewport->Window->DockNodeAsHost == nullptr)
            {
                continue;
            }

            ImGuiDockNode* Root = Viewport->Window->DockNodeAsHost;
            if (Root->LastFrameActive != G.FrameCount)
            {
                continue;
            }

            TopNodes.clear();
            GatherTopRowNodes(Root, Viewport->Pos.y, TopNodes);
            if (TopNodes.empty())
            {
                continue;
            }

            ImGuiDockNode* RightMost = TopNodes[0];
            for (ImGuiDockNode* Node : TopNodes)
            {
                if (Node->Pos.x + Node->Size.x > RightMost->Pos.x + RightMost->Size.x)
                {
                    RightMost = Node;
                }
            }

            const bool bCanMinimize = (Viewport->Flags & ImGuiViewportFlags_NoTaskBarIcon) == 0;
            const ImGuiID SpacerID = DrawWindowControls(RightMost, Window, bCanMinimize);

            FViewportCaption& Caption = GCaptions.emplace_back();
            Caption.ViewportID = Viewport->ID;
            for (ImGuiDockNode* Node : TopNodes)
            {
                if (Node == RightMost && SpacerID == 0)
                {
                    continue;
                }

                ImRect Rect = GetCaptionRect(Node, Node == RightMost ? SpacerID : 0);
                Rect.Translate(ImVec2(-Viewport->Pos.x, -Viewport->Pos.y));
                if (Rect.GetWidth() > 0.0f)
                {
                    Caption.Rects.push_back(Rect);
                }
            }
        }
    }
}
