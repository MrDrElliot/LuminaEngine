#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class FArchive;
}

// Per-script-type schema + value model bridging the C# [Property] reflection to the native editor.
namespace Lumina::Scripting
{
    // Schema kind. Mirrors LuminaSharp.EScriptKind (same integer values).
    enum class EScriptExportKind : uint8
    {
        Nil = 0,
        Bool,
        I8, I16, I32, I64,
        U8, U16, U32, U64,
        F32, F64,
        String,
        Enum,
        NativeStruct,
        ScriptStruct,
        AssetRef,
        Entity,
        Array,
    };

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

    struct FScriptExportType
    {
        EScriptExportKind             Kind = EScriptExportKind::Nil;

        // Enum kind.
        FName                         EnumName;
        EScriptExportKind             EnumUnderlying = EScriptExportKind::I32;
        TVector<FScriptEnumEntry>     EnumEntries;

        FName                         NativeName;     ///< NativeStruct kind, the native CStruct's name.
        FName                         TargetClass;    ///< AssetRef kind, the asset class filter ("" = any).
        TSharedPtr<FScriptExportType> ElementType;    ///< Array kind.
        TVector<FScriptExportField>   Fields;         ///< NativeStruct / ScriptStruct kind.
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
