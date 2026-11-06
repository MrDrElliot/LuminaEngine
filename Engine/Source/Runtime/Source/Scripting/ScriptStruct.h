#pragma once

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/ContainerOps.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/LuminaMacros.h"
#include "Core/Object/Class.h"
#include "Core/Object/Field.h"
#include "Core/Object/ScriptClass.h"
#include "Core/Object/ScriptEnum.h"
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

    // One element of a container a C# type declares, built, destroyed and copied through its inner property.
    struct FScriptArrayElementDesc
    {
        uint32                Size = 0;
        CField*               ElementType = nullptr;  // the reflected struct or enum the element names, if any
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

    // A script optional's payload, described by the array element desc that already carries all of its parts.
    struct FScriptOptionalDesc
    {
        FScriptArrayElementDesc Payload;
        FOptionalOps            Ops{};
    };

    // Heap payload rather than inline, so the slot is one fixed size and engaged is exactly a non-null payload.
    struct FScriptDynamicOptional
    {
        const FScriptOptionalDesc* Desc = nullptr;
        void*                      Payload = nullptr;

        FScriptDynamicOptional() = default;
        ~FScriptDynamicOptional();
        LE_NO_COPYMOVE(FScriptDynamicOptional);
    };

    // A map's key and value, packed per pair at 16-byte alignment so any key or value stays aligned in the buffer.
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

    // A map a C# type declares, packed pairs found by the key property's Identical, which suits the small maps scripts hold.
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
    // A struct minted from a C# type's schema, or the record of the block a C# class appends to its native base.
    class CScriptStruct : public CStruct
    {
        DECLARE_CLASS(Lumina, CScriptStruct, CStruct, "/Types/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CScriptStruct)

    public:

        CScriptStruct() = default;

        // Base, when given, is the struct this one extends; its fields come first and its own struct ops build them.
        RUNTIME_API bool BuildFromSchema(const Scripting::FScriptExportSchema& Schema, CStruct* Base = nullptr);

        // The block a schema was laid out as onto some other struct or class.
        struct FEmittedLayout
        {
            TVector<FProperty*> Properties;     ///< every property emitted, in layout order
            uint32              EndOffset = 0;  ///< one past the last byte the block occupies
            uint32              Alignment = 1;  ///< the block's required alignment
        };

        // Collector takes the properties instead of Target's member list, for a call frame whose slots are parameters.
        RUNTIME_API FEmittedLayout EmitLayoutInto(CStruct* Target, uint32 BaseOffset,
            const Scripting::FScriptExportSchema& Schema, TVector<FProperty*>* Collector = nullptr);

        const void* GetDefaults() const { return Defaults; }

        RUNTIME_API void ConstructInto(void* Buffer) const;
        RUNTIME_API void DestructIn(void* Buffer) const;
        RUNTIME_API void CopyInto(void* Dst, const void* Src) const;
        RUNTIME_API void RelocateInto(void* Dst, void* Src) const;

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
        void RelocateStruct(void* Dest, void* Source) const override { RelocateInto(Dest, Source); }

        // The layout's defaults buffer, so editor diff and reset-to-default work for script fields.
        void* GetDefaultInstance() override { return const_cast<void*>(GetDefaults()); }

        // Resets every [SkipHotReload] field in Buffer to the layout default; called on a hot-reload rebind.
        RUNTIME_API void ResetHotReloadFields(void* Buffer) const;

        // A reload reshaped the C# type, so a newer struct holds its name and values of this one are migrated by field name.
        NODISCARD bool IsSuperseded() const { return bSuperseded; }
        void MarkSuperseded() { bSuperseded = true; }

        // A class's layout record or a superseded type describes memory but is not a type anything stores a value of.
        NODISCARD bool IsValueType() const { return !bSuperseded && !bClassLayoutRecord; }
        void MarkClassLayoutRecord() { bClassLayoutRecord = true; }

        NODISCARD TSpan<FProperty* const> GetOwnProperties() const { return OwnProperties; }
        NODISCARD CStruct* GetNativeBase() const { return NativeBase; }

        void OnDestroy() override;

    private:

        struct FFieldPlan;

        bool ResolvePlan(const Scripting::FScriptExportField& Field, FFieldPlan& Out);
        FProperty* CreateProperty(CStruct* Target, const FFieldPlan& Plan, uint32 Offset, TVector<FProperty*>* Collector);
        bool ResolveElement(const Scripting::FScriptExportType& Type, const FName& DiagName, Scripting::FScriptArrayElementDesc& Out);
        FProperty* CreateElement(void* ArrayOwner, const Scripting::FScriptExportType& Type, Scripting::FScriptArrayElementDesc& Desc);
        void FreeRuntimeData();

    public:

        // The schema this record emitted, so the next reload can tell a layout change from a metadata one.
        const Scripting::FScriptExportSchema& GetAppliedSchema() const { return AppliedSchema; }
        void SetAppliedSchema(const Scripting::FScriptExportSchema& InSchema) { AppliedSchema = InSchema; }

    private:

        Scripting::FScriptExportSchema                 AppliedSchema;

        uint8*                                          Defaults = nullptr;
        bool                                           bRequiresLifecycle = false;
        bool                                           bSuperseded = false;
        bool                                           bClassLayoutRecord = false;

        // The fields this struct declares, without the base's, which its own struct ops look after.
        TVector<FProperty*>                            OwnProperties;
        CStruct*                                       NativeBase = nullptr;

        TVector<Scripting::FScriptArrayElementDesc*>   ElementDescs;
        TVector<Scripting::FScriptMapElementDesc*>     MapDescs;
        TVector<Scripting::FScriptOptionalDesc*>       OptionalDescs;
        bool                                           bRuntimeFreed = false;
    };

}

namespace Lumina::Scripting
{
    // Appends a script type's properties to its minted class as real FPropertys in the trailing block, so it must run before the class's CDO exists.
    RUNTIME_API uint32 AppendScriptPropertiesToClass(CScriptClass* Target, const FScriptExportSchema& Schema);

    /** Drops a minted class's layout records. Only safe once the class has no live instances. */
    RUNTIME_API void ForgetScriptClassLayout(CScriptClass* Target);

    // Resets every [SkipHotReload] property on Object to its class default after a reload carried the rest across.
    RUNTIME_API void ResetSkipHotReloadProperties(CObject* Object);

    // What differs between Target's block and Schema, a layout change needing a new class and a metadata one reapplied in place.
    RUNTIME_API EScriptTypeDirty DiffScriptClassLayout(const CScriptClass* Target, const FScriptExportSchema& Schema);

    // What changed between a block laid out from Applied, whose properties are Emitted, and Schema.
    RUNTIME_API EScriptTypeDirty DiffScriptLayout(const FScriptExportSchema& Applied, TSpan<FProperty* const> Emitted, const FScriptExportSchema& Schema);

    // Reapplies Schema's metadata to Target's properties by position, so only valid when the layout is unchanged.
    RUNTIME_API void RefreshScriptPropertyMetadata(CScriptClass* Target, const FScriptExportSchema& Schema);

    // True when Target's appended block already matches Schema, so a hot reload needs no rebuild.
    RUNTIME_API bool ScriptClassLayoutMatches(const CScriptClass* Target, const FScriptExportSchema& Schema);

}
