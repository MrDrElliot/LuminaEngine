#pragma once

#include "Core/LuminaMacros.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CMesh;
    namespace ECS { class FRegistry; }

    // Mesh builds asked for on the game thread while a scope is open finish together, in parallel, when it closes.
    class RUNTIME_API FMeshBuildBatchScope
    {
    public:

        explicit FMeshBuildBatchScope(ECS::FRegistry& InRegistry);
        ~FMeshBuildBatchScope();
        LE_NO_COPYMOVE(FMeshBuildBatchScope);

        // True on the thread that opened a scope, which is the only thread whose builds wait for it.
        static bool IsOpen();

        // Queues the meshlet build and upload of a mesh whose resource is already set, for the outermost scope.
        static void DeferMeshlets(CMesh* Mesh);

    private:

        ECS::FRegistry& Registry;
    };
}
