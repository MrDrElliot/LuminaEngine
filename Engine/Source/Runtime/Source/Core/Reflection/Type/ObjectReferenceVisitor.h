#pragma once

#include "Containers/FunctionRef.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObject;
    class CStruct;
    class FProperty;
    struct FSoftObjectPath;

    /**
     * Every CObject-pointer slot reachable from a value, reached through the reflected shape rather than
     * through any one holder's knowledge of its own fields.
     *
     * The callback returns the object to store, so one walk serves both directions: return the argument to
     * collect, return something else to rewrite. That is what lets a caller repoint the whole object graph
     * without the types in it participating.
     */
    class FObjectReferenceVisitor
    {
    public:

        using FSlotFunc = TFunctionRef<CObject*(CObject*)>;

        /** A soft reference names its target instead of pointing at it, so it is rewritten in place. */
        using FSoftSlotFunc = TFunctionRef<void(FSoftObjectPath&)>;

        /** Walks every property of Struct over Instance. */
        RUNTIME_API static void VisitStruct(const CStruct* Struct, void* Instance, FSlotFunc Func);

        /** Walks one property's value, recursing through containers, structs and optionals. */
        RUNTIME_API static void VisitProperty(const FProperty* Property, void* Value, FSlotFunc Func);

        //~ The same walk also reporting soft references, so callers that ignore them pay nothing.

        RUNTIME_API static void VisitStructWithSoft(const CStruct* Struct, void* Instance,
            FSlotFunc Func, FSoftSlotFunc SoftFunc);

        RUNTIME_API static void VisitPropertyWithSoft(const FProperty* Property, void* Value,
            FSlotFunc Func, FSoftSlotFunc SoftFunc);
    };
}
