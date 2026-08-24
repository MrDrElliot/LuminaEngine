#pragma once

#include "Core/Object/Object.h"
#include "GameInstance.generated.h"

namespace Lumina
{
    class FEngine;
    class CWorld;
    struct FWorldContext;
}

namespace Lumina
{
    // Persistent object that lives across world transitions and owns game-wide state.
    REFLECT(Scriptable)
    class RUNTIME_API CGameInstance : public CObject
    {
        GENERATED_BODY()
    public:

        // Called once after the GameInstance is constructed during project load.
        FUNCTION()
        virtual void Init();

        // Called once before the GameInstance is destroyed during engine shutdown / project unload.
        FUNCTION()
        virtual void Shutdown();

        // Called for every Game world (packaged boot, runtime travel, and editor PIE play) right BEFORE it is
        // initialized.
        FUNCTION()
        virtual void PreWorldLoad(CWorld* World) {}

        // Called for the same worlds right AFTER InitializeWorld.
        FUNCTION()
        virtual void PostWorldLoad(CWorld* World) {}
    };
}
