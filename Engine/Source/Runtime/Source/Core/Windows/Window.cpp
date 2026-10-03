#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "Window.h"
#include "Core/Windows/GLFWInclude.h"
#include "stb_image.h"
#include "Core/Application/Application.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Events/Event.h"
#include "Paths/Paths.h"
#include "Platform/Platform.h"
#include "Log/Log.h"

namespace
{
	void GLFWErrorCallback(int error, const char* description)
	{
		// 65540 = invalid scancode; spammed by some keyboard layouts.
		if (error == 65540)
		{
			return;
		}
		LOG_CRITICAL("GLFW Error: {0} | {1}", error, description);
	}

	void* CustomGLFWAllocate(size_t size, void* user)
	{
		LUMINA_MEMORY_SCOPE("Windowing");
		return Lumina::Memory::Malloc(size);
	}

	void* CustomGLFWReallocate(void* block, size_t size, void* user)
	{
		LUMINA_MEMORY_SCOPE("Windowing");
		return Lumina::Memory::Realloc(block, size);
	}

	void CustomGLFWDeallocate(void* block, void* user)
	{
		Lumina::Memory::Free(block);
	}

	GLFWallocator CustomAllocator =
	{
		CustomGLFWAllocate,
		CustomGLFWReallocate,
		CustomGLFWDeallocate,
		nullptr
	};
}


namespace Lumina
{
	// All GLFW-facing state lives here so glfw3.h never reaches the public header.
	struct FWindowImpl
	{
		GLFWwindow*		Window = nullptr;
		FWindow*		Owner = nullptr;
		FWindowSpecs	Specs;
		double			LastMouseX = 0.0;
		double			LastMouseY = 0.0;
		bool			bFirstMouseUpdate = true;
		bool			bInitialized = false;
		bool			bTitleBarHovered = false;

		EWindowMode		Mode = EWindowMode::Windowed;

		// Where the window sat before it went fullscreen, so returning to Windowed puts it back.
		int				WindowedX = 0;
		int				WindowedY = 0;
		int				WindowedWidth = 0;
		int				WindowedHeight = 0;
	};

	FWindowResizeDelegate FWindow::OnWindowResized;

	namespace
	{
		FWindowImpl* ImplFrom(GLFWwindow* Window)
		{
			return static_cast<FWindowImpl*>(glfwGetWindowUserPointer(Window));
		}

		FWindow* OwnerFrom(GLFWwindow* Window)
		{
			const FWindowImpl* Impl = ImplFrom(Window);
			return Impl != nullptr ? Impl->Owner : nullptr;
		}

		void MouseButtonCallback(GLFWwindow* Window, int Button, int Action, int /*Mods*/)
		{
			FWindow* Owner = OwnerFrom(Window);
			if (Owner == nullptr || (Action != GLFW_PRESS && Action != GLFW_RELEASE))
			{
				return;
			}

			double xpos, ypos;
			glfwGetCursorPos(Window, &xpos, &ypos);

			FMouseButtonInput Input;
			Input.Button   = static_cast<EMouseKey>(Button);
			Input.bPressed = Action == GLFW_PRESS;
			Input.X        = (float)xpos;
			Input.Y        = (float)ypos;

			Owner->OnMouseButton.Broadcast(Owner, Input);
		}

		void MousePosCallback(GLFWwindow* Window, double xpos, double ypos)
		{
			FWindowImpl* Impl = ImplFrom(Window);
			if (Impl == nullptr)
			{
				return;
			}

			FMouseMoveInput Input;
			Input.X = (float)xpos;
			Input.Y = (float)ypos;

			if (!Impl->bFirstMouseUpdate)
			{
				Input.DeltaX = (float)(xpos - Impl->LastMouseX);
				Input.DeltaY = (float)(ypos - Impl->LastMouseY);
			}

			Impl->LastMouseX = xpos;
			Impl->LastMouseY = ypos;
			Impl->bFirstMouseUpdate = false;

			Impl->Owner->OnMouseMove.Broadcast(Impl->Owner, Input);
		}

		void MouseScrollCallback(GLFWwindow* Window, double /*xoffset*/, double yoffset)
		{
			FWindow* Owner = OwnerFrom(Window);
			if (Owner == nullptr)
			{
				return;
			}

			// Vertical scroll only.
			Owner->OnScroll.Broadcast(Owner, FMouseScrollInput{ (float)yoffset });
		}

		void KeyCallback(GLFWwindow* Window, int Key, int /*Scancode*/, int Action, int Mods)
		{
			FWindow* Owner = OwnerFrom(Window);
			if (Owner == nullptr || Key == GLFW_KEY_UNKNOWN)
			{
				return;
			}

			FKeyInput Input;
			Input.Key      = static_cast<EKey>(Key);
			Input.bPressed = Action != GLFW_RELEASE;
			Input.bRepeat  = Action == GLFW_REPEAT;
			Input.bCtrl    = (Mods & GLFW_MOD_CONTROL) != 0;
			Input.bShift   = (Mods & GLFW_MOD_SHIFT) != 0;
			Input.bAlt     = (Mods & GLFW_MOD_ALT) != 0;
			Input.bSuper   = (Mods & GLFW_MOD_SUPER) != 0;

			Owner->OnKey.Broadcast(Owner, Input);
		}

		void CharCallback(GLFWwindow* Window, unsigned int Codepoint)
		{
			if (FWindow* Owner = OwnerFrom(Window))
			{
				Owner->OnChar.Broadcast(Owner, Codepoint);
			}
		}

		void WindowResizeCallback(GLFWwindow* Window, int width, int height)
		{
			FWindowImpl* Impl = ImplFrom(Window);
			Impl->Specs.Extent.x = width;
			Impl->Specs.Extent.y = height;

			FWindow::OnWindowResized.Broadcast(Impl->Owner, Impl->Specs.Extent);
		}

		void WindowDropCallback(GLFWwindow* Window, int PathCount, const char* Paths[])
		{
			double xpos, ypos;
			glfwGetCursorPos(Window, &xpos, &ypos);

			TVector<FFixedString> StringPaths;

			for (int i = 0; i < PathCount; ++i)
			{
				StringPaths.emplace_back(Paths[i]);
			}

			if (FWindow* Owner = OwnerFrom(Window))
			{
				Owner->OnFileDrop.Broadcast(Owner, StringPaths, static_cast<float>(xpos), static_cast<float>(ypos));
			}
		}

		void WindowCloseCallback(GLFWwindow* Window)
		{
			if (FWindow* Owner = OwnerFrom(Window))
			{
				Owner->OnCloseRequested.Broadcast(Owner);
			}
		}

		void TitleBarHitTestCallback(GLFWwindow* Window, int /*x*/, int /*y*/, int* hit)
		{
			const FWindowImpl* Impl = ImplFrom(Window);
			*hit = (Impl != nullptr && Impl->bTitleBarHovered) ? 1 : 0;
		}

		[[maybe_unused]] GLFWmonitor* GetCurrentMonitor(GLFWwindow* window)
		{
			int windowX, windowY, windowWidth, windowHeight;
			glfwGetWindowPos(window, &windowX, &windowY);
			glfwGetWindowSize(window, &windowWidth, &windowHeight);

			int monitorCount;
			GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);

			GLFWmonitor* bestMonitor = nullptr;
			int maxOverlap = 0;

			for (int i = 0; i < monitorCount; ++i)
			{
				int monitorX, monitorY, monitorWidth, monitorHeight;
				glfwGetMonitorWorkarea(monitors[i], &monitorX, &monitorY, &monitorWidth, &monitorHeight);

				int overlapX = Math::Max(0, Math::Min(windowX + windowWidth, monitorX + monitorWidth) - Math::Max(windowX, monitorX));
				int overlapY = Math::Max(0, Math::Min(windowY + windowHeight, monitorY + monitorHeight) - Math::Max(windowY, monitorY));
				int overlapArea = overlapX * overlapY;

				if (overlapArea > maxOverlap)
				{
					maxOverlap = overlapArea;
					bestMonitor = monitors[i];
				}
			}

			return bestMonitor;
		}
	}

	FWindow::FWindow(const FWindowSpecs& InSpecs)
		: Impl(MakeUnique<FWindowImpl>())
	{
		Impl->Owner = this;
		Impl->Specs = InSpecs;
		Init();
	}

	FWindow::~FWindow()
	{
		glfwDestroyWindow(Impl->Window);
		glfwTerminate();
	}

	void FWindow::Init()
	{
		if (LIKELY(!Impl->bInitialized))
		{
			glfwInitAllocator(&CustomAllocator);
			glfwInit();
			glfwSetErrorCallback(GLFWErrorCallback);

			glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
			glfwWindowHint(GLFW_TITLEBAR, Impl->Specs.bShowTitlebar ? GLFW_TRUE : GLFW_FALSE);

			GLFWmonitor* Monitor = glfwGetPrimaryMonitor();
			const GLFWvidmode* Mode = glfwGetVideoMode(Monitor);

			if (Impl->Specs.Extent.x == 0 || Impl->Specs.Extent.y == 0)
			{
				Impl->Specs.Extent.x = Mode->width - 300;
				Impl->Specs.Extent.y = Mode->height - 300;
			}

			Impl->Window = glfwCreateWindow(Impl->Specs.Extent.x, Impl->Specs.Extent.y, Impl->Specs.Title.c_str(), nullptr, nullptr);
			glfwSetWindowAttrib(Impl->Window, GLFW_RESIZABLE, GLFW_TRUE);
			
			glfwSetWindowSizeLimits(Impl->Window, 640, 360, GLFW_DONT_CARE, GLFW_DONT_CARE);
			

			const int PosX = (Mode->width  - (int)Impl->Specs.Extent.x) / 2;
			const int PosY = (Mode->height - (int)Impl->Specs.Extent.y) / 2;
			glfwSetWindowPos(Impl->Window, PosX, PosY);

			LOG_TRACE("Initializing Window: {} (Width: {}p Height: {}p)", Impl->Specs.Title, Impl->Specs.Extent.x, Impl->Specs.Extent.y);

			GLFWimage Icon;
			int Channels;
			FString IconPathStr = Paths::GetEngineResourceDirectory() + "/Textures/Lumina.png";
			Icon.pixels = stbi_load(IconPathStr.c_str(), &Icon.width, &Icon.height, &Channels, 4);
			if (Icon.pixels)
			{
				glfwSetWindowIcon(Impl->Window, 1, &Icon);
				stbi_image_free(Icon.pixels);
			}

			glfwSetWindowUserPointer(Impl->Window, Impl.get());
			glfwSetMouseButtonCallback(Impl->Window, MouseButtonCallback);
			glfwSetCursorPosCallback(Impl->Window, MousePosCallback);
			glfwSetScrollCallback(Impl->Window, MouseScrollCallback);
			glfwSetKeyCallback(Impl->Window, KeyCallback);
			glfwSetCharCallback(Impl->Window, CharCallback);
			glfwSetWindowSizeCallback(Impl->Window, WindowResizeCallback);
			glfwSetDropCallback(Impl->Window, WindowDropCallback);
			glfwSetWindowCloseCallback(Impl->Window, WindowCloseCallback);

			if (!Impl->Specs.bShowTitlebar)
			{
				// Route title-bar hit-test through OS for native Aero Snap / drag-to-maximize.
				glfwSetTitlebarHitTestCallback(Impl->Window, TitleBarHitTestCallback);
			}
		}
	}

	void FWindow::ProcessMessages()
	{
		glfwPollEvents();
	}

	GLFWwindow* FWindow::GetWindow() const
	{
		return Impl->Window;
	}

	FUIntVector2 FWindow::GetExtent() const
	{
		FIntVector2 ReturnVal;
		glfwGetWindowSize(Impl->Window, &ReturnVal.x, &ReturnVal.y);

		return ReturnVal;
	}

	uint32 FWindow::GetWidth() const
	{
		return GetExtent().x;
	}

	float FWindow::GetContentScale() const
	{
		float XScale = 1.0f;
		float YScale = 1.0f;
		glfwGetWindowContentScale(Impl->Window, &XScale, &YScale);
		return Math::Max(XScale, YScale);
	}

	FUIntVector2 FWindow::GetMonitorResolution() const
	{
		GLFWmonitor* Monitor = GetCurrentMonitor(Impl->Window);
		if (Monitor == nullptr)
		{
			Monitor = glfwGetPrimaryMonitor();
		}
		if (const GLFWvidmode* Mode = glfwGetVideoMode(Monitor))
		{
			return FUIntVector2(Mode->width, Mode->height);
		}
		return FUIntVector2(1920, 1080);
	}

	float FWindow::GetRefreshRate() const
	{
		GLFWmonitor* Monitor = GetCurrentMonitor(Impl->Window);
		if (Monitor == nullptr)
		{
			Monitor = glfwGetPrimaryMonitor();
		}
		const GLFWvidmode* Mode = Monitor != nullptr ? glfwGetVideoMode(Monitor) : nullptr;
		return Mode != nullptr ? (float)Mode->refreshRate : 0.0f;
	}

	uint32 FWindow::GetHeight() const
	{
		return GetExtent().y;
	}

	bool FWindow::IsWindowMaximized() const
	{
		return glfwGetWindowAttrib(Impl->Window, GLFW_MAXIMIZED);
	}

	void FWindow::GetWindowPosition(int& X, int& Y)
	{
		glfwGetWindowPos(Impl->Window, &X, &Y);
	}

	void FWindow::SetWindowPosition(int X, int Y)
	{
		glfwSetWindowPos(Impl->Window, X, Y);
	}

	void FWindow::SetWindowSize(int X, int Y)
	{
		glfwSetWindowSize(Impl->Window, X, Y);
	}

	void FWindow::SetWindowMode(EWindowMode Mode, FUIntVector2 Resolution, int32 RefreshRate)
	{
		GLFWwindow* Window = Impl->Window;
		GLFWmonitor* Monitor = glfwGetWindowMonitor(Window);
		if (Monitor == nullptr)
		{
			Monitor = GetCurrentMonitor(Window);
		}
		if (Monitor == nullptr)
		{
			Monitor = glfwGetPrimaryMonitor();
		}
		const GLFWvidmode* Desktop = glfwGetVideoMode(Monitor);
		if (Desktop == nullptr)
		{
			return;
		}

		if (Impl->Mode == EWindowMode::Windowed)
		{
			glfwGetWindowPos(Window, &Impl->WindowedX, &Impl->WindowedY);
			glfwGetWindowSize(Window, &Impl->WindowedWidth, &Impl->WindowedHeight);
		}

		const int Width  = Resolution.x != 0 ? (int)Resolution.x : Desktop->width;
		const int Height = Resolution.y != 0 ? (int)Resolution.y : Desktop->height;

		int MonitorX = 0;
		int MonitorY = 0;
		glfwGetMonitorPos(Monitor, &MonitorX, &MonitorY);

		switch (Mode)
		{
		case EWindowMode::Fullscreen:
			glfwSetWindowMonitor(Window, Monitor, 0, 0, Width, Height, RefreshRate > 0 ? RefreshRate : GLFW_DONT_CARE);
			break;

		case EWindowMode::BorderlessFullscreen:
			glfwSetWindowAttrib(Window, GLFW_DECORATED, GLFW_FALSE);
			glfwSetWindowMonitor(Window, nullptr, MonitorX, MonitorY, Desktop->width, Desktop->height, GLFW_DONT_CARE);
			break;

		case EWindowMode::Windowed:
		{
			// An explicit size is centered on the monitor, and no size returns to where the window last was.
			const bool bExplicit = Resolution.x != 0 && Resolution.y != 0;
			const int TargetWidth  = bExplicit ? Width : (Impl->WindowedWidth > 0 ? Impl->WindowedWidth : Desktop->width - 300);
			const int TargetHeight = bExplicit ? Height : (Impl->WindowedHeight > 0 ? Impl->WindowedHeight : Desktop->height - 300);
			const int TargetX = bExplicit || Impl->WindowedWidth == 0 ? MonitorX + (Desktop->width - TargetWidth) / 2 : Impl->WindowedX;
			const int TargetY = bExplicit || Impl->WindowedHeight == 0 ? MonitorY + (Desktop->height - TargetHeight) / 2 : Impl->WindowedY;

			// Decorated first, since adding the frame afterward resizes the client area off the requested size.
			glfwSetWindowAttrib(Window, GLFW_DECORATED, GLFW_TRUE);
			glfwSetWindowMonitor(Window, nullptr, TargetX, TargetY, TargetWidth, TargetHeight, GLFW_DONT_CARE);
			glfwSetWindowSize(Window, TargetWidth, TargetHeight);
			break;
		}
		}

		Impl->Mode = Mode;
	}

	EWindowMode FWindow::GetWindowMode() const
	{
		return Impl->Mode;
	}

	TVector<SDisplayMode> FWindow::GetDisplayModes() const
	{
		TVector<SDisplayMode> Modes;

		GLFWmonitor* Monitor = glfwGetWindowMonitor(Impl->Window);
		if (Monitor == nullptr)
		{
			Monitor = GetCurrentMonitor(Impl->Window);
		}
		if (Monitor == nullptr)
		{
			Monitor = glfwGetPrimaryMonitor();
		}

		int Count = 0;
		const GLFWvidmode* VideoModes = Monitor != nullptr ? glfwGetVideoModes(Monitor, &Count) : nullptr;
		for (int Index = 0; Index < Count; ++Index)
		{
			SDisplayMode Mode;
			Mode.Width = VideoModes[Index].width;
			Mode.Height = VideoModes[Index].height;
			Mode.RefreshRate = VideoModes[Index].refreshRate;

			// The same size and rate repeats once per bit depth, which a settings menu has no use for.
			const bool bSeen = Algo::AnyOf(Modes, [&Mode](const SDisplayMode& Existing)
			{
				return Existing.Width == Mode.Width && Existing.Height == Mode.Height && Existing.RefreshRate == Mode.RefreshRate;
			});
			if (!bSeen)
			{
				Modes.push_back(Mode);
			}
		}

		Algo::Sort(Modes, [](const SDisplayMode& A, const SDisplayMode& B)
		{
			if (A.Width != B.Width)
			{
				return A.Width > B.Width;
			}
			if (A.Height != B.Height)
			{
				return A.Height > B.Height;
			}
			return A.RefreshRate > B.RefreshRate;
		});
		return Modes;
	}

	void FWindow::SetTitle(const FString& Title)
	{
		Impl->Specs.Title = Title;
		glfwSetWindowTitle(Impl->Window, Title.c_str());
	}

	void FWindow::SetTitleBarHovered(bool bHovered)
	{
		Impl->bTitleBarHovered = bHovered;
	}

	void FWindow::SetCursorMode(ECursorMode Mode)
	{
		int Value = GLFW_CURSOR_NORMAL;
		switch (Mode)
		{
		case ECursorMode::Normal:   Value = GLFW_CURSOR_NORMAL;   break;
		case ECursorMode::Hidden:   Value = GLFW_CURSOR_HIDDEN;   break;
		case ECursorMode::Disabled: Value = GLFW_CURSOR_DISABLED; break;
		}
		glfwSetInputMode(Impl->Window, GLFW_CURSOR, Value);
	}

	bool FWindow::ShouldClose() const
	{
		return glfwWindowShouldClose(Impl->Window);
	}

	bool FWindow::IsWindowMinimized() const
	{
		return glfwGetWindowAttrib(Impl->Window, GLFW_ICONIFIED);
	}

	void FWindow::Minimize()
	{
		glfwIconifyWindow(Impl->Window);
	}

	void FWindow::Restore()
	{
		glfwRestoreWindow(Impl->Window);
	}

	void FWindow::Maximize()
	{
		glfwMaximizeWindow(Impl->Window);
	}

	void FWindow::Close()
	{
		glfwSetWindowShouldClose(Impl->Window, GLFW_TRUE);
	}

	void FWindow::CancelClose()
	{
		glfwSetWindowShouldClose(Impl->Window, GLFW_FALSE);
	}

	namespace Windowing
	{
		FWindow* PrimaryWindow = nullptr;

		FWindow* GetPrimaryWindowHandle()
		{
			ASSERT(PrimaryWindow != nullptr);
			return PrimaryWindow;
		}

		FWindow* TryGetPrimaryWindowHandle()
		{
			return PrimaryWindow;
		}

		void SetPrimaryWindowHandle(FWindow* InWindow)
		{
			ASSERT(PrimaryWindow == nullptr);
			PrimaryWindow = InWindow;
		}

		void SetCursorModeForNativeWindow(void* NativeWindow, ECursorMode Mode)
		{
			GLFWwindow* Window = static_cast<GLFWwindow*>(NativeWindow);
			if (Window == nullptr)
			{
				Window = PrimaryWindow ? PrimaryWindow->GetWindow() : nullptr;
			}
			if (Window == nullptr)
			{
				return;
			}

			int Value = GLFW_CURSOR_NORMAL;
			switch (Mode)
			{
			case ECursorMode::Normal:   Value = GLFW_CURSOR_NORMAL;   break;
			case ECursorMode::Hidden:   Value = GLFW_CURSOR_HIDDEN;   break;
			case ECursorMode::Disabled: Value = GLFW_CURSOR_DISABLED; break;
			}
			glfwSetInputMode(Window, GLFW_CURSOR, Value);
		}

		bool IsNativeWindowFocused(void* NativeWindow)
		{
			GLFWwindow* Window = static_cast<GLFWwindow*>(NativeWindow);
			if (Window == nullptr)
			{
				return false;
			}
			return glfwGetWindowAttrib(Window, GLFW_FOCUSED) != 0;
		}
	}
}
