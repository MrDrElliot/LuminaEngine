#pragma once

#include "Containers/Vector.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Simulation/SimulationSettings.h"
#include "World/Entity/Systems/SystemContext.h"
#include "SimulationLibrary.generated.h"

namespace Lumina
{
    class CWorld;

    // The FSimulation a C# system reaches through its SSystemContext, and the game reaches through its world. Every call is inert without a running session.
    REFLECT()
    class RUNTIME_API CSimulationLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        //~ For systems in the Simulation stage.

        FUNCTION(ScriptInternal)
        static bool IsRunning(const SSystemContext& Context);

        FUNCTION(ScriptInternal)
        static uint32 GetTick(const SSystemContext& Context);

        FUNCTION(ScriptInternal)
        static float GetTickSeconds(const SSystemContext& Context);

        FUNCTION(ScriptInternal)
        static bool IsResimulating(const SSystemContext& Context);

        FUNCTION(ScriptInternal)
        static uint32 GetNumPlayers(const SSystemContext& Context);

        FUNCTION(ScriptInternal)
        static uint32 GetLocalPlayer(const SSystemContext& Context);

        // Null when there is no session or its input is not InputStruct, so a caller never reads the wrong type.
        FUNCTION(ScriptInternal)
        static void* GetInput(const SSystemContext& Context, uint32 Player, FName InputStruct);

        FUNCTION(ScriptInternal)
        static ECS::FEntity Spawn(const SSystemContext& Context);

        FUNCTION(ScriptInternal)
        static void Destroy(const SSystemContext& Context, ECS::FEntity Entity);

        FUNCTION(ScriptInternal)
        static ECS::FEntity FindEntity(const SSystemContext& Context, uint32 SimId);

        FUNCTION(ScriptInternal)
        static void GetEntities(const SSystemContext& Context, TVector<ECS::FEntity>& Out);

        FUNCTION(ScriptInternal)
        static uint32 RandomUInt32(const SSystemContext& Context);

        FUNCTION(ScriptInternal)
        static int32 RandomRange(const SSystemContext& Context, int32 Min, int32 Max);

        FUNCTION(ScriptInternal)
        static float RandomFloat(const SSystemContext& Context);

        FUNCTION(ScriptInternal)
        static float RandomFloatRange(const SSystemContext& Context, float Min, float Max);

        FUNCTION(ScriptInternal)
        static bool RandomBool(const SSystemContext& Context);

        //~ For the game.

        // A standalone world plays alone, a server hands its ready clients a session, and a client joins the one its host starts.
        FUNCTION(ScriptInternal)
        static bool StartSession(CWorld* World, FName InputStruct, uint64 Seed);

        FUNCTION(ScriptInternal)
        static bool IsSessionRunning(CWorld* World);

        FUNCTION(ScriptInternal)
        static ESimulationMode GetMode(CWorld* World);

        FUNCTION(ScriptInternal)
        static uint32 GetSessionPlayers(CWorld* World);

        FUNCTION(ScriptInternal)
        static uint32 GetSessionLocalPlayer(CWorld* World);

        // The input the next stamped tick takes, written in place. Null when there is no session or its input is not InputStruct.
        FUNCTION(ScriptInternal)
        static void* GetLocalInput(CWorld* World, FName InputStruct);

        FUNCTION(ScriptInternal)
        static float GetInterpolationAlpha(CWorld* World);

        FUNCTION(ScriptInternal)
        static uint32 GetCurrentTick(CWorld* World);

        FUNCTION(ScriptInternal)
        static int64 GetConfirmedTick(CWorld* World);

        FUNCTION(ScriptInternal)
        static uint32 GetRollbackCount(CWorld* World);

        FUNCTION(ScriptInternal)
        static bool HasDesynced(CWorld* World);
    };
}
