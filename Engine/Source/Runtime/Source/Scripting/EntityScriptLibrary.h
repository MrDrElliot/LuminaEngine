#pragma once

#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/SubclassOf.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Scripting/EntityScript.h"
#include "World/ECS/Entity.h"
#include "EntityScriptLibrary.generated.h"

namespace Lumina
{
    class CWorld;

    // Keyed on the script's CClass, so the same call finds a C++ script and a C# one alike.
    REFLECT()
    class RUNTIME_API CEntityScriptLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        /** Attaches a script of the named class and returns it, or null if the class is not a CEntityScript. */
        FUNCTION()
        static CEntityScript* AddScript(CWorld* World, ECS::FEntity Entity, TSubclassOf<CEntityScript> ScriptClass);

        /** The first script on the entity whose class derives the named one, or null. */
        FUNCTION()
        static CEntityScript* FindScript(CWorld* World, ECS::FEntity Entity, TSubclassOf<CEntityScript> ScriptClass);

        /** Every script on the entity whose class derives the named one. */
        FUNCTION()
        static void FindScripts(CWorld* World, ECS::FEntity Entity, TSubclassOf<CEntityScript> ScriptClass,
            TVector<TObjectPtr<CEntityScript>>& OutScripts);

        /** Runs OnDetach and drops the script. False when it was not attached to this entity. */
        FUNCTION()
        static bool RemoveScript(CWorld* World, ECS::FEntity Entity, CEntityScript* Script);
    };
}
