#pragma once

#include "Cast.h"
#include "Class.h"
#include "Containers/HashTable.h"
#include "Core/Reflection/Type/Function.h"
#include "Containers/Vector.h"
#include "ObjectHandleTyped.h"

RUNTIME_API Lumina::CClass* Construct_CClass_Lumina_CClass();
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class FProperty;
}

namespace Lumina
{
    /**
     * What changed about a script type's shape since the layout it currently carries.
     *
     * Kept apart so a reload does the least work that is actually correct: a retyped field has to rebuild the
     * property block, but a reworded tooltip must not -- rebuilding refuses while instances are live, so
     * treating every edit as structural is what used to make a metadata-only edit silently not take.
     */
    enum class EScriptTypeDirty : uint8
    {
        None     = 0,
        /** The property set, order, or types differ, so the appended block has to be torn down and rebuilt. */
        Layout   = BIT(0),
        /** Editor-facing data only (tooltip, category, clamps). Reapplied in place. */
        Metadata = BIT(1),
        /** Declared initializers differ, so the CDO needs the new values replayed onto it. */
        Defaults = BIT(2),
    };

    ENUM_CLASS_FLAGS(EScriptTypeDirty);


    /**
     * A class whose layout was decided at runtime rather than by the C++ compiler.
     *
     * Everything a minted class needs beyond a native one lives here, so a native CClass -- which is nearly
     * all of them -- carries none of it, and "is this type script-defined" is a cast rather than a flag.
     */
    class LUMINA_VISIBLE_TYPE CScriptClass : public CClass
    {
    public:

        DECLARE_CLASS(Lumina, CScriptClass, CClass, "/Script/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CScriptClass)

        CScriptClass() = default;

        CScriptClass(CPackage* Package, const FName& InName, uint32 InSize, uint32 InAlignment, EObjectFlags InFlags, FactoryFunctionType InFactory)
            : CClass(Package, InName, InSize, InAlignment, InFlags, InFactory)
        {}

        RUNTIME_API CClass* GetMetaClass() const override;

        /** The events this C# subclass overrides, as real functions on this class whose body is managed.
         *
         *  A map rather than a mask: an override is a function like any other, found by the name it is
         *  declared under, which is what removes both the event ceiling and the agreed-index contract that
         *  the shim and the C# attribute used to have to keep in step. */
        THashMap<FName, FFunction*> ScriptOverrideFunctions;

        /** The overriding function for Name, or null when the subclass leaves the native body in place. */
        RUNTIME_API NODISCARD const FFunction* FindScriptOverride(const FName& Name) const;

        /** One bit per generated event slot, mirroring ScriptOverrideFunctions so a shim skips the name lookup. */
        TVector<uint64> ScriptOverrideSlots;

        /** Whether the script overrides the event the generator assigned Slot. */
        NODISCARD bool HasScriptOverrideSlot(int32 Slot) const
        {
            const size_t Word = (size_t)((uint32)Slot >> 6);
            return Word < ScriptOverrideSlots.size() && (ScriptOverrideSlots[Word] & (1ull << (Slot & 63))) != 0;
        }

        /** EScriptUpdatePhase for a minted entity-script class, from its C# [UpdatePhase]. */
        uint8 ScriptUpdatePhase = 0;

        // From the C# [ParallelUpdate], the managed spelling of REFLECT(ParallelUpdate).
        bool bScriptParallelUpdate = false;

        // ENetRealm from the C# [HostOnly], [ClientOnly] and [Cosmetic], the managed spelling of the REFLECT specifiers.
        uint8 ScriptNetRealm = 0;

        /** Every property appended from the script type's schema, in layout order. They live past the C++
         *  shim the class was minted from. */
        TVector<FProperty*> ScriptProperties;

        /** The subset of ScriptProperties whose values own storage, so they need construction/destruction over
         *  the object's trailing block. Holds the properties themselves rather than any description of them:
         *  each one knows how to build its own value (FProperty::ConstructValue), so adding a new property
         *  kind teaches that kind and changes nothing here. */
        TVector<FProperty*> ScriptLifecycleProperties;

        /** Placement-constructs every script-appended property. Called from StaticAllocateObject once the
         *  object's class is known, before PostInitProperties -- so a script's first callback sees valid
         *  values rather than a memzeroed FString, which is a plausible-looking empty string that corrupts
         *  on the first assignment. Returns whether anything was constructed, which is what stamps
         *  OF_ScriptProperties and therefore what decides if the destructor comes back here at all. */
        RUNTIME_API bool ConstructScriptProperties(void* Object) const;

        /** Mirror of the above, from ~CObjectBase. Must be kept in lockstep: a construct without its destruct
         *  leaks, the reverse double-frees. */
        RUNTIME_API void DestructScriptProperties(void* Object) const;

        /** Anchors the type that emitted ScriptProperties: its arena owns their storage, so the two die
         *  together. Held as a CStruct because the concrete emitting type is the scripting layer's. */
        TStrongObjectPtr<CStruct> LayoutRecord;

        /** Size and alignment before the block was appended, to restore when it is rebuilt. */
        uint32 ShimSize = 0;
        uint32 ShimAlign = 1;

        /** Set once a block has been appended, which is what tells a first append from a rebuild. Not the
         *  same as a non-empty ScriptProperties: a type may legitimately append nothing. */
        bool bHasAppendedBlock = false;

    };

    /**
     * Null unless Class was minted from a script type.
     *
     * Checks that the class has finished registering before asking what it is: bootstrap allocates objects
     * from classes whose own ClassPrivate is not set yet, and IsA on one of those trips its assert. Nothing
     * is minted that early, so an unregistered class is never a script class.
     */
    FORCEINLINE const CScriptClass* ToScriptClass(const CClass* Class)
    {
        return (Class != nullptr && Class->GetClass() != nullptr) ? Cast<CScriptClass>(Class) : nullptr;
    }

    FORCEINLINE CScriptClass* ToScriptClass(CClass* Class)
    {
        return (Class != nullptr && Class->GetClass() != nullptr) ? Cast<CScriptClass>(Class) : nullptr;
    }

    /**
     * The script body overriding Name on this class, or null to run the C++ one.
     *
     * Called by the generated forwarding shim on every reflected virtual. A native class is not a
     * CScriptClass at all and answers null from the cast, which is what keeps an unscripted call off the
     * managed path without touching the map.
     */
    FORCEINLINE const FFunction* FindScriptOverride(const CClass* Class, const FName& Name)
    {
        const CScriptClass* ScriptClass = ToScriptClass(Class);
        return ScriptClass != nullptr ? ScriptClass->FindScriptOverride(Name) : nullptr;
    }
}
