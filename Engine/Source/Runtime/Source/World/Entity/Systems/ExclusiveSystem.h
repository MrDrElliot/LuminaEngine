#pragma once

#include "EntitySystem.h"
#include "ExclusiveSystem.generated.h"

namespace Lumina
{
    // Runs alone on the main thread with the whole world in reach, for work that may touch anything, such as dispatching into scripts.
    REFLECT(Scriptable)
    class RUNTIME_API CExclusiveSystem : public CEntitySystem
    {
        GENERATED_BODY()

    public:

        CExclusiveSystem() { bExclusiveClass = true; }

        // The owning world, valid from Configure onwards; only a system running alone may reach it.
        FUNCTION()
        CWorld* GetWorld() const { return GetOwningWorld(); }

        FUNCTION()
        virtual void OnExclusiveUpdate(const SExclusiveContext& Context) {}

        void OnUpdate(const SSystemContext& Context) override;
    };
}
