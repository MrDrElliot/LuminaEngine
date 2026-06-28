#pragma once

#include "Containers/Array.h"
#include "Containers/ContainerOps.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/LuminaMacros.h"
#include "Core/Object/Class.h"
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

    enum class EScriptElementKind : uint8
    {
        Trivial,
        String,
        AssetRef,
        NativeStruct,
        ScriptStruct,
        Instance,     ///< FInstancedStruct field (polymorphic, picks one of a candidate set).
    };

    struct FScriptArrayElementDesc
    {
        uint32                Size = 0;
        EScriptElementKind    Kind = EScriptElementKind::Trivial;
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

        const void* GetDefaults() const { return Defaults; }

        RUNTIME_API void ConstructInto(void* Buffer) const;
        RUNTIME_API void DestructIn(void* Buffer) const;
        RUNTIME_API void CopyInto(void* Dst, const void* Src) const;

        // CStruct lifetime overrides so an FInstancedStruct can own a script-defined instance (a minted
        // CScriptStruct has no FStructOps).
        void InitializeStruct(void* Dest) const override { ConstructInto(Dest); }
        void DestroyStruct(void* Dest) const override { DestructIn(Dest); }
        void CopyStruct(void* Dest, const void* Src) const override { CopyInto(Dest, Src); }

        // The layout's defaults buffer, so editor diff and reset-to-default work for script fields.
        void* GetDefaultInstance() override { return const_cast<void*>(GetDefaults()); }

        // Resets every [SkipHotReload] field in Buffer to the layout default; called on a hot-reload rebind.
        RUNTIME_API void ResetHotReloadFields(void* Buffer) const;

        void OnDestroy() override;

    private:

        struct FFieldInfo
        {
            uint32                                  Offset = 0;
            Scripting::EScriptElementKind           Kind = Scripting::EScriptElementKind::Trivial;
            CStruct*                                NativeStruct = nullptr;
            const CScriptStruct*                    ScriptStruct = nullptr;
            bool                                    bArray = false;
            const Scripting::FScriptArrayElementDesc* ArrayElement = nullptr;
        };

        struct FFieldPlan;

        bool ResolvePlan(const Scripting::FScriptExportField& Field, FFieldPlan& Out);
        FProperty* CreateProperty(const FFieldPlan& Plan, uint32 Offset);
        bool ResolveElement(const Scripting::FScriptExportType& Type, Scripting::FScriptArrayElementDesc& Out);
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
        TVector<FFieldInfo>                             FieldInfos;
        TVector<TObjectPtr<CScriptStruct>>             SubStructs;
        TVector<TObjectPtr<CEnum>>                     MintedEnums;
        TVector<Scripting::FScriptArrayElementDesc*>   ElementDescs;
        bool                                           bRuntimeFreed = false;
    };
}

namespace Lumina::Scripting
{
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
