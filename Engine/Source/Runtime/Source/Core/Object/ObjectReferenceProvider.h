#pragma once

#include "Containers/FunctionRef.h"
#include "Core/Reflection/Type/ObjectReferenceVisitor.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Exposes CObject references the reflected graph cannot reach, such as an ECS registry or an editor selection.
    class IObjectReferenceProvider
    {
    public:

        virtual ~IObjectReferenceProvider() = default;

        // Named only so a failure to repoint can say which holder it came from.
        virtual const char* GetReferenceProviderName() const = 0;

        virtual void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) = 0;
    };

    // The registered holders, shared by everything that rewrites the object graph.
    struct RUNTIME_API FObjectReferenceProviders
    {
        static void Register(IObjectReferenceProvider* Provider);
        static void Unregister(IObjectReferenceProvider* Provider);

        // Returns how many providers were visited.
        static int32 ForEach(TFunctionRef<void(IObjectReferenceProvider&)> Func);
    };
}
