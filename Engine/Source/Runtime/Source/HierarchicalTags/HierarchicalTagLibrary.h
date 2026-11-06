#pragma once

#include "Containers/String.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "HierarchicalTagLibrary.generated.h"

namespace Lumina
{
    // The process-global tag registry, while tags on an entity live on CWorld.
    REFLECT()
    class RUNTIME_API CHierarchicalTagLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        // Interns the name and all of its ancestors, returning zero for an empty name.
        FUNCTION()
        static uint32 RequestTag(const FString& Name);

        // Empty when the id names nothing.
        FUNCTION()
        static FString GetTagName(uint32 TagId);

        // A matches B when A is B or a descendant of it.
        FUNCTION()
        static bool Matches(uint32 A, uint32 B);

        FUNCTION()
        static bool MatchesExact(uint32 A, uint32 B);

        // Zero at the root.
        FUNCTION()
        static uint32 GetParent(uint32 TagId);

        FUNCTION()
        static bool IsValidTag(uint32 TagId);
    };
}
