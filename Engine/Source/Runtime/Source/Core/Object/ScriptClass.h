#pragma once

#include "Cast.h"
#include "Class.h"
#include "Containers/HashTable.h"
#include "Core/Reflection/Type/Function.h"
#include "Containers/Vector.h"
#include "ObjectHandleTyped.h"

#include "Memory/SmartPtr.h"

namespace Lumina
{
    class FProperty;
}

namespace Lumina
{
    // Kept apart because a rebuild refuses while instances are live, so a metadata-only edit must not count as layout.
    enum class EScriptTypeDirty : uint8
    {
        None     = 0,
        // The property set, order or types differ, so the appended block needs a new class.
        Layout   = BIT(0),
        // Editor-facing data only, such as a tooltip or a clamp, reapplied in place.
        Metadata = BIT(1),
    };

    ENUM_CLASS_FLAGS(EScriptTypeDirty);


    // A class whose layout was decided at runtime, so asking whether a type is script-defined is a cast.
    class LUMINA_VISIBLE_TYPE CScriptClass : public CClass
    {
    public:

        DECLARE_CLASS(Lumina, CScriptClass, CClass, "/Types/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CScriptClass)

        CScriptClass() = default;

        CScriptClass(CPackage* Package, const FName& InName, uint32 InSize, uint32 InAlignment, EObjectFlags InFlags, FactoryFunctionType InFactory)
            : CClass(Package, InName, InSize, InAlignment, InFlags, InFactory)
        {}

        RUNTIME_API CClass* GetMetaClass() const override;

        bool IsAbstract() const override { return bAbstract; }
        bool IsSuperseded() const override { return bSuperseded; }

        // Every appended property of this class and of its C# parents, which together are an instance's script storage.
        template<typename TFunc>
        void ForEachScriptProperty(TFunc&& Func) const
        {
            for (const CScriptClass* Class = this; Class != nullptr; Class = Cast<CScriptClass>(Class->GetSuperClass()))
            {
                for (FProperty* Property : Class->ScriptProperties)
                {
                    Func(Property);
                }
            }
        }

        // Minted from an abstract C# class, so it exists for its subclasses' layout and is never instantiated.
        bool bAbstract = false;

        // Set while a reload has replaced or removed it, and cleared if the same C# type comes back.
        bool bSuperseded = false;

        // Overridden events as real functions with managed bodies, keyed by name so there is no event ceiling.
        THashMap<FName, FFunction*> ScriptOverrideFunctions;

        // Null when the subclass leaves the native body in place.
        NODISCARD RUNTIME_API const FFunction* FindScriptOverride(const FName& Name) const;

        // One bit per generated event slot, mirroring ScriptOverrideFunctions so a shim skips the name lookup.
        TVector<uint64> ScriptOverrideSlots;

        NODISCARD bool HasScriptOverrideSlot(int32 Slot) const
        {
            const int32 Word = (size_t)((uint32)Slot >> 6);
            return Word < ScriptOverrideSlots.Num() && (ScriptOverrideSlots[Word] & (1ull << (Slot & 63))) != 0;
        }

        // EScriptUpdatePhase from the UpdatePhase metadata up the chain, resolved at mint since the script tick reads it per script.
        uint8 ScriptUpdatePhase = 0;

        // The class metadata the C# attributes set last reload, so a removed or changed attribute does not linger on a reused class.
        TVector<FName> ScriptClassMetaKeys;

        // Every property appended from the script type's schema, in layout order.
        TVector<FProperty*> ScriptProperties;

        // The subset of ScriptProperties whose values own storage and need constructing over the trailing block.
        TVector<FProperty*> ScriptLifecycleProperties;

        // Runs before PostInitProperties, and its result stamps OF_ScriptProperties so the destructor comes back here.
        RUNTIME_API bool ConstructScriptProperties(void* Object) const;

        // Must stay in lockstep with ConstructScriptProperties, or values leak or double-free.
        RUNTIME_API void DestructScriptProperties(void* Object) const;

        // Anchors the type whose arena owns ScriptProperties, held as a CStruct since that type is the scripting layer's.
        TStrongObjectPtr<CStruct> LayoutRecord;

        // Size and alignment before the block was appended, to restore when it is rebuilt.
        uint32 ShimSize = 0;
        uint32 ShimAlign = 1;

        // Tells a first append from a rebuild, since a type may legitimately append nothing.
        bool bHasAppendedBlock = false;

    };

    // Skips classes still registering, whose IsA would assert, since nothing is minted that early.
    FORCEINLINE const CScriptClass* ToScriptClass(const CClass* Class)
    {
        return (Class != nullptr && Class->GetClass() != nullptr) ? Cast<CScriptClass>(Class) : nullptr;
    }

    FORCEINLINE CScriptClass* ToScriptClass(CClass* Class)
    {
        return (Class != nullptr && Class->GetClass() != nullptr) ? Cast<CScriptClass>(Class) : nullptr;
    }

    // Called by the generated shim on every reflected virtual, and null to run the C++ body.
    FORCEINLINE const FFunction* FindScriptOverride(const CClass* Class, const FName& Name)
    {
        const CScriptClass* ScriptClass = ToScriptClass(Class);
        return ScriptClass != nullptr ? ScriptClass->FindScriptOverride(Name) : nullptr;
    }
}
