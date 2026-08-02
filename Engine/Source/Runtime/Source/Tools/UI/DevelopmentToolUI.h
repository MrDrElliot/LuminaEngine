#pragma once
#include "Core/UpdateContext.h"
#include "Events/EventProcessor.h"


namespace Lumina
{
    class FPropertyCustomizationRegistry;
    class IImGuiRenderer;
    class FUpdateContext;
}


namespace Lumina
{
    class RUNTIME_API IDevelopmentToolUI : public IEventHandler
    {
    public:
        IDevelopmentToolUI() = default;
        IDevelopmentToolUI(const IDevelopmentToolUI&) = default;
        virtual ~IDevelopmentToolUI() = default;

        virtual void Initialize(const FUpdateContext& UpdateContext) = 0;
        virtual void Deinitialize(const FUpdateContext& UpdateContext) = 0;

        /** Called at the very beginning of the frame just after rendering kick-off. */
        void StartFrame(const FUpdateContext& UpdateContext);

        /** Called before every stage of scene updating */
        void Update(const FUpdateContext& UpdateContext);

        /** Called right before renderer present, not safe to modify any scene data here */
        void EndFrame(const FUpdateContext& UpdateContext);


        virtual void OnStartFrame(const FUpdateContext& UpdateContext)  { }
        virtual void OnUpdate(const FUpdateContext& UpdateContext)      { }
        virtual void OnEndFrame(const FUpdateContext& UpdateContext)    { }

        FPropertyCustomizationRegistry* GetPropertyCustomizationRegistry() const { return PropertyCustomizationRegistry; }
        
    protected:

        FPropertyCustomizationRegistry* PropertyCustomizationRegistry = nullptr;

    };
    
}
