#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class CAnimationGraph;
    class CWorld;
    struct FSkeletonResource;

    struct FSkeletonEditContext
    {
        static FName ContextKey() { return "Skeleton"; }

        const FSkeletonResource* Skeleton = nullptr;
    };

    struct FAnimGraphEditContext
    {
        static FName ContextKey() { return "AnimGraph"; }

        CAnimationGraph* Graph = nullptr;
    };

    // Skeleton is set when the attach target is skeletal, which makes raw bones attachable too.
    struct FSocketEditContext
    {
        static FName ContextKey() { return "Sockets"; }

        TVector<FName>           Sockets;
        const FSkeletonResource* Skeleton = nullptr;
    };

    // The state machine canvas being edited, so a picker can list the states on it.
    struct FAnimStateMachineEditContext
    {
        static FName ContextKey() { return "AnimStateMachine"; }

        class CAnimStateMachineGraph* Graph = nullptr;
    };

    struct FWorldEditContext
    {
        static FName ContextKey() { return "World"; }

        CWorld* World = nullptr;
    };

    // Brokers the eyedropper handshake between a property widget and the viewport that services it.
    class FEntityPickBroker
    {
    public:

        void Request(uint64 Token)
        {
            bRequested = true;
            PickToken = Token;
            bHasResult = false;
        }

        void Cancel()
        {
            bRequested = false;
            bHasResult = false;
        }

        bool IsRequested() const { return bRequested; }

        bool IsActiveFor(uint64 Token) const { return bRequested && PickToken == Token; }

        void Fulfill(uint32 Entity)
        {
            if (bRequested)
            {
                Result = Entity;
                bHasResult = true;
                bRequested = false;
            }
        }

        bool ConsumeResult(uint64 Token, uint32& OutEntity)
        {
            if (bHasResult && PickToken == Token)
            {
                OutEntity = Result;
                bHasResult = false;
                return true;
            }
            return false;
        }

    private:

        bool   bRequested = false;
        uint64 PickToken = 0;
        bool   bHasResult = false;
        uint32 Result = 0;
    };

    // Shared, so a picker that outlives the tool can still withdraw its own request.
    struct FEntityPickContext
    {
        static FName ContextKey() { return "EntityPick"; }

        TSharedPtr<FEntityPickBroker> Broker;
    };
}
