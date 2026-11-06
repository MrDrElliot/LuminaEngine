#pragma once

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/ObjectCore.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

// The schema a C# type describes itself with, in the EPropertyTypeFlags kinds C++ reflection uses.
namespace Lumina::Scripting
{
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
        // Author-declared Lumina::EPropertyFlags from [Property] specifiers, OR'd onto the minted property.
        uint32                        Flags = 0;
        TSharedPtr<FScriptExportType> Type;
        FScriptExportMeta             Meta;      ///< Editor display data, rebuilt per load.
    };

    struct FScriptExportType
    {
        EPropertyTypeFlags            Kind = EPropertyTypeFlags::None;
        bool                          bInputAction = false;  ///< A String that is really an input action name.

        // Enum kind, the reflected enum by name and the width a field of it reserves.
        FName                         EnumName;
        EPropertyTypeFlags            EnumUnderlying = EPropertyTypeFlags::Int32;

        // Struct kind, the reflected struct by name, native or minted from C#.
        FName                         NativeName;

        // Object and SoftObject kinds, the class a reference is limited to, or none for any.
        FName                         TargetClass;

        TSharedPtr<FScriptExportType> ElementType;    ///< Vector and Optional kinds.
        TSharedPtr<FScriptExportType> KeyType;        ///< Map kind, the key shape.
        TSharedPtr<FScriptExportType> ValueType;      ///< Map kind, the value shape.

        // InstancedStruct kind, the struct every value derives from.
        FName                         BaseName;
    };

    // One [Function] method, its name and its parameters as the fields of its call frame.
    struct FScriptExportFunction
    {
        FName                        Name;

        // Arguments in declaration order, with the return value last when there is one.
        TVector<FScriptExportField>  Params;

        // Index into Params of the return value, or -1 for a function that returns nothing.
        int32                        ReturnIndex = -1;

        // EFunctionFlags the declaration adds, such as where a C# [Rpc] method runs.
        uint32                       Flags = 0;

        // What the function carries as metadata, such as the label of a C# [Function(Button)].
        FScriptExportMeta            Meta;
    };

    struct FScriptExportSchema
    {
        TVector<FScriptExportField> Fields;

        // The type's [Function] methods, each describing its own call frame.
        TVector<FScriptExportFunction> Functions;

        // A behavior-only type still mints, for its functions, so a schema with either counts.
        bool IsValid() const { return !Fields.IsEmpty() || !Functions.IsEmpty(); }
    };
}
