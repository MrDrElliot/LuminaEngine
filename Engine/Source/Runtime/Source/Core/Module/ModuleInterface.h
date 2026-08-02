#pragma once
#include "Core/LuminaMacros.h"

namespace Lumina
{
    class RUNTIME_API IModuleInterface
    {
    public:

        IModuleInterface() = default;
        virtual ~IModuleInterface() = default;
        LE_NO_COPYMOVE(IModuleInterface);

        /** Called after the DLL has been loaded and module object has been created. */
        virtual void StartupModule() { }


        /** Called before the module is unloaded, right before the module object is destroyed */
        virtual void ShutdownModule() { }
        
    };
}