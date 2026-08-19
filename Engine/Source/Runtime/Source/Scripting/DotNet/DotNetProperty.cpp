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

// The scalable property-interop surface that replaces the Reflector's per-property get/set thunks
// (LuminaSharp_Get_<Type>_<Member>). BLITTABLE fields (numeric/bool/enum/blittable-struct) are read and
// written by C# DIRECTLY at the property's runtime-resolved offset (Unsafe.Read/WriteUnaligned), so they
// need no export at all. NON-BLITTABLE fields (FString/FName/object ref) route through this fixed library
// of GENERIC per-FProperty-type exporters: one export per property KIND, reused for every property of that
// kind, with each property's FProperty* token resolved once and cached on the C# side. Net: native exports
// drop from O(properties) to O(property-types).
//
// The token is an opaque const FProperty*; the C# binding holds it and the container pointer (the live
// component/object) and passes both back here. Game thread only.

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

// Resolves "Type::Prop" to the FProperty* token (searches the full inheritance chain). Cached once per
// property on the C# side; null when the type or property isn't found.
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

// The property's byte offset within its container. Resolved once per blittable property on the C# side,
// which then reads/writes native memory directly at (container + offset).
LUMINA_DOTNET_EXPORT(int32, PropertyOffset)(const void* Prop)
{
    return Prop ? (int32)static_cast<const FProperty*>(Prop)->Offset : -1;
}

// Combined resolve: type+prop -> offset in one crossing (the C# blittable path caches just the offset).
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

// FName get/set: same buffer-fill shape as FString; the set builds an FName from the UTF-8 bytes.
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

// Asset reference (FSoftObjectPath) get/set as its virtual path, the same shape C# stores it in. Kept as a
// property-kind exporter rather than reading the embedded FString at a hardcoded offset, so the storage
// layout stays FSoftObjectPath's business.
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

// The element ops table for an array property, so C# can build a TVector<T> view over ANY reflected
// array -- including one appended to a minted script class, where there is no generated code to emit a
// per-property ops export the way the Reflector does for native components.
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

// Assigns an FString that lives at an arbitrary address.
//
// PropSetString addresses a string BY property on an object, which cannot reach a string inside a container:
// a script list of strings holds FStrings packed in the array's own buffer, and the element address comes
// from the ops table, not from an FProperty. Reading needs no export at all (NativeMarshal.ReadString decodes
// the native string in place), so this is the write half only.
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

// FName interning, both directions.
//
// FName is POD -- an interned id plus a number, no heap -- so unlike FString the C# side mirrors it BY VALUE
// and reads and writes the bytes in place at the property's offset, exactly as it does for FVector3. Only
// these two conversions need the name table, so only they cross. The pair is address-based rather than
// property-based so it also serves an FName sitting inside a container, where there is no FProperty to
// address it by.
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
    // c_str() hands back a pointer into a ring buffer, so the copy has to happen before anything else can
    // intern a name. It does: this is the whole body.
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
