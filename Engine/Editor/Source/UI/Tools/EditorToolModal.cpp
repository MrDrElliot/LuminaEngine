#include "EditorToolModal.h"

#include "Core/Templates/LuminaTemplate.h"
#include "UI/SlowTaskModal.h"

namespace Lumina
{
    namespace
    {
        // The main viewport can be on another monitor, so anchor to the one containing the click.
        ImVec2 ModalAnchorCenter(ImVec2 ClickPos)
        {
            if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            {
                const ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
                for (int i = 0; i < PlatformIO.Monitors.Size; ++i)
                {
                    const ImGuiPlatformMonitor& Monitor = PlatformIO.Monitors[i];
                    if (ClickPos.x >= Monitor.WorkPos.x && ClickPos.x < Monitor.WorkPos.x + Monitor.WorkSize.x
                        && ClickPos.y >= Monitor.WorkPos.y && ClickPos.y < Monitor.WorkPos.y + Monitor.WorkSize.y)
                    {
                        return ImVec2(Monitor.WorkPos.x + Monitor.WorkSize.x * 0.5f, Monitor.WorkPos.y + Monitor.WorkSize.y * 0.5f);
                    }
                }
            }
            return ImGui::GetMainViewport()->GetCenter();
        }
    }

    void FEditorModalManager::CreateDialogue(const FString& Title, ImVec2 Size, TMoveOnlyFunction<bool()> DrawFunction, bool bBlocking, bool bCloseable)
    {
        if (ActiveModal != nullptr)
        {
            return;
        }

        ActiveModal = MakeUnique<FEditorToolModal>(Title, Size, bCloseable);
        ActiveModal->DrawFunction = Move(DrawFunction);
        ActiveModal->bBlocking = bBlocking;
        ActiveModal->OpenPos = ImGui::GetMousePos();
    }


    void FEditorModalManager::DrawDialogue()
    {
        // ImGui allows only one modal chain, so sibling modals would close each other every frame.
        if (ActiveModal && ActiveModal->bBlocking)
        {
            ImGui::OpenPopup(ActiveModal->Title.c_str());

            ImGui::SetNextWindowPos(ModalAnchorCenter(ActiveModal->OpenPos), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ActiveModal->Size, ImGuiCond_Appearing);

            bool* bOpen = ActiveModal->bCloseable ? &ActiveModal->bOpen : nullptr;

            if (ImGui::BeginPopupModal(ActiveModal->Title.c_str(), bOpen, ImGuiWindowFlags_NoCollapse))
            {
                const bool bClose = ActiveModal->DrawModal() || !ActiveModal->bOpen;

                SlowTaskModal::Render();

                if (bClose)
                {
                    ImGui::CloseCurrentPopup();
                    ActiveModal.reset();
                }
                ImGui::EndPopup();
            }
            else if (!ActiveModal->bOpen)
            {
                // Without this teardown OpenPopup re-opens the modal every frame, blocking input invisibly.
                ActiveModal.reset();
            }
            return;
        }

        if (ActiveModal)
        {
            // A non-blocking modal is a plain window, so there is no modal chain to conflict with.
            ImGui::SetNextWindowPos(ModalAnchorCenter(ActiveModal->OpenPos), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ActiveModal->Size, ImGuiCond_Appearing);

            bool* bOpen = ActiveModal->bCloseable ? &ActiveModal->bOpen : nullptr;

            const bool bVisible = ImGui::Begin(ActiveModal->Title.c_str(), bOpen, ImGuiWindowFlags_NoCollapse);
            if (bVisible && (ActiveModal->DrawModal() || !ActiveModal->bOpen))
            {
                ActiveModal.reset();
            }
            ImGui::End();
        }

        // With no blocking modal in the way, the slow-task popup owns the root modal scope.
        SlowTaskModal::Render();
    }
}
