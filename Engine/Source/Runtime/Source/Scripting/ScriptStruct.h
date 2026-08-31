#pragma once

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/ContainerOps.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/LuminaMacros.h"
#include "Core/Object/Class.h"
#include "Core/Object/Field.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "Scripting/ScriptExports.h"

namespace Lumina
{
    class CEnum;
    class CScriptStruct;
    class FProperty;
}

namespace Lumina::Scripting
{
    class FScriptStructRegistry;

    // One element of a script-minted container: how big it is, and -- through Inner -- how to bring one up,
    // tear it down and copy it. There is deliberately no per-kind switch here: every one of those three
    // operations is the inner FProperty's own (ConstructValue / DestructValue / CopyCompleteValue), so
    // supporting a new element type means teaching that property type and changing nothing in this file.
    struct FScriptArrayElementDesc
    {
        uint32                Size = 0;
        CStruct*              NativeStruct = nullptr;
        const CScriptStruct*  ScriptStruct = nullptr;
        const FProperty*      Inner = nullptr;
        FVectorOps            Ops{};

        void ConstructElement(void* Element) const;
        void DestructElement(void* Element) const;
        void CopyElement(void* Dst, const void* Src) const;
    };

    struct FScriptDynamicArray
    {
        TVector<uint8>                   Bytes;
        const FScriptArrayElementDesc*   Element = nullptr;

        FScriptDynamicArray() = default;
        ~FScriptDynamicArray();
        LE_NO_COPYMOVE(FScriptDynamicArray);
    };

    // Key/value element description for a script-minted map. The Key/Value reuse the array element desc (size,
    // lifecycle kind, inner FProperty, construct/destruct/copy). Pairs are packed [key][pad][value][pad] with a
    // 16-byte alignment so any key/value type stays aligned inside the (16-byte-aligned) backing buffer.
    struct FScriptMapElementDesc
    {
        FScriptArrayElementDesc  Key;
        FScriptArrayElementDesc  Value;
        uint32                   ValueOffset = 0;   // byte offset of the value within a pair
        uint32                   PairStride  = 0;   // byte stride between consecutive pairs
        FMapOps                  Ops{};

        void* KeyAt(void* Pair)   const { return Pair; }
        void* ValueAt(void* Pair) const { return static_cast<uint8*>(Pair) + ValueOffset; }

        void ConstructPair(void* Pair) const;
        void DestructPair(void* Pair) const;
        void CopyPair(void* Dst, const void* Src) const;
    };

    // A script map's runtime backing: packed key/value pairs in a byte buffer, with linear find-by-key via the
    // key property's Identical (no hashing needed; script/config maps are small and this is an editor/serialize
    // path, not a hot loop). The C# instance keeps a real Dictionary<K,V>; this mirrors it for the FMapProperty.
    struct FScriptDynamicMap
    {
        TVector<uint8>                   Bytes;
        const FScriptMapElementDesc*     Desc = nullptr;

        FScriptDynamicMap() = default;
        ~FScriptDynamicMap();
        LE_NO_COPYMOVE(FScriptDynamicMap);
    };
}

namespace Lumina
{
    // Runtime-minted CStruct mirroring one C# script type's [Property] layout with real FProperties.
    class CScriptStruct : public CStruct
    {
        DECLARE_CLASS(Lumina, CScriptStruct, CStruct, "/Script/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CScriptStruct)

    public:

        CScriptStruct() = default;

        RUNTIME_API bool BuildFromSchema(const Scripting::FScriptExportSchema& Schema,
            const TVector<Scripting::FScriptPropertyEntry>* DefaultValues = nullptr);

        /** Result of laying a schema out onto some other struct or class. */
        struct FEmittedLayout
        {
            TVector<FProperty*> Properties;     ///< every property emitted, in layout order
            uint32              EndOffset = 0;  ///< one past the last byte the block occupies
            uint32              Alignment = 1;  ///< the block's required alignment
        };

        // The record owning the emitted properties' element descriptions, so it must outlive every instance of Target.
        RUNTIME_API FEmittedLayout EmitLayoutInto(CStruct* Target, uint32 BaseOffset,
            const Scripting::FScriptExportSchema& Schema);

        const void* GetDefaults() const { return Defaults; }

        RUNTIME_API void ConstructInto(void* Buffer) const;
        RUNTIME_API void DestructIn(void* Buffer) const;
        RUNTIME_API void CopyInto(void* Dst, const void* Src) const;

        // True once any field owns storage, or once there are defaults worth seeding: either way a zeroed
        // buffer is not a valid instance of this layout.
        bool RequiresValueLifecycle() const override { return bRequiresLifecycle; }


        void InitializeStruct(void* Dest) const override
        {
            ConstructInto(Dest);
            if (Defaults != nullptr)
            {
                CopyInto(Dest, Defaults);
            }
        }
        void DestroyStruct(void* Dest) const override { DestructIn(Dest); }
        void CopyStruct(void* Dest, const void* Src) const override { CopyInto(Dest, Src); }

        // The layout's defaults buffer, so editor diff and reset-to-default work for script fields.
        void* GetDefaultInstance() override { return const_cast<void*>(GetDefaults()); }

        // Resets every [SkipHotReload] field in Buffer to the layout default; called on a hot-reload rebind.
        RUNTIME_API void ResetHotReloadFields(void* Buffer) const;

        void OnDestroy() override;

    private:

        struct FFieldPlan;
        struct FKindLayout;

        /**
         * The two questions asked of every export kind -- how big is it, and what FProperty represents it --
         * each answered in ONE place.
         *
         * A kind is reachable two ways: as a field of the layout, and as the element of a container field.
         * Those used to be four hand-kept if-chains (plan/create x field/element), which is two chances per
         * kind for the size path and the property path to disagree about what a kind means. They are now
         * these two functions, with the field and element paths differing only in what they do with the
         * answer: a field also gets an alignment and the author's [Property] metadata, an element is packed
         * at its size and carries only the kind's own tag.
         *
         * Container kinds are deliberately absent. A Vector or a Map is only ever a field -- native has no
         * nested-container property -- so ResolvePlan handles those before asking, and ResolveKindLayout
         * refusing them is exactly what enforces the rule for elements.
         */
        bool ResolveKindLayout(const Scripting::FScriptExportType& Type, const FName& DiagName, FKindLayout& Out);
        FProperty* MakeForKind(const FFieldOwner& Owner, const FName& FieldName, uint32 Offset,
            const Scripting::FScriptExportType& Type, CStruct* Resolved);

        bool ResolvePlan(const Scripting::FScriptExportField& Field, FFieldPlan& Out);
        FProperty* CreateProperty(CStruct* Target, const FFieldPlan& Plan, uint32 Offset);
        bool ResolveElement(const Scripting::FScriptExportType& Type, const FName& DiagName, Scripting::FScriptArrayElementDesc& Out);
        FProperty* CreateElement(void* ArrayOwner, const Scripting::FScriptExportType& Type, Scripting::FScriptArrayElementDesc& Desc);
        CScriptStruct* MintSubStruct(const Scripting::FScriptExportType& Type);

        // An empty marker the candidate sub-structs derive from, so the picker enumerates this field's
        // candidates via IsChildOf. Tagged ScriptInstanceBase so the picker hides it.
        CScriptStruct* MintInstanceBase(const FName& BaseName);

        // One selectable concrete type for an Instance field. A sub-CScriptStruct deriving from Base,
        // tagged with its stable C# type name in ScriptTypeName metadata.
        CScriptStruct* MintInstanceCandidate(const Scripting::FScriptExportInstanceCandidate& Candidate, CScriptStruct* Base);

        CEnum* MintEnum(const Scripting::FScriptExportType& Type);
        void FreeRuntimeData();

        uint8*                                          Defaults = nullptr;
        bool                                           bRequiresLifecycle = false;
        TVector<TObjectPtr<CScriptStruct>>             SubStructs;
        TVector<TObjectPtr<CEnum>>                     MintedEnums;

        // Keyed by type shape, so many fields of one type share a single mint within this layout.
        THashMap<FString, CScriptStruct*>              SubStructsByKey;
        THashMap<FString, CEnum*>                      EnumsByKey;

        TVector<Scripting::FScriptArrayElementDesc*>   ElementDescs;
        TVector<Scripting::FScriptMapElementDesc*>     MapDescs;
        bool                                           bRuntimeFreed = false;
    };

}

namespace Lumina::Scripting
{
    /**
     * Appends a C# script type's [Property] members to an already-minted CClass as REAL FPropertys, laid out
     * in the trailing block past the C++ shim the class was minted from. That is what lets a script's
     * properties be drawn by the stock FPropertyTable, serialized by SerializeTaggedProperties, and covered by
     * undo / prefab overrides / replication -- instead of a parallel minted CStruct fed by a value blob.
     *
     * Every reflected property kind is supported, because the layout is planned by the same code that plans a
     * CScriptStruct and each property brings its own value lifecycle (FProperty::ConstructValue /
     * DestructValue / OwnsStorage). The ones that own storage are recorded on the class so
     * StaticAllocateObject and ~CObjectBase drive them.
     *
     * Grows Target->Size/Alignment, so it MUST run before the CDO exists (CreateDefaultObject allocates from
     * GetSize() and latches Link()). Declared defaults are NOT applied here: they are replayed against the CDO
     * by DotNet::ApplyScriptableDefaults once it exists, and every later instance is copied from it.
     *
     * Returns the number of properties appended.
     */
    RUNTIME_API uint32 AppendScriptPropertiesToClass(CClass* Target, const FScriptExportSchema& Schema);

    /** Drops a retired minted class's layout record. Only safe once the class has no live instances. */
    RUNTIME_API void ForgetScriptClassLayout(CClass* Target);

    /**
     * Resets every `[SkipHotReload]` script property on Object to its class default.
     *
     * Called after a hot reload has restored an instance's authored values, for the fields whose author
     * asked NOT to carry them: a value you tune at edit time but want back at its default whenever you
     * iterate. The class default object is the source, which is where the C# declared initializers live.
     *
     * A no-op for a class with no appended properties, or before its CDO exists.
     */
    RUNTIME_API void ResetSkipHotReloadProperties(CObject* Object);

    /** A string identifying what a schema lays out. Equal strings mean an identical layout; metadata-only
     *  edits (a tooltip, a Min/Max) deliberately do not change it. */
    RUNTIME_API FString DescribeScriptSchemaLayout(const FScriptExportSchema& Schema);

    // Identity of one exported type's shape, so a type used by many fields is minted once.
    RUNTIME_API FString DescribeScriptTypeSignature(const FScriptExportType& Type);

    // Opens a reload's generation, freeing the layouts an earlier one superseded. Call once per reload.
    RUNTIME_API void AdvanceScriptTypeGeneration();

    /** True when Target's appended block already matches Schema, so a hot reload needs no rebuild. */
    RUNTIME_API bool ScriptClassLayoutMatches(CClass* Target, const FScriptExportSchema& Schema);

    /**
     * Rebuilds Target's appended property block from Schema, for a hot reload that added, removed, retyped
     * or renamed a C# `[Property]`.
     *
     * A minted class is reused by name across reloads and keeps its identity, but its SIZE is baked into
     * every object at allocation (StaticAllocateObject reads Class->GetSize() once), so a changed property
     * set cannot be patched in place. This tears the block down and builds the new one: retire the layout
     * record, discard the CDO, unlink, restore the shim's size, re-append, and create a fresh CDO.
     *
     * Returns false and changes nothing if the class has live instances. They are laid out at the old size,
     * so the caller must evacuate them first (serializing their owning components, which is already how
     * SEntityScriptComponent round-trips) and repopulate after. Also returns false if Target never had an
     * appended block, where the caller wants AppendScriptPropertiesToClass instead.
     *
     * Declared defaults are NOT applied; as with a first mint, the caller replays them onto the new CDO.
     */
    RUNTIME_API bool MigrateMintedClassLayout(CClass* Target, const FScriptExportSchema& Schema);

    // Per-ScriptClass cache of minted script structs, owned by the .NET host and cleared on reload.
    class FScriptStructRegistry
    {
    public:

        RUNTIME_API const CScriptStruct* GetOrBuild(FStringView ScriptClass);
        RUNTIME_API void Clear();

    private:

        THashMap<FName, TObjectPtr<CScriptStruct>> Entries;
    };
}
