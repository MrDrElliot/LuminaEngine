#include "RuntimePCH.h"
#include "ScriptInteropLibrary.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/PropertyFlags.h"
#include "Core/Reflection/Type/PropertyRegistry.h"
#include "Scripting/DotNet/LayoutRegistry.h"

namespace Lumina
{
    int32 CScriptInteropLibrary::GetMirrorSize(const FString& Key)
    {
        return DotNet::GetLayoutSize(*Key, Key.Len());
    }

    int32 CScriptInteropLibrary::GetPropertyTypeValue(const FString& Name)
    {
        const FStringView Wanted(*Name, Name.Len());
        for (int32 Index = 0; Index < (int32)EPropertyTypeFlags::Count; ++Index)
        {
            if (FStringView(PropertyTypePlainNames[Index]) == Wanted)
            {
                return Index;
            }
        }
        return -1;
    }

    int32 CScriptInteropLibrary::GetPropertyTypeCount()
    {
        return (int32)EPropertyTypeFlags::Count;
    }

    int32 CScriptInteropLibrary::GetPropertyFlagValue(const FString& Name)
    {
        const FStringView Wanted(*Name, Name.Len());
        for (size_t Index = 0; Index < std::size(PropertyFlagNames); ++Index)
        {
            if (FStringView(PropertyFlagNames[Index]) == Wanted)
            {
                return (int32)PropertyFlagValues[Index];
            }
        }
        return -1;
    }

    int64 CScriptInteropLibrary::GetValueLayout(int32 Kind, const FString& StructName)
    {
        uint32 Size = 0;
        uint32 Alignment = 0;
        if (Kind == (int32)EPropertyTypeFlags::Struct)
        {
            const CStruct* Struct = StructName.IsEmpty() ? nullptr : FindObject<CStruct>(FName(*StructName));
            if (Struct == nullptr)
            {
                return -1;
            }
            Size = Struct->GetAlignedSize();
            Alignment = Struct->GetAlignment();
        }
        else if (Kind <= 0 || Kind >= (int32)EPropertyTypeFlags::Count
            || !GetPropertyKindOps((EPropertyTypeFlags)Kind).GetFixedLayout(Size, Alignment))
        {
            return -1;
        }
        return (int64)Size | ((int64)Alignment << 32);
    }

    uint64 CScriptInteropLibrary::GetModuleLayoutHash(const FString& Module)
    {
        return FindModuleLayoutHash(FStringView(*Module, Module.Len()));
    }
}
