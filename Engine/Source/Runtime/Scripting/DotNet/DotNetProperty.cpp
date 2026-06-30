#include "pch.h"

#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Reflection/Type/LuminaTypes.h"
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
    // Resolves a reflected struct OR class by its simple (DisplayName) name. FindObject filters by the
    // exact CClass bucket, so a struct (a CStruct instance) and a class (a CClass instance) live in
    // different buckets; try both. Mirrors the type-by-name lookup the component ops use.
    const CStruct* FindReflectedType(const char* Type, int TLen)
    {
        if (Type == nullptr || TLen <= 0)
        {
            return nullptr;
        }
        const FName Name(Type, (size_t)TLen);
        if (CStruct* AsStruct = FindObject<CStruct>(Name))
        {
            return AsStruct;
        }
        return FindObject<CClass>(Name);
    }
}

// Resolves "Type::Prop" to the FProperty* token (searches the full inheritance chain). Cached once per
// property on the C# side; null when the type or property isn't found.
extern "C" LUMINA_SCRIPT_API const void* LuminaSharp_FindProperty(const char* Type, int TLen, const char* Prop, int PLen)
{
    const CStruct* Struct = FindReflectedType(Type, TLen);
    if (Struct == nullptr || Prop == nullptr || PLen <= 0)
    {
        return nullptr;
    }
    return Struct->GetProperty(FName(FStringView(Prop, (size_t)PLen)));
}

// The property's byte offset within its container. Resolved once per blittable property on the C# side,
// which then reads/writes native memory directly at (container + offset).
extern "C" LUMINA_SCRIPT_API int32 LuminaSharp_PropertyOffset(const void* Prop)
{
    return Prop ? (int32)static_cast<const FProperty*>(Prop)->Offset : -1;
}

// Combined resolve: type+prop -> offset in one crossing (the C# blittable path caches just the offset).
extern "C" LUMINA_SCRIPT_API int32 LuminaSharp_PropertyOffsetByName(const char* Type, int TLen, const char* Prop, int PLen)
{
    const void* Property = LuminaSharp_FindProperty(Type, TLen, Prop, PLen);
    return LuminaSharp_PropertyOffset(Property);
}

//================================================================================================
// Generic per-type exporters. C is the container (component/object) pointer; Prop the cached token.
// Each views the field at the property offset via the FProperty's typed pointer API.
//================================================================================================

// FString get: fills a caller buffer (UTF-8 bytes) and returns the full length. Two-pass protocol matching
// LuminaSharp_GetObjectPath: a first call with a null/0 buffer queries the length, the second copies.
extern "C" LUMINA_SCRIPT_API int32 LuminaSharp_PropGetString(void* C, const void* Prop, char* Buf, int Cap)
{
    if (C == nullptr || Prop == nullptr)
    {
        return 0;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    const FString& Value = *Property->GetValuePtr<FString>(C);
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

extern "C" LUMINA_SCRIPT_API void LuminaSharp_PropSetString(void* C, const void* Prop, const char* Utf8, int Len)
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
extern "C" LUMINA_SCRIPT_API int32 LuminaSharp_PropGetName(void* C, const void* Prop, char* Buf, int Cap)
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

extern "C" LUMINA_SCRIPT_API void LuminaSharp_PropSetName(void* C, const void* Prop, const char* Utf8, int Len)
{
    if (C == nullptr || Prop == nullptr)
    {
        return;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    FName& Value = *Property->GetValuePtr<FName>(C);
    Value = (Len > 0) ? FName(FStringView(Utf8, (size_t)Len)) : FName();
}

// Object/TObjectPtr get/set. The member is a pointer-sized TObjectPtr<T>; read its raw CObject* and write
// via the type-erased setter (which views it as TObjectPtr<CObject> for correct refcounting). Mirrors the
// old per-property object thunk.
extern "C" LUMINA_SCRIPT_API void LuminaSharp_SetObjectPtr(void*, void*); // defined in DotNetHost.cpp

extern "C" LUMINA_SCRIPT_API void* LuminaSharp_PropGetObject(void* C, const void* Prop)
{
    if (C == nullptr || Prop == nullptr)
    {
        return nullptr;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    const TObjectPtr<CObject>& Value = *Property->GetValuePtr<TObjectPtr<CObject>>(C);
    return (void*)Value.Get();
}

extern "C" LUMINA_SCRIPT_API void LuminaSharp_PropSetObject(void* C, const void* Prop, void* Obj)
{
    if (C == nullptr || Prop == nullptr)
    {
        return;
    }
    const FProperty* Property = static_cast<const FProperty*>(Prop);
    LuminaSharp_SetObjectPtr(Property->GetValuePtr<TObjectPtr<CObject>>(C), Obj);
}

// Script delegate bind/unbind. Game thread only.

extern "C" LUMINA_SCRIPT_API uint64 LuminaSharp_DelegateBind(void* DelegatePtr, void* Thunk, void* Context)
{
    if (DelegatePtr == nullptr || Thunk == nullptr)
    {
        return 0;
    }
    return static_cast<FScriptDelegateBase*>(DelegatePtr)->BindManaged(
        reinterpret_cast<FScriptDelegateBase::FManagedThunk>(Thunk), Context);
}

extern "C" LUMINA_SCRIPT_API void LuminaSharp_DelegateUnbind(void* DelegatePtr, uint64 Handle)
{
    if (DelegatePtr != nullptr)
    {
        static_cast<FScriptDelegateBase*>(DelegatePtr)->UnbindManaged(Handle);
    }
}
