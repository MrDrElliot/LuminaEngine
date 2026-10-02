#pragma once

#include "Containers/String.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "GameplayProfilerLibrary.generated.h"

namespace Lumina
{
    /** Named CPU scopes for gameplay work, aggregated per name in the editor's profiler tool. */
    REFLECT()
    class RUNTIME_API CGameplayProfilerLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        FUNCTION()
        static void BeginScope(const FString& Name);

        // Interns a scope name once, so a hot sample opens by id without sending or copying its text.
        FUNCTION()
        static int32 RegisterScope(const FString& Name);

        FUNCTION()
        static void BeginRegisteredScope(int32 ScopeId);

        FUNCTION()
        static void EndScope();

        /** False when nobody is recording, which is when a scope costs almost nothing. */
        FUNCTION()
        static bool IsProfilerEnabled();
    };
}
