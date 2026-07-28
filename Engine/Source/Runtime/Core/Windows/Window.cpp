#include "pch.h"
#include "Window.h"
#include "Core/Windows/GLFWInclude.h"
#include "stb_image.h"
#include "Core/Application/Application.h"
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
		return Lumina::Memory::Malloc(size);
	}

	void* CustomGLFWReallocate(void* block, size_t size, void* user)
	{
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
	};

	FWindowResizeDelegate FWindow::OnWindowResized;

	namespace
	{
		FWindowImpl* ImplFrom(GLFWwindow* Window)
		{
			return static_cast<FWindowImpl*>(glfwGetWindowUserPointer(Window));
		}

		void MouseButtonCallback(GLFWwindow* Window, int Button, int Action, int /*Mods*/)
		{
			double xpos, ypos;
			glfwGetCursorPos(Window, &xpos, &ypos);

			switch (Action)
			{
			case GLFW_PRESS:
				GApp->GetEventProcessor().Dispatch<FMouseButtonPressedEvent>(static_cast<EMouseKey>(Button), xpos, ypos);
				break;

			case GLFW_RELEASE:
				GApp->GetEventProcessor().Dispatch<FMouseButtonReleasedEvent>(static_cast<EMouseKey>(Button), xpos, ypos);
				break;
			}
		}

		void MousePosCallback(GLFWwindow* Window, double xpos, double ypos)
		{
			FWindowImpl* Impl = ImplFrom(Window);

			if (Impl->bFirstMouseUpdate)
			{
				Impl->LastMouseX = xpos;
				Impl->LastMouseY = ypos;
				Impl->bFirstMouseUpdate = false;

				GApp->GetEventProcessor().Dispatch<FMouseMovedEvent>(xpos, ypos, 0.0, 0.0);
				return;
			}

			double DeltaX = xpos - Impl->LastMouseX;
			double DeltaY = ypos - Impl->LastMouseY;

			Impl->LastMouseX = xpos;
			Impl->LastMouseY = ypos;

			GApp->GetEventProcessor().Dispatch<FMouseMovedEvent>(xpos, ypos, DeltaX, DeltaY);
		}

		void MouseScrollCallback(GLFWwindow* /*Window*/, double /*xoffset*/, double yoffset)
		{
			// Vertical scroll only.
			GApp->GetEventProcessor().Dispatch<FMouseScrolledEvent>(EMouseKey::Scroll, yoffset);
		}

		void KeyCallback(GLFWwindow* /*Window*/, int Key, int /*Scancode*/, int Action, int Mods)
		{
			if (Key == GLFW_KEY_UNKNOWN)
			{
				return;
			}

			bool Ctrl = Mods & GLFW_MOD_CONTROL;
			bool Shift = Mods & GLFW_MOD_SHIFT;
			bool Alt = Mods & GLFW_MOD_ALT;
			bool Super = Mods & GLFW_MOD_SUPER;

			switch (Action)
			{
			case GLFW_RELEASE:
				GApp->GetEventProcessor().Dispatch<FKeyReleasedEvent>(static_cast<EKey>(Key), Ctrl, Shift, Alt, Super);
				break;
			case GLFW_PRESS:
				GApp->GetEventProcessor().Dispatch<FKeyPressedEvent>(static_cast<EKey>(Key), Ctrl, Shift, Alt, Super);
				break;
			case GLFW_REPEAT:
				GApp->GetEventProcessor().Dispatch<FKeyPressedEvent>(static_cast<EKey>(Key), Ctrl, Shift, Alt, Super, /* Repeat */ true);
				break;
			}
		}

		void WindowResizeCallback(GLFWwindow* Window, int width, int height)
		{
			FWindowImpl* Impl = ImplFrom(Window);
			Impl->Specs.Extent.x = width;
			Impl->Specs.Extent.y = height;

			GApp->GetEventProcessor().Dispatch<FWindowResizeEvent>(width, height);

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

			GApp->GetEventProcessor().Dispatch<FFileDropEvent>(StringPaths, static_cast<float>(xpos), static_cast<float>(ypos));
		}

		void WindowCloseCallback(GLFWwindow* /*Window*/)
		{
			FApplication::RequestExit();
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

				int overlapX = std::max(0, std::min(windowX + windowWidth, monitorX + monitorWidth) - std::max(windowX, monitorX));
				int overlapY = std::max(0, std::min(windowY + windowHeight, monitorY + monitorHeight) - std::max(windowY, monitorY));
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
#if WITH_EDITOR
			glfwWindowHint(GLFW_TITLEBAR, GLFW_FALSE);
#else
			glfwWindowHint(GLFW_TITLEBAR, GLFW_TRUE);

#endif
			GLFWmonitor* Monitor = glfwGetPrimaryMonitor();
			const GLFWvidmode* Mode = glfwGetVideoMode(Monitor);

			Impl->Specs.Extent.x = Mode->width - 300;
			Impl->Specs.Extent.y = Mode->height - 300;

			Impl->Window = glfwCreateWindow(Impl->Specs.Extent.x, Impl->Specs.Extent.y, Impl->Specs.Title.c_str(), nullptr, nullptr);
			glfwSetWindowAttrib(Impl->Window, GLFW_RESIZABLE, GLFW_TRUE);
			

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
			glfwSetWindowSizeCallback(Impl->Window, WindowResizeCallback);
			glfwSetDropCallback(Impl->Window, WindowDropCallback);
			glfwSetWindowCloseCallback(Impl->Window, WindowCloseCallback);

#if WITH_EDITOR
			// Route title-bar hit-test through OS for native Aero Snap / drag-to-maximize.
			glfwSetTitlebarHitTestCallback(Impl->Window, TitleBarHitTestCallback);
#endif
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
		return std::max(XScale, YScale);
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
		FWindow* PrimaryWindow;

		FWindow* GetPrimaryWindowHandle()
		{
			ASSERT(PrimaryWindow != nullptr);
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
