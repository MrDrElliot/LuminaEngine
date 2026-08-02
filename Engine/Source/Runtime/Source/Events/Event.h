#pragma once

#include "Containers/Array.h"
#include "Containers/String.h"
#include "KeyCodes.h"
#include "MouseCodes.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    enum class EEventType : uint8
    {
        None = 0,

        KeyPressed,
        KeyReleased,
        KeyRepeat,
        CharInput,
        CharInputMods,
        
        MouseButtonPressed,
        MouseButtonReleased,
        MouseMoved,
        MouseScrolled,
        MouseEntered,
        MouseLeft,

        WindowResize,
        WindowClose,
        WindowFocus,
        WindowLostFocus,
        WindowMoved,
        WindowMaximized,
        WindowMinimized,
        WindowRestored,
        WindowRefresh,
        WindowContentScaleChanged,
        FramebufferResize,

        FileDrop,

        JoystickConnected,
        JoystickDisconnected,

        AppTick,
        AppUpdate,
        AppRender,
    };
    
    class FEvent
    {
    public:
        virtual ~FEvent() = default;
        virtual EEventType GetEventType() const = 0;
        virtual const char* GetName() const = 0;
        
        bool IsHandled() const { return bHandled; }
        void SetHandled(bool bInHandled) { bHandled = bInHandled; }
        
        template<typename T>
        requires(eastl::is_base_of_v<FEvent, T>)
        bool IsA() const { return GetEventType() == T::GetStaticType(); }

        template<typename T>
        requires(eastl::is_base_of_v<FEvent, T>)
        T& As()
        {
            return static_cast<T&>(*this);
        }
        
    protected:
        bool bHandled = false;
    };

    #define EVENT_CLASS_TYPE(type) \
        static EEventType GetStaticType() { return EEventType::type; } \
        virtual EEventType GetEventType() const override { return GetStaticType(); } \
        virtual const char* GetName() const override { return #type; }

    class FKeyEvent : public FEvent
    {
    public:
        EKey GetKeyCode() const { return KeyCode; }
        
        bool IsModifierDown(EKey Modifier) const
        {
            switch (Modifier)
            {
                case EKey::LeftControl:
                case EKey::RightControl:
                    return bCtrl;
                case EKey::LeftShift:
                case EKey::RightShift:
                    return bShift;
                case EKey::LeftAlt:
                case EKey::RightAlt:
                    return bAlt;
                case EKey::LeftSuper:
                case EKey::RightSuper:
                    return bSuper;
                default:
                    return false;
            }
        }
        
        bool IsCtrlDown() const { return bCtrl; }
        bool IsShiftDown() const { return bShift; }
        bool IsAltDown() const { return bAlt; }
        bool IsSuperDown() const { return bSuper; }
        
    protected:
        FKeyEvent(EKey InKeyCode, bool InCtrl, bool InShift, bool InAlt, bool InSuper)
            : KeyCode(InKeyCode), bCtrl(InCtrl), bShift(InShift), bAlt(InAlt), bSuper(InSuper) {}
        
        EKey KeyCode;
        bool bCtrl;
        bool bShift;
        bool bAlt;
        bool bSuper;
    };

    class FKeyPressedEvent : public FKeyEvent
    {
    public:
        FKeyPressedEvent(EKey InKeyCode, bool InCtrl, bool InShift, bool InAlt, bool InSuper, bool InRepeat = false)
            : FKeyEvent(InKeyCode, InCtrl, InShift, InAlt, InSuper), bRepeat(InRepeat) {}
        
        bool IsRepeat() const { return bRepeat; }
        
        EVENT_CLASS_TYPE(KeyPressed)
        
    private:
        bool bRepeat;
    };

    class FKeyReleasedEvent : public FKeyEvent
    {
    public:
        FKeyReleasedEvent(EKey InKeyCode, bool InCtrl, bool InShift, bool InAlt, bool InSuper)
            : FKeyEvent(InKeyCode, InCtrl, InShift, InAlt, InSuper) {}
        
        EVENT_CLASS_TYPE(KeyReleased)
    };

    class FCharInputEvent : public FEvent
    {
    public:
        FCharInputEvent(uint32 InCodepoint)
            : Codepoint(InCodepoint) {}
        
        uint32 GetCodepoint() const { return Codepoint; }

        FString GetCharacter() const
        {
            char buffer[5] = { 0 };
            if (Codepoint < 0x80)
            {
                buffer[0] = (char)Codepoint;
            }
            else if (Codepoint < 0x800)
            {
                buffer[0] = (char)(0xC0 | (Codepoint >> 6));
                buffer[1] = (char)(0x80 | (Codepoint & 0x3F));
            }
            else if (Codepoint < 0x10000)
            {
                buffer[0] = (char)(0xE0 | (Codepoint >> 12));
                buffer[1] = (char)(0x80 | ((Codepoint >> 6) & 0x3F));
                buffer[2] = (char)(0x80 | (Codepoint & 0x3F));
            }
            else
            {
                buffer[0] = (char)(0xF0 | (Codepoint >> 18));
                buffer[1] = (char)(0x80 | ((Codepoint >> 12) & 0x3F));
                buffer[2] = (char)(0x80 | ((Codepoint >> 6) & 0x3F));
                buffer[3] = (char)(0x80 | (Codepoint & 0x3F));
            }
            return FString(buffer);
        }
        
        EVENT_CLASS_TYPE(CharInput)
        
    private:
        uint32 Codepoint;
    };

    class FCharInputModsEvent : public FEvent
    {
    public:
        FCharInputModsEvent(uint32 InCodepoint, bool InCtrl, bool InShift, bool InAlt, bool InSuper)
            : Codepoint(InCodepoint), bCtrl(InCtrl), bShift(InShift), bAlt(InAlt), bSuper(InSuper) {}
        
        uint32 GetCodepoint() const { return Codepoint; }
        bool IsCtrlDown() const { return bCtrl; }
        bool IsShiftDown() const { return bShift; }
        bool IsAltDown() const { return bAlt; }
        bool IsSuperDown() const { return bSuper; }
        
        EVENT_CLASS_TYPE(CharInputMods)
        
    private:
        uint32 Codepoint;
        bool bCtrl;
        bool bShift;
        bool bAlt;
        bool bSuper;
    };

    class FMouseMovedEvent : public FEvent
    {
    public:
        FMouseMovedEvent(float InX, float InY, float InDeltaX, float InDeltaY)
            : X(InX), Y(InY), DeltaX(InDeltaX), DeltaY(InDeltaY) {}
        
        float GetX() const { return X; }
        float GetY() const { return Y; }
        float GetDeltaX() const { return DeltaX; }
        float GetDeltaY() const { return DeltaY; }
        
        EVENT_CLASS_TYPE(MouseMoved)
        
    private:
        float X, Y;
        float DeltaX, DeltaY;
    };

    class FMouseScrolledEvent : public FEvent
    {
    public:
        FMouseScrolledEvent(EMouseKey InCode, float InXOffset)
            : Offset(InXOffset), Code(InCode) {}
        
        EMouseKey GetCode() const { return Code; }
        float GetOffset() const { return Offset; }
        
        EVENT_CLASS_TYPE(MouseScrolled)
        
    private:
        EMouseKey Code; 
        float Offset;
    };

    class FMouseButtonEvent : public FEvent
    {
    public:
        EMouseKey GetButton() const { return Button; }
        float GetX() const { return X; }
        float GetY() const { return Y; }
        
    protected:
        FMouseButtonEvent(EMouseKey InButton, float InX, float InY)
            : Button(InButton), X(InX), Y(InY) {}
        
        EMouseKey Button;
        float X, Y;
    };

    class FMouseButtonPressedEvent : public FMouseButtonEvent
    {
    public:
        FMouseButtonPressedEvent(EMouseKey InButton, float InX, float InY)
            : FMouseButtonEvent(InButton, InX, InY) {}
        
        EVENT_CLASS_TYPE(MouseButtonPressed)
    };

    class FMouseButtonReleasedEvent : public FMouseButtonEvent
    {
    public:
        FMouseButtonReleasedEvent(EMouseKey InButton, float InX, float InY)
            : FMouseButtonEvent(InButton, InX, InY) {}
        
        EVENT_CLASS_TYPE(MouseButtonReleased)
    };

    class FMouseEnteredEvent : public FEvent
    {
    public:
        EVENT_CLASS_TYPE(MouseEntered)
    };

    class FMouseLeftEvent : public FEvent
    {
    public:
        EVENT_CLASS_TYPE(MouseLeft)
    };

    class FWindowResizeEvent : public FEvent
    {
    public:
        FWindowResizeEvent(uint32 InWidth, uint32 InHeight)
            : Width(InWidth), Height(InHeight) {}
        
        uint32 GetWidth() const { return Width; }
        uint32 GetHeight() const { return Height; }
        
        EVENT_CLASS_TYPE(WindowResize)
        
    private:
        uint32 Width, Height;
    };

    class FWindowCloseEvent : public FEvent
    {
    public:
        EVENT_CLASS_TYPE(WindowClose)
    };

    class FWindowFocusEvent : public FEvent
    {
    public:
        EVENT_CLASS_TYPE(WindowFocus)
    };

    class FWindowLostFocusEvent : public FEvent
    {
    public:
        EVENT_CLASS_TYPE(WindowLostFocus)
    };

    class FWindowMovedEvent : public FEvent
    {
    public:
        FWindowMovedEvent(int32 InX, int32 InY)
            : X(InX), Y(InY) {}
        
        int32 GetX() const { return X; }
        int32 GetY() const { return Y; }
        
        EVENT_CLASS_TYPE(WindowMoved)
        
    private:
        int32 X, Y;
    };

    class FWindowMaximizedEvent : public FEvent
    {
    public:
        EVENT_CLASS_TYPE(WindowMaximized)
    };

    class FWindowMinimizedEvent : public FEvent
    {
    public:
        EVENT_CLASS_TYPE(WindowMinimized)
    };

    class FWindowRestoredEvent : public FEvent
    {
    public:
        EVENT_CLASS_TYPE(WindowRestored)
    };

    class FWindowRefreshEvent : public FEvent
    {
    public:
        EVENT_CLASS_TYPE(WindowRefresh)
    };

    class FWindowContentScaleChangedEvent : public FEvent
    {
    public:
        FWindowContentScaleChangedEvent(float InXScale, float InYScale)
            : XScale(InXScale), YScale(InYScale) {}
        
        float GetXScale() const { return XScale; }
        float GetYScale() const { return YScale; }
        
        EVENT_CLASS_TYPE(WindowContentScaleChanged)
        
    private:
        float XScale, YScale;
    };
    

    class FFileDropEvent : public FEvent
    {
    public:
        FFileDropEvent(const TVector<FFixedString>& InPaths, float InMouseX, float InMouseY)
            : Paths(InPaths)
            , MouseX(InMouseX)
            , MouseY(InMouseY)
        {}
        
        const TVector<FFixedString>& GetPaths() const { return Paths; }
        size_t GetCount() const { return Paths.size(); }
        float GetMouseX() const { return MouseX; }
        float GetMouseY() const { return MouseY; }
        
        EVENT_CLASS_TYPE(FileDrop)
        
    private:
        
        TVector<FFixedString> Paths;
        float MouseX, MouseY;
    };

    class FJoystickConnectedEvent : public FEvent
    {
    public:
        FJoystickConnectedEvent(int32 InJoystickID)
            : JoystickID(InJoystickID) {}
        
        int32 GetJoystickID() const { return JoystickID; }
        
        EVENT_CLASS_TYPE(JoystickConnected)
        
    private:
        int32 JoystickID;
    };

    class FJoystickDisconnectedEvent : public FEvent
    {
    public:
        FJoystickDisconnectedEvent(int32 InJoystickID)
            : JoystickID(InJoystickID) {}
        
        int32 GetJoystickID() const { return JoystickID; }
        
        EVENT_CLASS_TYPE(JoystickDisconnected)
        
    private:
        int32 JoystickID;
    };

    class FAppTickEvent : public FEvent
    {
    public:
        EVENT_CLASS_TYPE(AppTick)
    };

    class FAppUpdateEvent : public FEvent
    {
    public:
        FAppUpdateEvent(float InDeltaTime)
            : DeltaTime(InDeltaTime) {}
        
        float GetDeltaTime() const { return DeltaTime; }
        
        EVENT_CLASS_TYPE(AppUpdate)
        
    private:
        float DeltaTime;
    };

    class FAppRenderEvent : public FEvent
    {
    public:
        EVENT_CLASS_TYPE(AppRender)
    };
}
