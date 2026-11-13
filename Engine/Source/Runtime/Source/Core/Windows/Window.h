#pragma once

#include "WindowTypes.h"
#include "WindowMode.h"
#include "Containers/Vector.h"
#include "WindowInput.h"
#include "Core/Delegates/Delegate.h"
#include "Memory/SmartPtr.h"

// Opaque GLFW handle; full glfw3.h is included only in TUs that need it (via GLFWInclude.h).
struct GLFWwindow;

namespace Lumina
{
	class FWindow;
	struct FWindowImpl;

	DECLARE_MULTICAST_DELEGATE(FWindowResizeDelegate, FWindow*, const FUIntVector2&);

	class FWindow
	{
	public:

		RUNTIME_API FWindow(const FWindowSpecs& InSpecs);
		RUNTIME_API virtual ~FWindow();
		LE_NO_COPYMOVE(FWindow);

		RUNTIME_API void ProcessMessages();

		// Reads the OS events that arrived since ProcessMessages and holds every one for the next call, so only their mouse motion is seen early.
		RUNTIME_API void PumpLateMessages();

		// The mouse motion PumpLateMessages held, which the next ProcessMessages still delivers.
		RUNTIME_API FVector2 GetLateMouseDelta() const;

		// Native handle for code that genuinely needs it (Vulkan surface, ImGui GLFW backend).
		RUNTIME_API GLFWwindow* GetWindow() const;

		RUNTIME_API FUIntVector2 GetExtent() const;
		RUNTIME_API uint32 GetWidth() const;
		RUNTIME_API uint32 GetHeight() const;

		// Monitor content scale (1.0 = 96 DPI). Drives editor UI / ImGui DPI scaling.
		RUNTIME_API float GetContentScale() const;

		// Resolution of the monitor the window currently sits on (pixels).
		RUNTIME_API FUIntVector2 GetMonitorResolution() const;

		// Refresh rate of the monitor the window sits on, in Hz, or zero when the platform cannot say.
		RUNTIME_API float GetRefreshRate() const;

		RUNTIME_API void GetWindowPosition(int& X, int& Y);
		RUNTIME_API void SetWindowPosition(int X, int Y);

		RUNTIME_API void SetWindowSize(int X, int Y);

		// A zero Resolution means the monitor's own; RefreshRate only matters in exclusive Fullscreen, where zero picks the highest.
		RUNTIME_API void SetWindowMode(EWindowMode Mode, FUIntVector2 Resolution = FUIntVector2(0), int32 RefreshRate = 0);
		RUNTIME_API EWindowMode GetWindowMode() const;

		// Every resolution and refresh rate the window's current monitor offers, widest first.
		RUNTIME_API TVector<SDisplayMode> GetDisplayModes() const;

		RUNTIME_API void SetTitle(const FString& Title);

		// Keeps the current icon when Path is empty or LoadWindowIcon fails.
		RUNTIME_API void SetIcon(FStringView Path);
		RUNTIME_API void SetTitleBarHovered(bool bHovered);
		RUNTIME_API void SetCursorMode(ECursorMode Mode);

		RUNTIME_API bool ShouldClose() const;
		RUNTIME_API bool IsWindowMinimized() const;
		RUNTIME_API bool IsWindowMaximized() const;
		RUNTIME_API void Minimize();
		RUNTIME_API void Restore();
		RUNTIME_API void Maximize();
		RUNTIME_API void Close();

		// Clears the close flag set by the OS (X button, Alt+F4) to keep the editor alive,
		// e.g. when the user cancels the dirty-packages prompt.
		RUNTIME_API void CancelClose();

		RUNTIME_API static FWindowResizeDelegate OnWindowResized;

		//~ Per window input. Subscribe directly; a host needs no application object to read the keyboard.

		FWindowKeyDelegate         OnKey;
		FWindowCharDelegate        OnChar;
		FWindowMouseButtonDelegate OnMouseButton;
		FWindowMouseMoveDelegate   OnMouseMove;
		FWindowScrollDelegate      OnScroll;
		FWindowFileDropDelegate    OnFileDrop;
		FWindowCloseDelegate       OnCloseRequested;

	private:

		void Init();

		TUniquePtr<FWindowImpl> Impl;
	};

	namespace Global
	{
		RUNTIME_API extern FWindow* PrimaryWindow;
	}

	namespace Windowing
	{
		// Asserts when there is none. A host that may not have published one uses TryGet instead.
		RUNTIME_API FWindow* GetPrimaryWindowHandle();

		RUNTIME_API FWindow* TryGetPrimaryWindowHandle();
		void SetPrimaryWindowHandle(FWindow* InWindow);

		// Null falls back to the primary window. Raw motion only applies while disabled and skips the desktop's pointer acceleration.
		RUNTIME_API void SetCursorModeForNativeWindow(void* NativeWindow, ECursorMode Mode, bool bRawMotion = false);

		// Authoritative OS-level focus for a native window (GLFWwindow*). Exactly one window is focused at a
		// time, so this disambiguates the active viewport across separate preview windows under game input.
		RUNTIME_API bool IsNativeWindowFocused(void* NativeWindow);

		// A gamepad slot read through the platform's standard mapping, with axes in the device's raw -1 to 1 range.
		struct FRawGamepad
		{
			float       Axes[6] = {};
			uint8       Buttons[15] = {};
			const char* Name = nullptr;
		};

		struct FWindowIcon
		{
			TVector<uint8> Pixels;
			int32          Width = 0;
			int32          Height = 0;
		};

		// Decodes a PNG at a virtual or disk path to RGBA8, warning with the path when it cannot be read or decoded.
		RUNTIME_API bool LoadWindowIcon(FStringView Path, FWindowIcon& Out);

		// False when the slot is empty or holds a device with no standard gamepad mapping.
		RUNTIME_API bool ReadGamepad(int32 Index, FRawGamepad& Out);
	}

}
