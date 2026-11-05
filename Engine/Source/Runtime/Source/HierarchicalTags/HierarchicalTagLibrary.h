#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "HierarchicalTagLibrary.generated.h"

namespace Lumina
{
    // The process-global tag registry, while tags on an entity live on CWorld.
    REFLECT()
    class RUNTIME_API CHierarchicalTagLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        //~ The process-global registry, where an id interns a dotted name and all of its ancestors.

        /** Interns the name and its ancestors and returns its id. Zero for an empty name. */
        FUNCTION()
        static uint32 RequestTag(const FString& Name);

        /** The dotted name behind an id, empty when the id names nothing. */
        FUNCTION()
        static FString GetTagName(uint32 TagId);

        /** Hierarchical, so A matches B when A is B or a descendant of it. */
        FUNCTION()
        static bool Matches(uint32 A, uint32 B);

        FUNCTION()
        static bool MatchesExact(uint32 A, uint32 B);

        /** The immediate parent id, or zero at the root. */
        FUNCTION()
        static uint32 GetParent(uint32 TagId);

        FUNCTION()
        static bool IsValidTag(uint32 TagId);
    };
}
