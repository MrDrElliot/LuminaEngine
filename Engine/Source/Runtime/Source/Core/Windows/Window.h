#pragma once

#include "WindowTypes.h"
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

		FWindow(const FWindowSpecs& InSpecs);
		virtual ~FWindow();
		LE_NO_COPYMOVE(FWindow);

		void Init();
		void ProcessMessages();

		// Native handle for code that genuinely needs it (Vulkan surface, ImGui GLFW backend).
		GLFWwindow* GetWindow() const;

		RUNTIME_API FUIntVector2 GetExtent() const;
		RUNTIME_API uint32 GetWidth() const;
		RUNTIME_API uint32 GetHeight() const;

		// Monitor content scale (1.0 = 96 DPI). Drives editor UI / ImGui DPI scaling.
		RUNTIME_API float GetContentScale() const;

		// Resolution of the monitor the window currently sits on (pixels).
		RUNTIME_API FUIntVector2 GetMonitorResolution() const;

		RUNTIME_API void GetWindowPosition(int& X, int& Y);
		RUNTIME_API void SetWindowPosition(int X, int Y);

		RUNTIME_API void SetWindowSize(int X, int Y);

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

	private:

		TUniquePtr<FWindowImpl> Impl;
	};

	namespace Windowing
	{
		RUNTIME_API extern FWindow* PrimaryWindow;
		RUNTIME_API FWindow* GetPrimaryWindowHandle();
		void SetPrimaryWindowHandle(FWindow* InWindow);

		// Apply a cursor mode to a specific native window (GLFWwindow*); null falls back to the primary.
		// Lets mouse capture target the focused PIE preview window rather than always the primary window.
		RUNTIME_API void SetCursorModeForNativeWindow(void* NativeWindow, ECursorMode Mode);

		// Authoritative OS-level focus for a native window (GLFWwindow*). Exactly one window is focused at a
		// time, so this disambiguates the active viewport across separate preview windows under game input.
		RUNTIME_API bool IsNativeWindowFocused(void* NativeWindow);
	}

}
