#pragma once

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectReinstancer.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptableObject.h"

namespace ScriptTest
{
    inline Lumina::Scripting::FScriptExportField ScalarField(const char* Name, Lumina::EPropertyTypeFlags Kind)
    {
        Lumina::Scripting::FScriptExportField Field;
        Field.Name = Lumina::FName(Name);
        Field.Type = Lumina::MakeShared<Lumina::Scripting::FScriptExportType>();
        Field.Type->Kind = Kind;
        return Field;
    }

    // Mints a C# class the way a reload does, laid out and with its default object built.
    inline Lumina::CScriptClass* MintWith(const char* ClassName, const Lumina::Scripting::FScriptExportSchema& Schema,
        const char* NativeBase = "CScriptableTest")
    {
        using namespace Lumina;

        CScriptClass* Minted = FScriptableRegistry::Mint(ClassName, NativeBase);
        if (Minted == nullptr)
        {
            return nullptr;
        }
        Scripting::AppendScriptPropertiesToClass(Minted, Schema);
        RegisterPendingTypes();
        Minted->GetDefaultObject();
        return Minted;
    }

    // An instance's size is fixed at allocation, so a new layout needs a new class and the reinstancer moves the live instances onto it.
    inline Lumina::CScriptClass* Reshape(Lumina::CScriptClass* Old, const char* NewName,
        const Lumina::Scripting::FScriptExportSchema& Schema, const char* NativeBase = "CScriptableTest")
    {
        using namespace Lumina;

        CScriptClass* New = MintWith(NewName, Schema, NativeBase);
        if (New == nullptr)
        {
            return nullptr;
        }

        FObjectReinstancer Reinstancer;
        Reinstancer.MapClass(Old, New);
        Reinstancer.Commit();
        return New;
    }

    // Publishes a C# enum the way a reload does, so a schema can name it.
    inline void PublishEnum(const char* Name, Lumina::EPropertyTypeFlags Underlying,
        std::initializer_list<Lumina::Scripting::FScriptEnumEntry> Entries)
    {
        using namespace Lumina;

        Scripting::FManagedTypeDefinition Definition;
        Definition.Kind = Scripting::EManagedTypeKind::Enum;
        Definition.TypeName = FName(Name);
        Definition.EnumUnderlying = Underlying;
        Definition.EnumEntries.Reset();
        Definition.EnumEntries.Append(Entries.begin(), Entries.end());
        FScriptEnumRegistry::Get().Refresh(TSpan<const Scripting::FManagedTypeDefinition>(&Definition, 1));
    }

    inline Lumina::Scripting::FManagedTypeDefinition StructDefinition(const char* Name, Lumina::Scripting::FScriptExportSchema Schema)
    {
        Lumina::Scripting::FManagedTypeDefinition Definition;
        Definition.Kind = Lumina::Scripting::EManagedTypeKind::ScriptStruct;
        Definition.TypeName = Lumina::FName(Name);
        Definition.Schema = std::move(Schema);
        Definition.bHasSchema = true;
        return Definition;
    }

    // Publishes C# structs the way a reload does, which supersedes any published earlier and absent here.
    inline void PublishStructs(std::initializer_list<Lumina::Scripting::FManagedTypeDefinition> Definitions)
    {
        Lumina::FScriptStructRegistry::Get().Refresh(
            Lumina::TSpan<const Lumina::Scripting::FManagedTypeDefinition>(Definitions.begin(), Lumina::IntCastChecked<Lumina::int32>(Definitions.size())));
    }
}
