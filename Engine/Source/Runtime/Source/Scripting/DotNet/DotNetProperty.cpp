#include "RuntimePCH.h"

#include "DotNetExport.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
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
