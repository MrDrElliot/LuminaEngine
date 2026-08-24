#include "RuntimePCH.h"

#include "DotNetExport.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Reflection/Type/Properties/ClassProperty.h"
#include "Core/Reflection/Type/Properties/SubStructProperty.h"
#include "Scripting/ScriptDataStruct.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Memory/Memory.h"
#include "Memory/SmartPtr.h"

// The token is an opaque property pointer the C# binding holds and passes back with the container.

using namespace Lumina;

namespace
{
    const CStruct* FindReflectedType(const char* Type, int TLen)
    {
        if (Type == nullptr || TLen <= 0)
        {
            return nullptr;
        }
        const FName Name{FStringView(Type, (size_t)TLen)};
        if (CStruct* AsStruct = FindObject<CStruct>(Name))
        {
            return AsStruct;
        }
        return FindObject<CClass>(Name);
    }
}

// Cached once per property on the C# side, and null when the type or property is not found.
LUMINA_DOTNET_EXPORT(const void*, FindProperty)(const char* Type, int TLen, const char* Prop, int PLen)
{
    const CStruct* Struct = FindReflectedType(Type, TLen);
    if (Struct == nullptr || Prop == nullptr || PLen <= 0)
    {
        LOG_ERROR("FindProperty: reflected type '{}' not found.", FStringView(Type ? Type : "", (size_t)Math::Max(TLen, 0)));
        return nullptr;
    }

    const FName PropName(FStringView(Prop, (size_t)PLen));
    if (FProperty* Found = Struct->GetProperty(PropName))
    {
        return Found;
    }
    
    for (const CStruct* Super = Struct->GetSuperStruct(); Super != nullptr; Super = Super->GetSuperStruct())
    {
        if (FProperty* Found = Super->GetProperty(PropName))
        {
            return Found;
        }
    }

    LOG_ERROR("FindProperty: '{}' has no reflected property '{}'.", Struct->GetName(), PropName);
    return nullptr;
}

// Resolved once per blittable property, which then reads native memory directly at that offset.
LUMINA_DOTNET_EXPORT(int32, PropertyOffset)(const void* Prop)
{
    return Prop ? (int32)static_cast<const FProperty*>(Prop)->Offset : -1;
}

// One crossing resolves type and property to an offset, which the blittable path caches.
LUMINA_DOTNET_EXPORT(int32, PropertyOffsetByName)(const char* Type, int TLen, const char* Prop, int PLen)
{
    const void* Property = LuminaSharp_FindProperty(Type, TLen, Prop, PLen);
    return LuminaSharp_PropertyOffset(Property);
}


LUMINA_DOTNET_EXPORT(void, PropSetString)(void* C, const void* Prop, const char* Utf8, int Len)
{
    if (C == nullptr || Prop == nullptr)
    {
        return;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    FString& Value = *Property->GetValuePtr<FString>(C);
    if (Len > 0)
    {
        Value.assign(Utf8, (size_t)Len);
    }
    else
    {
        Value.clear();
    }
}

// The same buffer-fill shape as a string, with the setter building a name from the UTF-8 bytes.
LUMINA_DOTNET_EXPORT(int32, PropGetName)(void* C, const void* Prop, char* Buf, int Cap)
{
    if (C == nullptr || Prop == nullptr)
    {
        return 0;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    const FName& Value = *Property->GetValuePtr<FName>(C);
    const char* S = Value.c_str();
    const int L = S ? (int)Value.length() : 0;
    if (S && Buf && Cap > 0)
    {
        const int N = L < Cap ? L : Cap;
        for (int i = 0; i < N; ++i)
        {
            Buf[i] = S[i];
        }
    }
    return L;
}

LUMINA_DOTNET_EXPORT(void, PropSetName)(void* C, const void* Prop, const char* Utf8, int Len)
{
    if (C == nullptr || Prop == nullptr)
    {
        return;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    FName& Value = *Property->GetValuePtr<FName>(C);
    Value = (Len > 0) ? FName(FStringView(Utf8, (size_t)Len)) : FName();
}

// Object/TObjectPtr get/set.
LUMINA_DOTNET_EXPORT(void, SetObjectPtr)(void*, void*); // defined in DotNetHost.cpp

LUMINA_DOTNET_EXPORT(void*, PropGetObject)(void* C, const void* Prop)
{
    if (C == nullptr || Prop == nullptr)
    {
        return nullptr;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    const TObjectPtr<CObject>& Value = *Property->GetValuePtr<TObjectPtr<CObject>>(C);
    return (void*)Value.Get();
}

LUMINA_DOTNET_EXPORT(void, PropSetObject)(void* C, const void* Prop, void* Obj)
{
    if (C == nullptr || Prop == nullptr)
    {
        return;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    LuminaSharp_SetObjectPtr(Property->GetValuePtr<TObjectPtr<CObject>>(C), Obj);
}

// Kept as a property-kind exporter so the storage layout stays the path type's own business.
LUMINA_DOTNET_EXPORT(int32, PropGetAssetPath)(void* C, const void* Prop, char* Buf, int Cap)
{
    if (C == nullptr || Prop == nullptr)
    {
        return 0;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    const FStringView Path = Property->GetValuePtr<FSoftObjectPath>(C)->GetPath();
    const int Length = (int)Path.size();
    if (Buf != nullptr && Cap > 0 && Length > 0)
    {
        const int Count = Length < Cap ? Length : Cap;
        for (int Index = 0; Index < Count; ++Index)
        {
            Buf[Index] = Path[Index];
        }
    }
    return Length;
}

LUMINA_DOTNET_EXPORT(void, PropSetAssetPath)(void* C, const void* Prop, const char* Utf8, int Len)
{
    if (C == nullptr || Prop == nullptr)
    {
        return;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    FSoftObjectPath* Value = Property->GetValuePtr<FSoftObjectPath>(C);
    *Value = (Len > 0) ? FSoftObjectPath(FString(Utf8, (size_t)Len)) : FSoftObjectPath();
}

// Works for an array appended to a minted script class, where no generated code exists.
// TSubclassOf is one CClass* at offset 0, but a raw write would skip the MetaClass check its Set does.

LUMINA_DOTNET_EXPORT(void*, PropGetClass)(void* C, const void* Prop)
{
    if (C == nullptr || Prop == nullptr)
    {
        return nullptr;
    }
    // GetValuePtr refuses a pointer value type, so the slot address is taken as bytes and re-typed.
    return *reinterpret_cast<CClass**>(static_cast<const FProperty*>(Prop)->GetValuePtr<uint8>(C));
}

// Rejects a class outside the property's MetaClass, which is the constraint TSubclassOf<T> carries.
LUMINA_DOTNET_EXPORT(void, PropSetClass)(void* C, const void* Prop, void* Class)
{
    if (C == nullptr || Prop == nullptr)
    {
        return;
    }

    const FClassProperty* Property = static_cast<const FClassProperty*>(Prop);
    CClass* Value = static_cast<CClass*>(Class);
    CClass* Meta = Property->GetMetaClass();

    if (Value != nullptr && Meta != nullptr && !Value->IsChildOf(Meta))
    {
        LOG_ERROR("PropSetClass: '{}' does not derive from '{}'.", Value->GetName(), Meta->GetName());
        return;
    }

    *reinterpret_cast<CClass**>(Property->GetValuePtr<uint8>(C)) = Value;
}

LUMINA_DOTNET_EXPORT(void*, FindClassByName)(const char* Name, int Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return nullptr;
    }
    return FindObject<CClass>(FName(FStringView(Name, (size_t)Len)));
}

LUMINA_DOTNET_EXPORT(int32, ClassGetName)(void* Class, char* Buf, int Cap)
{
    if (Class == nullptr)
    {
        return 0;
    }
    const FString Name = static_cast<CClass*>(Class)->GetName().ToString();
    const int Length = (int)Name.size();
    if (Buf != nullptr && Cap > 0 && Length > 0)
    {
        const int Count = Length < Cap ? Length : Cap;
        for (int Index = 0; Index < Count; ++Index)
        {
            Buf[Index] = Name[Index];
        }
    }
    return Length;
}

LUMINA_DOTNET_EXPORT(void*, ClassGetDefaultObject)(void* Class)
{
    return Class != nullptr ? static_cast<CClass*>(Class)->GetDefaultObject() : nullptr;
}

// The struct analog of the TSubclassOf pair, with MetaStruct standing in for MetaClass.

LUMINA_DOTNET_EXPORT(void*, PropGetSubStruct)(void* C, const void* Prop)
{
    if (C == nullptr || Prop == nullptr)
    {
        return nullptr;
    }
    return *reinterpret_cast<CStruct**>(static_cast<const FProperty*>(Prop)->GetValuePtr<uint8>(C));
}

LUMINA_DOTNET_EXPORT(void, PropSetSubStruct)(void* C, const void* Prop, void* Struct)
{
    if (C == nullptr || Prop == nullptr)
    {
        return;
    }

    const FSubStructProperty* Property = static_cast<const FSubStructProperty*>(Prop);
    CStruct* Value = static_cast<CStruct*>(Struct);
    CStruct* Meta = Property->GetMetaStruct();

    if (Value != nullptr && Meta != nullptr && !Value->IsChildOf(Meta))
    {
        LOG_ERROR("PropSetSubStruct: '{}' does not derive from '{}'.", Value->GetName(), Meta->GetName());
        return;
    }

    *reinterpret_cast<CStruct**>(Property->GetValuePtr<uint8>(C)) = Value;
}

// Resolves a script-declared struct as well as a native one, matching what InstancedStruct accepts.
LUMINA_DOTNET_EXPORT(void*, FindStructByName)(const char* Name, int Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return nullptr;
    }
    return ResolveDataStructByName(FName(FStringView(Name, (size_t)Len)));
}

LUMINA_DOTNET_EXPORT(int32, StructGetName)(void* Struct, char* Buf, int Cap)
{
    if (Struct == nullptr)
    {
        return 0;
    }
    const FString Name = static_cast<CStruct*>(Struct)->GetName().ToString();
    const int Length = (int)Name.size();
    if (Buf != nullptr && Cap > 0 && Length > 0)
    {
        const int Count = Length < Cap ? Length : Cap;
        for (int Index = 0; Index < Count; ++Index)
        {
            Buf[Index] = Name[Index];
        }
    }
    return Length;
}

// Addressed by raw pointer rather than property, so the same calls serve a member and a container element.

// The registered key rather than the object name, so a script type round-trips across a reload.
LUMINA_DOTNET_EXPORT(int32, InstancedStructGetType)(void* Ptr, char* Buf, int Cap)
{
    if (Ptr == nullptr)
    {
        return 0;
    }
    CStruct* Type = static_cast<FInstancedStruct*>(Ptr)->GetScriptStruct();
    if (Type == nullptr)
    {
        return 0;
    }

    const FString Key = InstancedStructKey(Type).ToString();
    const int Length = (int)Key.size();
    if (Buf != nullptr && Cap > 0 && Length > 0)
    {
        const int Count = Length < Cap ? Length : Cap;
        for (int Index = 0; Index < Count; ++Index)
        {
            Buf[Index] = Key[Index];
        }
    }
    return Length;
}

LUMINA_DOTNET_EXPORT(void*, InstancedStructGetMemory)(void* Ptr)
{
    return Ptr != nullptr ? static_cast<FInstancedStruct*>(Ptr)->GetMutableMemory() : nullptr;
}

// IsChildOf, so a view typed as the base accepts any derived value the slot happens to hold.
LUMINA_DOTNET_EXPORT(int32, InstancedStructIsA)(void* Ptr, const char* TypeName, int Len)
{
    if (Ptr == nullptr || TypeName == nullptr || Len <= 0)
    {
        return 0;
    }
    CStruct* Stored = static_cast<FInstancedStruct*>(Ptr)->GetScriptStruct();
    CStruct* Wanted = ResolveDataStructByName(FName(FStringView(TypeName, (size_t)Len)));
    return (Stored != nullptr && Wanted != nullptr && Stored->IsChildOf(Wanted)) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, InstancedStructInitializeAs)(void* Ptr, const char* TypeName, int Len)
{
    if (Ptr == nullptr)
    {
        return;
    }
    CStruct* Type = (TypeName != nullptr && Len > 0)
        ? ResolveDataStructByName(FName(FStringView(TypeName, (size_t)Len)))
        : nullptr;

    if (Type == nullptr && Len > 0)
    {
        LOG_ERROR("InstancedStructInitializeAs: no reflected struct named '{}'.", FStringView(TypeName, (size_t)Len));
        return;
    }
    static_cast<FInstancedStruct*>(Ptr)->InitializeAs(Type);
}

LUMINA_DOTNET_EXPORT(void, InstancedStructReset)(void* Ptr)
{
    if (Ptr != nullptr)
    {
        static_cast<FInstancedStruct*>(Ptr)->Reset();
    }
}

// The same path access as PropGetAssetPath, but against a bare FSoftObjectPath such as a container element.
LUMINA_DOTNET_EXPORT(int32, SoftPathGet)(const void* PathPtr, char* Buf, int Cap)
{
    if (PathPtr == nullptr)
    {
        return 0;
    }
    const FStringView Path = static_cast<const FSoftObjectPath*>(PathPtr)->GetPath();
    const int Length = (int)Path.size();
    if (Buf != nullptr && Cap > 0 && Length > 0)
    {
        const int Count = Length < Cap ? Length : Cap;
        for (int Index = 0; Index < Count; ++Index)
        {
            Buf[Index] = Path[Index];
        }
    }
    return Length;
}

LUMINA_DOTNET_EXPORT(void, SoftPathSet)(void* PathPtr, const char* Utf8, int Len)
{
    if (PathPtr != nullptr)
    {
        *static_cast<FSoftObjectPath*>(PathPtr) = (Len > 0) ? FSoftObjectPath(FString(Utf8, (size_t)Len)) : FSoftObjectPath();
    }
}

// Copies one reflected struct value over another, so a container element is assigned the way native would.
LUMINA_DOTNET_EXPORT(void, StructAssign)(void* Dst, const void* Src, const char* TypeName, int32 Len)
{
    if (Dst == nullptr || Src == nullptr || TypeName == nullptr || Len <= 0)
    {
        return;
    }

    CStruct* Struct = FindObject<CStruct>(FName(FStringView(TypeName, (size_t)Len)));
    if (Struct == nullptr)
    {
        LOG_ERROR("StructAssign: no reflected struct named '{}'.", FStringView(TypeName, (size_t)Len));
        return;
    }

    Struct->CopyStruct(Dst, Src);
}

// TOptional accessors take the optional member itself, so every one of these resolves the offset first.
static void* OptionalMember(void* Container, const void* Prop)
{
    const auto* Optional = static_cast<const FOptionalProperty*>(Prop);
    return (Container != nullptr && Optional != nullptr) ? Optional->GetValuePtr<void>(Container) : nullptr;
}

LUMINA_DOTNET_EXPORT(int32, PropOptionalHasValue)(void* C, const void* Prop)
{
    void* Member = OptionalMember(C, Prop);
    return Member != nullptr && static_cast<const FOptionalProperty*>(Prop)->HasValue(Member) ? 1 : 0;
}

// Null when unset, which is what the managed side turns into a null Nullable.
LUMINA_DOTNET_EXPORT(void*, PropOptionalGetValue)(void* C, const void* Prop)
{
    void* Member = OptionalMember(C, Prop);
    return Member != nullptr ? static_cast<const FOptionalProperty*>(Prop)->GetValue(Member) : nullptr;
}

LUMINA_DOTNET_EXPORT(void, PropOptionalSetValue)(void* C, const void* Prop, const void* Value)
{
    if (void* Member = OptionalMember(C, Prop))
    {
        static_cast<const FOptionalProperty*>(Prop)->SetValue(Member, Value);
    }
}

LUMINA_DOTNET_EXPORT(void, PropOptionalReset)(void* C, const void* Prop)
{
    if (void* Member = OptionalMember(C, Prop))
    {
        static_cast<const FOptionalProperty*>(Prop)->Reset(Member);
    }
}

LUMINA_DOTNET_EXPORT(const void*, PropVectorOps)(const void* Prop)
{
    if (Prop == nullptr)
    {
        return nullptr;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    if (Property->TypeFlags != EPropertyTypeFlags::Vector)
    {
        return nullptr;
    }
    return static_cast<const FArrayProperty*>(Property)->GetOps();
}

// Reading needs no export, since the managed marshal decodes the native string in place.
LUMINA_DOTNET_EXPORT(void, StringAssign)(void* StringPtr, const char* Utf8, int Len)
{
    if (StringPtr == nullptr)
    {
        return;
    }
    FString& Value = *static_cast<FString*>(StringPtr);
    if (Len > 0)
    {
        Value.assign(Utf8, (size_t)Len);
    }
    else
    {
        Value.clear();
    }
}

// Address-based rather than property-based, so it also serves an FName inside a container.
LUMINA_DOTNET_EXPORT(void, NameFromString)(const char* Utf8, int Len, void* OutName)
{
    if (OutName == nullptr)
    {
        return;
    }
    *static_cast<FName*>(OutName) = (Utf8 != nullptr && Len > 0) ? FName(FStringView(Utf8, (size_t)Len)) : FName();
}

LUMINA_DOTNET_EXPORT(int32, NameToString)(const void* NamePtr, char* Buf, int Cap)
{
    if (NamePtr == nullptr)
    {
        return 0;
    }
    // c_str hands back a ring-buffer pointer, so the copy happens before anything can intern a name.
    const FName& Value = *static_cast<const FName*>(NamePtr);
    const char* S = Value.c_str();
    const int L = S ? (int)Value.length() : 0;
    if (S && Buf && Cap > 0)
    {
        const int N = L < Cap ? L : Cap;
        for (int i = 0; i < N; ++i)
        {
            Buf[i] = S[i];
        }
    }
    return L;
}

// The key/value ops table for a map property, the associative counterpart of PropVectorOps.
LUMINA_DOTNET_EXPORT(const void*, PropMapOps)(const void* Prop)
{
    if (Prop == nullptr)
    {
        return nullptr;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    if (Property->TypeFlags != EPropertyTypeFlags::Map)
    {
        return nullptr;
    }
    return static_cast<const FMapProperty*>(Property)->GetOps();
}

// Script delegate bind/unbind. Game thread only.

LUMINA_DOTNET_EXPORT(uint64, DelegateBind)(void* DelegatePtr, void* Thunk, void* Context)
{
    if (DelegatePtr == nullptr || Thunk == nullptr)
    {
        return 0;
    }
    return static_cast<FScriptDelegateBase*>(DelegatePtr)->BindManaged(
        reinterpret_cast<FScriptDelegateBase::FManagedThunk>(Thunk), Context);
}

LUMINA_DOTNET_EXPORT(void, DelegateUnbind)(void* DelegatePtr, uint64 Handle)
{
    if (DelegatePtr != nullptr)
    {
        static_cast<FScriptDelegateBase*>(DelegatePtr)->UnbindManaged(Handle);
    }
}
