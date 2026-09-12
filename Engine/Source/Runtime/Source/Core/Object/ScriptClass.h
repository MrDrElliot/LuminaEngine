#pragma once

#include "Cast.h"
#include "Class.h"
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

        /** Which ScriptEvents the C# subclass actually overrides (bit i == the wrapper's [ScriptEvent(i)]),
         *  so a non-overridden event costs one class-level test in the generated shim rather than a lookup. */
        uint64 ScriptOverrides = 0;

        /** EScriptUpdatePhase for a minted entity-script class, from its C# [UpdatePhase]. */
        uint8 ScriptUpdatePhase = 0;

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
        TObjectPtr<CStruct> LayoutRecord;

        /** Size and alignment before the block was appended, to restore when it is rebuilt. */
        uint32 ShimSize = 0;
        uint32 ShimAlign = 1;

        /** Set once a block has been appended, which is what tells a first append from a rebuild. Not the
         *  same as a non-empty ScriptProperties: a type may legitimately append nothing. */
        bool bHasAppendedBlock = false;

        //~ One superseded generation kept alive, so a property pointer that outlived a rebuild reads stale
        //~ rather than freed. Dropped once a whole reload has passed.
        TObjectPtr<CStruct> RetiredRecord;
        TVector<FProperty*> RetiredProperties;
        uint64              RetiredIn = 0;

        /** Drops the superseded generation. Safe once a reload has passed with nothing reaching it. */
        RUNTIME_API void DiscardRetiredLayout();

        /** Moves the live block to the retired slot and clears it, for a rebuild. */
        RUNTIME_API void RetireLayout(uint64 Generation);
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
     * Whether the class minted from a script type overrides ScriptEvent number Index.
     *
     * Called by the generated forwarding shim on every reflected virtual, so it stays a test on the CLASS:
     * a native class is not a CScriptClass at all and answers false from the cast, which is what keeps a
     * non-overridden event off the managed path entirely.
     */
    FORCEINLINE bool HasScriptOverride(const CClass* Class, int32 Index)
    {
        const CScriptClass* ScriptClass = ToScriptClass(Class);
        return ScriptClass != nullptr && (ScriptClass->ScriptOverrides & (1ull << Index)) != 0;
    }
}
