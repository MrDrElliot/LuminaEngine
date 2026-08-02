#pragma once

#include "Core/Module/ModuleInterface.h"

namespace Lumina
{
    class $EDITORMODULEUPPER_API F$EDITORMODULEModule : public IModuleInterface
    {
    public:
        void StartupModule()  override;
        void ShutdownModule() override;
    };
}
