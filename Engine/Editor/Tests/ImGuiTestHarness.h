#pragma once

#include "Tools/UI/ImGui/ImGuiModule.h"

extern "C" DLL_IMPORT void LuminaModuleSetupImGui(void* InImGui, void* InImPlot);

namespace Lumina
{
    // Runs ImGui frames with no renderer behind them, standing in for the backend's texture work.
    class FImGuiTestHarness
    {
    public:

        static constexpr ImVec2 kDisplaySize = ImVec2(1280.0f, 720.0f);

        FImGuiTestHarness()
        {
            ImGuiX::InstallImGuiAllocator();
            Context = ImGui::CreateContext();
            ShareContext(Context);

            ImGuiIO& IO = ImGui::GetIO();
            IO.DisplaySize  = kDisplaySize;
            IO.DeltaTime    = 1.0f / 60.0f;
            IO.IniFilename  = nullptr;
            IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        }

        ~FImGuiTestHarness()
        {
            ImGui::DestroyContext(Context);
            ShareContext(nullptr);
        }

        FImGuiTestHarness(const FImGuiTestHarness&) = delete;
        FImGuiTestHarness& operator=(const FImGuiTestHarness&) = delete;
        FImGuiTestHarness(FImGuiTestHarness&&) = delete;
        FImGuiTestHarness& operator=(FImGuiTestHarness&&) = delete;

        // Opens a frame with one unpadded window covering the display.
        static void BeginFrame()
        {
            ImGui::NewFrame();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(kDisplaySize);
            ImGui::Begin("Host", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            ImGui::PopStyleVar();
        }

        static void EndFrame()
        {
            ImGui::End();
            ImGui::Render();
            for (ImTextureData* Texture : ImGui::GetPlatformIO().Textures)
            {
                if (Texture->Status == ImTextureStatus_WantCreate || Texture->Status == ImTextureStatus_WantUpdates)
                {
                    Texture->SetTexID((ImTextureID)1);
                    Texture->SetStatus(ImTextureStatus_OK);
                }
                else if (Texture->Status == ImTextureStatus_WantDestroy)
                {
                    Texture->SetTexID(ImTextureID_Invalid);
                    Texture->SetStatus(ImTextureStatus_Destroyed);
                }
            }
        }

    private:

        // The test binary, the Editor DLL and the Runtime DLL each link their own ImGui copy.
        static void ShareContext(ImGuiContext* InContext)
        {
            ImGuiX::AdoptImGuiContext(InContext, nullptr);
            ImGuiX::AdoptRuntimeImGuiContext(InContext, nullptr);
            LuminaModuleSetupImGui(InContext, nullptr);
        }

        ImGuiContext* Context = nullptr;
    };
}
