#pragma once
#include "ExclusiveSystem.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptNet.h"
#include "ScriptSyncSystem.generated.h"

namespace Lumina
{
    // Compares C++ scripts' Sync fields once a frame, before the network system sends, so no script polls itself.
    REFLECT(Networked)
    class RUNTIME_API CScriptSyncSystem : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnExclusiveUpdate(const SExclusiveContext& Context) override;

    private:

        ECS::TQuery<ECS::Read<SEntityScriptComponent>, ECS::Write<FScriptSyncState>> Synced;
    };
}
