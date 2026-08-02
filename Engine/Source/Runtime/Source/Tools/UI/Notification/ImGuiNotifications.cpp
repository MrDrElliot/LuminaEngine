#include "RuntimePCH.h"
#include "ImGuiNotifications.h"
#include "imgui.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Core/Templates/LuminaTemplate.h"
#ifdef _WIN32
    #include <Windows.h>
#endif
#include "GLFW/glfw3.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"

namespace Lumina::ImGuiX::Notifications
{
    
    class RUNTIME_API FNotification
    {
        constexpr static float DefaultLifetime = 3.0f;
        constexpr static float DefaultFadeTime = 1.0f;

    public:
        
        enum class EPhase
        {
            FadeIn,
            Wait,
            FadeOut,
            Expired,
        };

        enum class EPosition
        {
            TopLeft,
            TopCenter,
            TopRight,
            BottomLeft,
            BottomCenter,
            BottomRight,
            Center,
        };

    public:

        FNotification(EType InType, FFixedString&& Message)
            : Message(Move(Message))
            , Type(InType)
        {
            CreationTime = glfwGetTime();
        }

        NODISCARD EType GetType() const { return Type; }

        NODISCARD EPhase GetPhase() const
        {
            const float ElapsedTime = static_cast<float>(glfwGetTime() - CreationTime);

            if (ElapsedTime > DefaultFadeTime + Lifetime + DefaultFadeTime)
            {
                return EPhase::Expired;
            }
            
            if (ElapsedTime > DefaultFadeTime + Lifetime)
            {
                return EPhase::FadeOut;
            }
            
            if (ElapsedTime > DefaultFadeTime)
            {
                return EPhase::Wait;
            }

            return EPhase::FadeIn;
        }

        NODISCARD float GetFadePercentage() const
        {
            const EPhase Phase = GetPhase();
            float const ElapsedTime = static_cast<float>(glfwGetTime() - CreationTime);

            if (Phase == EPhase::FadeIn)
            {
                return ElapsedTime / DefaultFadeTime;
            }
            
            
            if (Phase == EPhase::FadeOut)
            {
                return (1.0f - ((ElapsedTime - DefaultFadeTime - Lifetime) / DefaultFadeTime));
            }

            return 1.0f;
        }

        NODISCARD ImVec4 GetColor( float opacity = 1.0f ) const
        {
            switch (Type)
            {
                case EType::Success:    return { 0, 255, 0, opacity }; // Green
                case EType::Warning:    return { 255, 255, 0, opacity }; // Yellow
                case EType::Error:      return { 255, 0, 0, opacity }; // Error
                case EType::Info:       return { 0, 157, 255, opacity }; // Blue
                default:                break;
            }

            return { 1.0f, 1.0f, 1.0f, 1.0f };
        }

        FStringView GetIcon() const
        {
            switch (Type)
            {
                case EType::Success:    return LE_ICON_CHECK_CIRCLE;
                case EType::Warning:    return LE_ICON_ALERT;
                case EType::Error:      return LE_ICON_CLOSE_CIRCLE;
                case EType::Info:       return LE_ICON_INFORMATION;
                default:                break;
            }

            return {};
        }

        FStringView GetTitle() const
        {
            switch (Type)
            {
                case EType::Success:    return "Success";
                case EType::Warning:    return "Warning";
                case EType::Error:      return "Error";
                case EType::Info:       return "Info";
                default:                break;
            }

            return {};
        }

        FStringView GetMessageContent() const { return Message; }
        
    private:

        FFixedString    Message;
        double          CreationTime = -1.0f;
        float           Lifetime = DefaultLifetime;
        EType           Type = EType::None;
    };
    
    static FMutex NotificationMutex;
    static TFixedVector<FNotification, 10> GNotifications;
    static float GBottomInset = 0.0f;

    void SetBottomInset(float Pixels)
    {
        GBottomInset = Pixels;
    }

    void Initialize()
    {
    }

    void Shutdown()
    {
    }

    void Render()
    {
        constexpr static float PaddingX = 20.0f; // Bottom-left X padding
        constexpr static float PaddingY = 20.0f; // Bottom-left Y padding
        constexpr static float PaddingNotificationY = 10.0f; // Padding Y between each message
        
        const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
        // Anchor to the work area (excludes the status bar) and lift above any open
        // footer drawer via GBottomInset so toasts are never covered/clipped.
        const ImVec2 WorkPos  = MainViewport->WorkPos;
        const ImVec2 WorkSize = MainViewport->WorkSize;

        FScopeLock Lock(NotificationMutex);

        float NotificationStartPosY = 0.0f;
        for (size_t i = 0; i < GNotifications.size(); i++)
        {
            const FNotification& Notification = GNotifications[i];

            if (Notification.GetPhase() == FNotification::EPhase::Expired)
            {
                GNotifications.erase(GNotifications.begin() + i);
                i--;
                continue;
            }
            
            FStringView Icon        = Notification.GetIcon();
            FStringView Title       = Notification.GetTitle();
            FStringView Message     = Notification.GetMessageContent();
            const float Opacity     = Notification.GetFadePercentage();
            const ImVec4 TextColor  = Notification.GetColor(Opacity);

            FFixedString WindowName(FFixedString::CtorSprintf(), "##Notification%d", i);
            
            ImGuiWindowFlags Flags = 
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoInputs | 
                ImGuiWindowFlags_NoNav | 
                ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoFocusOnAppearing;
            
            ImGui::SetNextWindowBgAlpha(Opacity);
            ImGui::SetNextWindowPos(WorkPos + ImVec2(WorkSize.x - PaddingX, WorkSize.y - PaddingY - GBottomInset - NotificationStartPosY), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
            ImGui::PushStyleColor(ImGuiCol_Border, TextColor);
            if (ImGui::Begin(WindowName.c_str(), nullptr, Flags))
            {
                ImGui::PushTextWrapPos(WorkSize.x / 3.0f);

                bool DrawSeparator = false;

                if (!Icon.empty())
                {
                    ImGui::TextColored(TextColor, "%s", Icon.data());
                    DrawSeparator = true;
                }

                if (!Title.empty())
                {
                    if (!Icon.empty())
                    {
                        ImGui::SameLine();
                    }

                    ImGui::TextUnformatted(Title.begin(), Title.end());
                    DrawSeparator = true;
                }

                if (DrawSeparator && !Message.empty())
                {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);
                }

                if (!Message.empty())
                {
                    if (DrawSeparator)
                    {
                        ImGui::Separator();
                    }

                    ImGui::TextUnformatted(Message.begin(), Message.end());
                }

                ImGui::PopTextWrapPos();
            }

            NotificationStartPosY += ImGui::GetWindowHeight() + PaddingNotificationY;

            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
    }

    void NotifyInternal(EType Type, FStringView Msg)
    {
        FScopeLock Lock(NotificationMutex);
        
        FFixedString MessageContent(Msg.begin(), Msg.end());
        GNotifications.emplace_back(Type, Move(MessageContent));
    }
    
}