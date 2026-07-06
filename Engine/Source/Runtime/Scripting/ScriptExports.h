#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/ObjectCore.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class FArchive;
}

// Per-script-type schema + value model bridging the C# [Property] reflection to the native editor.
namespace Lumina::Scripting
{
    // The schema kind is the shared reflected taxonomy Lumina::EPropertyTypeFlags (ObjectCore.h), mirrored by
    // LuminaSharp.EPropertyType. Script-specific shapes are carried as data on FScriptExportType rather than as
    // distinct kinds: an entity is UInt32 + bEntity; an asset ref is SoftObject + TargetClass; a native vs.
    // script struct is Struct distinguished by whether NativeName is set.

    // Self-describing value kind. Mirrors LuminaSharp.EScriptValueKind.
    enum class EScriptValueKind : uint8
    {
        Nil = 0,
        Bool,
        Int,
        Double,
        String,
        Nested,
        Array,
        Instance,
        Map,   ///< count (i32) then that many [key value, value value] pairs. Append-only (persisted wire).
    };

    struct FScriptExportType;

    // One editor-display metadata pair set on a field.
    struct FScriptExportMetaArg
    {
        FName   Key;
        FString Value;
    };

    struct RUNTIME_API FScriptExportMeta
    {
        TVector<FScriptExportMetaArg> Entries;

        const FString* Find(const FName& Key) const;
        bool Has(const FName& Key) const { return Find(Key) != nullptr; }
        void Set(const FName& Key, const FString& Value);
        bool GetNumber(const FName& Key, double& OutValue) const;
    };

    // One enumerator of a C# enum.
    struct FScriptEnumEntry
    {
        FName Name;
        int64 Value = 0;
    };

    struct FScriptExportField
    {
        FName                         Name;
        TSharedPtr<FScriptExportType> Type;
        FScriptExportMeta             Meta;   ///< Editor display data, rebuilt per load.
    };

    // One selectable concrete type for an Instance field. Its stable C# type name plus the [Property]
    // members minted into that candidate's sub-CScriptStruct.
    struct FScriptExportInstanceCandidate
    {
        FName                       TypeName;
        TVector<FScriptExportField> Fields;
    };

    struct FScriptExportType
    {
        EPropertyTypeFlags            Kind = EPropertyTypeFlags::None;
        bool                          bEntity = false;  ///< A UInt32 that is really an entt entity handle.

        // Enum kind.
        FName                         EnumName;
        EPropertyTypeFlags            EnumUnderlying = EPropertyTypeFlags::Int32;
        TVector<FScriptEnumEntry>     EnumEntries;

        FName                         NativeName;     ///< Struct kind: the native CStruct's name (empty = script struct).
        FName                         TargetClass;    ///< SoftObject kind, the asset class filter ("" = any).
        TSharedPtr<FScriptExportType> ElementType;    ///< Vector kind.
        TSharedPtr<FScriptExportType> KeyType;        ///< Map kind, the key shape.
        TSharedPtr<FScriptExportType> ValueType;      ///< Map kind, the value shape.
        TVector<FScriptExportField>   Fields;         ///< Struct kind (native or script).

        FName                                       BaseName;    ///< InstancedStruct kind, the C# base type's display name.
        TVector<FScriptExportInstanceCandidate>     Candidates;  ///< InstancedStruct kind, the selectable concrete types.
    };

    struct FScriptExportSchema
    {
        TVector<FScriptExportField> Fields;

        bool IsValid() const { return !Fields.empty(); }
    };

    // A [Button] method exposed on a script type: a parameterless method drawn as an inspector button and
    // invoked by name on the live instance. Pure data; no VM coupling.
    struct FScriptButton
    {
        FString Method;    ///< Reflected method name (the invoke key).
        FString Label;     ///< Button text.
        FString Tooltip;   ///< Optional hover help.
    };

    struct FScriptPropertyEntry;

    // Transient per-field value bridging the native CScriptStruct buffer to and from the managed instance.
    struct FScriptPropertyValue
    {
        EScriptValueKind            Kind = EScriptValueKind::Nil;

        bool                        AsBool   = false;
        int64                       AsInt    = 0;
        double                      AsDouble = 0.0;
        FString                     AsString;

        TVector<FScriptPropertyValue> Items;             ///< When Kind == Array.
        TVector<FScriptPropertyEntry> StructFields;      ///< When Kind == Nested.
    };

    struct FScriptPropertyEntry
    {
        FName                       Name;
        FScriptPropertyValue        Value;
    };
}
