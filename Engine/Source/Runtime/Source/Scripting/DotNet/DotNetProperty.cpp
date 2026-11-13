#include "RuntimePCH.h"
#include "Scripting/ScriptFunctionMint.h"

#include "DotNetExport.h"
#include "ExportSignature.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Reflection/Type/Properties/DelegateProperty.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Templates/IntegerCompare.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Reflection/Type/Properties/ClassProperty.h"
#include "Core/Reflection/Type/Properties/SubStructProperty.h"
#include "Scripting/ScriptCallback.h"
#include "Scripting/ScriptTypeRegistry.h"
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
    using DotNet::CopyOut;

    // operator* hands back a ring-buffer pointer, so the copy happens before anything can intern a name.
    int32 CopyOut(const FName& Name, char* Buf, int Cap)
    {
        const char* Text = *Name;
        return CopyOut(Text != nullptr ? FStringView(Text, Name.Len()) : FStringView(), Buf, Cap);
    }

    const CStruct* FindReflectedType(const char* Type, int TLen)
    {
        if (Type == nullptr || TLen <= 0)
        {
            return nullptr;
        }
        // C# names every type by NativeTypeName.Of, which is exactly the name it was registered or minted under.
        const FName Name{FStringView(Type, (size_t)TLen)};
        if (CClass* AsClass = FindObject<CClass>(Name))
        {
            return AsClass;
        }
        return ResolveStructByName(Name);
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

    LOG_ERROR("FindProperty: '{}' has no reflected property '{}'.", Struct->GetName(), PropName);
    return nullptr;
}

// Resolved once per blittable property, which then reads native memory directly at that offset.
LUMINA_DOTNET_EXPORT(int32, PropertyOffset)(const void* Prop)
{
    return Prop ? (int32)static_cast<const FProperty*>(Prop)->Offset : -1;
}

// The width of one value in a container or a call frame, which for an enum is its underlying width.
LUMINA_DOTNET_EXPORT(int32, PropertySize)(const void* Prop)
{
    return Prop ? (int32)static_cast<const FProperty*>(Prop)->GetElementSize() : -1;
}

// One crossing resolves type and property to an offset, which the blittable path caches.
LUMINA_DOTNET_EXPORT(int32, PropertyOffsetByName)(const char* Type, int TLen, const char* Prop, int PLen)
{
    const void* Property = LuminaSharp_FindProperty(Type, TLen, Prop, PLen);
    return LuminaSharp_PropertyOffset(Property);
}

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

// Constructs a CObject from script, in the transient package when Package is null, with no reference held for the caller.
LUMINA_DOTNET_EXPORT(void*, NewObject)(void* Class, void* Package, const char* Name, int NameLen)
{
    CClass* ObjectClass = static_cast<CClass*>(Class);
    if (ObjectClass == nullptr)
    {
        LOG_ERROR("NewObject from script was given no class");
        return nullptr;
    }

    // Every generated class carries a factory, but a hand-written one may omit it and EmplaceInstance would assert without naming it.
    if (ObjectClass->FactoryFunction == nullptr)
    {
        LOG_ERROR("NewObject from script: '{}' cannot be instantiated, it has no factory", ObjectClass->GetName());
        return nullptr;
    }

    CPackage* Outer = (Package != nullptr) ? static_cast<CPackage*>(Package) : CPackage::GetTransientPackage();

    const FName ObjectName = (Name != nullptr && NameLen > 0)
        ? FName(FStringView(Name, (size_t)NameLen))
        : NAME_None;

    return NewObject(ObjectClass, Outer, ObjectName, FGuid::New());
}

// Reflected functions, which the managed dispatcher walks to read a call frame like any other container.

LUMINA_DOTNET_EXPORT(int32, FunctionParamCount)(const void* Function)
{
    return Function ? static_cast<const FFunction*>(Function)->GetArguments().Num() : 0;
}

LUMINA_DOTNET_EXPORT(const void*, FunctionParamAt)(const void* Function, int32 Index)
{
    if (Function == nullptr || Index < 0)
    {
        return nullptr;
    }
    const TSpan<FProperty* const> Arguments = static_cast<const FFunction*>(Function)->GetArguments();
    return Cmp::Less(Index, Arguments.Num()) ? Arguments[Index] : nullptr;
}

LUMINA_DOTNET_EXPORT(const void*, FunctionReturnParam)(const void* Function)
{
    return Function ? static_cast<const FFunction*>(Function)->GetReturnParam() : nullptr;
}

// Publishing is a cache fill on the function, so dispatch stops routing through the shared dispatcher.
LUMINA_DOTNET_EXPORT(void, FunctionPublishInvoker)(const void* Function, void* Invoker, const void* Offsets)
{
    if (Function != nullptr)
    {
        Lumina::Scripting::PublishManagedInvoker(*static_cast<const Lumina::FFunction*>(Function), Invoker,
            static_cast<const Lumina::int32*>(Offsets));
    }
}

LUMINA_DOTNET_EXPORT(void, FunctionClearInvokers)()
{
    Lumina::Scripting::ClearManagedInvokers();
}

LUMINA_DOTNET_EXPORT(int32, FunctionGetName)(const void* Function, char* Buf, int Cap)
{
    if (Function == nullptr)
    {
        return 0;
    }
    return CopyOut(static_cast<const FFunction*>(Function)->GetFunctionName(), Buf, Cap);
}

LUMINA_DOTNET_EXPORT(int32, ClassGetName)(void* Class, char* Buf, int Cap)
{
    if (Class == nullptr)
    {
        return 0;
    }
    return CopyOut(static_cast<CClass*>(Class)->GetName(), Buf, Cap);
}

LUMINA_DOTNET_EXPORT(void*, ClassGetDefaultObject)(void* Class)
{
    return Class != nullptr ? static_cast<CClass*>(Class)->GetDefaultObject() : nullptr;
}

// Returning a struct by value. The property owns the copy, since only it knows whether the members own memory.
LUMINA_DOTNET_EXPORT(void, PropCopyStruct)(void* C, const void* Prop, const void* Source)
{
    if (C == nullptr || Prop == nullptr || Source == nullptr)
    {
        return;
    }

    const FProperty* Property = static_cast<const FProperty*>(Prop);
    Property->CopyCompleteValue(Property->GetValuePtr<void>(C), Source);
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
    return ResolveStructByName(FName(FStringView(Name, (size_t)Len)));
}

LUMINA_DOTNET_EXPORT(int32, StructGetName)(void* Struct, char* Buf, int Cap)
{
    if (Struct == nullptr)
    {
        return 0;
    }
    return CopyOut(static_cast<CStruct*>(Struct)->GetName(), Buf, Cap);
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

    return CopyOut(StructIdentity(Type), Buf, Cap);
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
    CStruct* Wanted = ResolveStructByName(FName(FStringView(TypeName, (size_t)Len)));
    return (Stored != nullptr && Wanted != nullptr && Stored->IsChildOf(Wanted)) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, InstancedStructInitializeAs)(void* Ptr, const char* TypeName, int Len)
{
    if (Ptr == nullptr)
    {
        return;
    }
    CStruct* Type = (TypeName != nullptr && Len > 0)
        ? ResolveStructByName(FName(FStringView(TypeName, (size_t)Len)))
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
    return CopyOut(static_cast<const FSoftObjectPath*>(PathPtr)->GetPath(), Buf, Cap);
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

// The payload property, so the managed side can size its copy of the value behind the optional.
LUMINA_DOTNET_EXPORT(const void*, PropOptionalInner)(const void* Prop)
{
    if (Prop == nullptr)
    {
        return nullptr;
    }

    const FProperty* Property = static_cast<const FProperty*>(Prop);
    return Property->GetType() == EPropertyTypeFlags::Optional
        ? static_cast<const FOptionalProperty*>(Property)->GetInternalProperty()
        : nullptr;
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

// The same three against the member itself, since a frame slot's address already is the optional.
LUMINA_DOTNET_EXPORT(void*, OptionalValueAt)(void* Member, const void* Prop)
{
    return (Member != nullptr && Prop != nullptr)
        ? static_cast<const FOptionalProperty*>(Prop)->GetValue(Member)
        : nullptr;
}

// A null value engages the optional with a default payload, which is the contract every setter implements.
LUMINA_DOTNET_EXPORT(void, OptionalSetValueAt)(void* Member, const void* Prop, const void* Value)
{
    if (Member != nullptr && Prop != nullptr)
    {
        static_cast<const FOptionalProperty*>(Prop)->SetValue(Member, Value);
    }
}

LUMINA_DOTNET_EXPORT(void, OptionalResetAt)(void* Member, const void* Prop)
{
    if (Member != nullptr && Prop != nullptr)
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

LUMINA_DOTNET_EXPORT(void, SetObjectPtr)(void* Member, void* Value)
{
    if (Member != nullptr)
    {
        *reinterpret_cast<Lumina::TStrongObjectPtr<Lumina::CObject>*>(Member) = static_cast<Lumina::CObject*>(Value);
    }
}

// Through TStrongObjectPtr::Get, so a slot that has moved on reads as null rather than as reclaimed memory.
LUMINA_DOTNET_EXPORT(void*, GetObjectPtr)(const void* Member)
{
    return Member != nullptr
        ? static_cast<void*>(reinterpret_cast<const Lumina::TStrongObjectPtr<Lumina::CObject>*>(Member)->Get())
        : nullptr;
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
        Value = FStringView(Utf8, (size_t)Len);
    }
    else
    {
        Value.Reset();
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
    return CopyOut(*static_cast<const FName*>(NamePtr), Buf, Cap);
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

LUMINA_DOTNET_EXPORT(uint64, DelegateBind)(void* DelegatePtr, uint64 Callback)
{
    if (DelegatePtr == nullptr)
    {
        Scripting::ReleaseScriptCallback(SScriptCallback{ Callback });
        return 0;
    }
    return static_cast<FScriptDelegateBase*>(DelegatePtr)->BindManaged(SScriptCallback{ Callback });
}

// A delegate's argument properties, so the managed side can bind frame slots the way a function does.

LUMINA_DOTNET_EXPORT(int32, DelegateArgCount)(void* PropertyPtr)
{
    const FDelegateProperty* Delegate = static_cast<const FDelegateProperty*>(PropertyPtr);
    return Delegate != nullptr ? Delegate->GetNumArgs() : 0;
}

LUMINA_DOTNET_EXPORT(void*, DelegateArgProperty)(void* PropertyPtr, int32 Index)
{
    const FDelegateProperty* Delegate = static_cast<const FDelegateProperty*>(PropertyPtr);
    if (Delegate == nullptr || Index < 0 || Index >= Delegate->GetNumArgs())
    {
        return nullptr;
    }
    return Delegate->GetArgs()[Index];
}

LUMINA_DOTNET_EXPORT(void, DelegateUnbind)(void* DelegatePtr, uint64 Handle)
{
    FScriptDelegateBase::UnbindManagedAnywhere(DelegatePtr, Handle);
}

// What -= on a script event searches, since only the managed side can compare two handlers.
LUMINA_DOTNET_EXPORT(int32, DelegateManagedListeners)(void* DelegatePtr, uint64* OutIds, uint64* OutTokens, int32 Capacity)
{
    return DelegatePtr != nullptr ? static_cast<FScriptDelegateBase*>(DelegatePtr)->GetManagedListeners(OutIds, OutTokens, Capacity) : 0;
}

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(FindProperty),
    LUMINA_DOTNET_SIG(PropertyOffset),
    LUMINA_DOTNET_SIG(PropertySize),
    LUMINA_DOTNET_SIG(PropertyOffsetByName),
    LUMINA_DOTNET_SIG(SetObjectPtr),
    LUMINA_DOTNET_SIG(GetObjectPtr),
    LUMINA_DOTNET_SIG(PropGetClass),
    LUMINA_DOTNET_SIG(PropSetClass),
    LUMINA_DOTNET_SIG(FindClassByName),
    LUMINA_DOTNET_SIG(NewObject),
    LUMINA_DOTNET_SIG(FunctionParamCount),
    LUMINA_DOTNET_SIG(FunctionParamAt),
    LUMINA_DOTNET_SIG(FunctionReturnParam),
    LUMINA_DOTNET_SIG(FunctionPublishInvoker),
    LUMINA_DOTNET_SIG(FunctionClearInvokers),
    LUMINA_DOTNET_SIG(FunctionGetName),
    LUMINA_DOTNET_SIG(ClassGetName),
    LUMINA_DOTNET_SIG(ClassGetDefaultObject),
    LUMINA_DOTNET_SIG(PropCopyStruct),
    LUMINA_DOTNET_SIG(PropGetSubStruct),
    LUMINA_DOTNET_SIG(PropSetSubStruct),
    LUMINA_DOTNET_SIG(FindStructByName),
    LUMINA_DOTNET_SIG(StructGetName),
    LUMINA_DOTNET_SIG(InstancedStructGetType),
    LUMINA_DOTNET_SIG(InstancedStructGetMemory),
    LUMINA_DOTNET_SIG(InstancedStructIsA),
    LUMINA_DOTNET_SIG(InstancedStructInitializeAs),
    LUMINA_DOTNET_SIG(InstancedStructReset),
    LUMINA_DOTNET_SIG(SoftPathGet),
    LUMINA_DOTNET_SIG(SoftPathSet),
    LUMINA_DOTNET_SIG(StructAssign),
    LUMINA_DOTNET_SIG(PropOptionalInner),
    LUMINA_DOTNET_SIG(PropOptionalGetValue),
    LUMINA_DOTNET_SIG(PropOptionalSetValue),
    LUMINA_DOTNET_SIG(PropOptionalReset),
    LUMINA_DOTNET_SIG(OptionalValueAt),
    LUMINA_DOTNET_SIG(OptionalSetValueAt),
    LUMINA_DOTNET_SIG(OptionalResetAt),
    LUMINA_DOTNET_SIG(PropVectorOps),
    LUMINA_DOTNET_SIG(StringAssign),
    LUMINA_DOTNET_SIG(NameFromString),
    LUMINA_DOTNET_SIG(NameToString),
    LUMINA_DOTNET_SIG(PropMapOps),
    LUMINA_DOTNET_SIG(DelegateBind),
    LUMINA_DOTNET_SIG(DelegateUnbind),
    LUMINA_DOTNET_SIG(DelegateManagedListeners),
    LUMINA_DOTNET_SIG(DelegateArgCount),
    LUMINA_DOTNET_SIG(DelegateArgProperty)
);
