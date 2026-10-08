#pragma once

#include "AnimEvents.h"
#include "Containers/Vector.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "World/ECS/Entity.h"

namespace Lumina
{
    namespace ECS
    {
        class FCommandBus;
    }

    // Simple playback runs before graphs, matching the order the passes resolve a dual-component entity.
    enum class EAnimNotifyPass : uint8
    {
        Simple,
        Graph,
    };

    // Typed notifies recorded by the animation passes on any worker, run later on the main thread at a command bus flush.
    class RUNTIME_API FAnimNotifyQueue
    {
    public:

        // Main thread, before any pass records.
        void Prepare();

        // Any thread. Name-only events are skipped, since they stay on the component for polling.
        void Record(ECS::FEntity Entity, EAnimNotifyPass Pass, const TVector<FAnimNotifyEvent>& Events);

        // Main thread, once every pass has joined. The handlers run at the bus's next flush with full registry access.
        void Submit(ECS::FCommandBus& Bus);

    private:

        struct FRecord
        {
            // Keeps the authored instance alive even if an earlier handler releases the clip.
            TStrongObjectPtr<const CObject> Source;
            const SAnimNotify*              Notify = nullptr;
            const SAnimNotifyState*         State  = nullptr;
            ECS::FEntity                    Entity;
            float                           Alpha  = 0.0f;
            EAnimNotifyEventType            Type   = EAnimNotifyEventType::Trigger;
            EAnimNotifyPass                 Pass   = EAnimNotifyPass::Simple;
        };

        static void Dispatch(const TVector<FRecord>& Records, ECS::FRegistry& Registry);

        TVector<TVector<FRecord>> Slots;
    };
}
