#include "Platform/GenericPlatform.h"
#include "World/ECS/Registry.h"
#include "Scripting/DotNet/LayoutRegistry.h"
#include "Containers/String.h"
#include "World/World.h"
#include "World/WorldContext.h"
#include "Physics/PhysicsScene.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Systems/SystemContext.h"
#include "World/Entity/Systems/NavMeshSystem.h"
#include "World/Entity/Systems/CameraSystem.h"
#include "World/Entity/Systems/SystemSingletons.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "AI/Navigation/NavTypes.h"
#include "GameplayTags/GameplayTagRegistry.h"
#include "GameplayTags/GameplayTagComponent.h"
#include "Core/Engine/Engine.h"
#include "Core/Engine/EngineURL.h"
#include "Core/Profiler/GameplayProfiler.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/ExportSignature.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Input/InputActionMap.h"
#include "Input/InputQuery.h"
#include "Input/InputViewport.h"
#include "Input/InputContext.h"
#include "Input/InputMode.h"
#include "Events/MouseCodes.h"

// World is an opaque pointer and Entity an entity id, matching the component ops convention.

using namespace Lumina;
using namespace Lumina::DotNet;   // AsWorld / AsEntity / ToId

// Only the not-yet-reflectable surface lives here, the rest is generated from CWorld's declarations.

// The world's own FSystemContext, so a managed system can be handed a valid context outside a tick.
LUMINA_DOTNET_EXPORT(const void*, World_GetSystemContext)(uint64 World)
{
    CWorld* W = AsWorld(World);
    return W ? &W->GetSystemContext() : nullptr;
}

// Game, the engine-level session operations.

// Valid only for the duration of the OnUpdate crossing, forwarding to the matching context method.

LUMINA_DOTNET_EXPORT(float, SystemContext_GetDeltaTime)(const FSystemContext* Ctx)
{
    return Ctx ? (float)Ctx->GetDeltaTime() : 0.0f;
}

LUMINA_DOTNET_EXPORT(double, SystemContext_GetTime)(const FSystemContext* Ctx)
{
    return Ctx ? Ctx->GetTime() : 0.0;
}

LUMINA_DOTNET_EXPORT(uint32, SystemContext_Create)(const FSystemContext* Ctx)
{
    return Ctx ? ToId(Ctx->Create()) : ToId(ECS::NullEntity);
}

LUMINA_DOTNET_EXPORT(void, SystemContext_Destroy)(const FSystemContext* Ctx, uint32 Entity)
{
    if (Ctx)
    {
        Ctx->Destroy(AsEntity(Entity));
    }
}

LUMINA_DOTNET_EXPORT(void, SystemContext_SetEntityLocation)(const FSystemContext* Ctx, uint32 Entity, FVector3 Location)
{
    // The scheduler owns a non-const context, so removing const here is safe.
    if (Ctx)
    {
        const_cast<FSystemContext*>(Ctx)->SetEntityLocation(AsEntity(Entity), Location);
    }
}

LUMINA_DOTNET_EXPORT(void, SystemContext_DrawDebugLine)(const FSystemContext* Ctx, FVector3 Start, FVector3 End, FVector4 Color)
{
    if (Ctx)
    {
        Ctx->DrawDebugLine(Start, End, Color);
    }
}

// The process-global registry is the single source of truth, and id 0 means none.

// Queries are hierarchical, so an entity tagged with a leaf matches a query on its parent.

// IsEnabled lets the managed side skip per-script scope calls when nobody is recording.

// A binding resolves its name once per settings generation, so it costs no crossing per frame.

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(World_GetSystemContext),
    LUMINA_DOTNET_SIG(SystemContext_GetDeltaTime),
    LUMINA_DOTNET_SIG(SystemContext_GetTime),
    LUMINA_DOTNET_SIG(SystemContext_Create),
    LUMINA_DOTNET_SIG(SystemContext_Destroy),
    LUMINA_DOTNET_SIG(SystemContext_SetEntityLocation),
    LUMINA_DOTNET_SIG(SystemContext_DrawDebugLine)
);
