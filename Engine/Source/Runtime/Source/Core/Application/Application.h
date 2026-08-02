#pragma once

#include "Core/LuminaMacros.h"
#include "Core/Engine/Engine.h"
#include "Events/EventProcessor.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
	struct FWindowSpecs;
	class  FInputViewport;

	enum class RUNTIME_API EApplicationFlags : uint32
	{
		DevelopmentTools =		1 << 0,
	};

	constexpr EApplicationFlags operator|(EApplicationFlags lhs, EApplicationFlags rhs)
	{
		return static_cast<EApplicationFlags>(static_cast<uint32>(lhs) | static_cast<uint32>(rhs));
	}

	constexpr EApplicationFlags operator&(EApplicationFlags lhs, EApplicationFlags rhs)
	{
		return static_cast<EApplicationFlags>(static_cast<uint32>(lhs) & static_cast<uint32>(rhs));
	}
	
	class RUNTIME_API FApplication
	{
	public:

		FApplication();
		~FApplication();
		LE_NO_COPYMOVE(FApplication);
		
		int32 Run(int argc, char** argv);

		void Shutdown();
		
		void WindowResized(FWindow* Window, const FUIntVector2& Extent);

		static void RequestExit();

		// Abort a pending exit; clears bExitRequested AND the window close flag
		// so an X-button-triggered exit can be cancelled too.
		static void CancelExit();

		FEventProcessor& GetEventProcessor()	{ return EventProcessor; }

		// Null in editor builds; per-tool viewports own input there.
		FInputViewport* GetPrimaryViewport()	{ return PrimaryViewport.get(); }

	private:

		void PreInitStartup();
		bool CreateApplicationWindow();

		bool ShouldExit() const;

	protected:

		FEventProcessor				EventProcessor;
		FWindow*					MainWindow = nullptr;
		TUniquePtr<FInputViewport>	PrimaryViewport;

		bool bExitRequested			= false;
	
	public:

	};
	
	RUNTIME_API extern FApplication* GApp;


	/* Implemented by client */
	static FApplication* CreateApplication(int argc, char** argv);
}
