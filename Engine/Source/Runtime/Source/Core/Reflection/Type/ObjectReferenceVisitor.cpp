#include "RuntimePCH.h"

#include "ObjectReferenceVisitor.h"

#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/ClassProperty.h"
#include "Core/Reflection/Type/Properties/InstancedStructProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Reflection/Type/Properties/SoftObjectProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Reflection/Type/Properties/SubStructProperty.h"

namespace Lumina
{
    namespace
    {
        using FSlotFunc     = FObjectReferenceVisitor::FSlotFunc;
        using FSoftSlotFunc = FObjectReferenceVisitor::FSoftSlotFunc;

        // One walk serves both entry points; the soft callback is a pointer because a TFunctionRef cannot be
        // absent, and a caller that does not want soft references should not have to invent one.
        void VisitStructImpl(const CStruct* Struct, void* Instance, FSlotFunc Func, const FSoftSlotFunc* SoftFunc);

        void VisitPropertyImpl(const FProperty* Property, void* Value, FSlotFunc Func, const FSoftSlotFunc* SoftFunc)
        {
            if (Property == nullptr || Value == nullptr)
            {
                return;
            }

            switch (Property->GetType())
            {
            case EPropertyTypeFlags::Object:
                {
                    auto* Handle = static_cast<TStrongObjectPtr<CObject>*>(Value);
                    CObject* Current = Handle->Get();
                    CObject* Replacement = Func(Current);
                    if (Replacement != Current)
                    {
                        *Handle = Replacement;
                    }
                    break;
                }

            case EPropertyTypeFlags::Class:
                {
                    auto* Slot = static_cast<CClass**>(Value);
                    CObject* Replacement = Func(*Slot);
                    if (Replacement != *Slot)
                    {
                        *Slot = Cast<CClass>(Replacement);
                    }
                    break;
                }

            case EPropertyTypeFlags::SubStruct:
                {
                    auto* Slot = static_cast<CStruct**>(Value);
                    CObject* Replacement = Func(*Slot);
                    if (Replacement != *Slot)
                    {
                        *Slot = Cast<CStruct>(Replacement);
                    }
                    break;
                }

            case EPropertyTypeFlags::SoftObject:
                {
                    if (SoftFunc != nullptr)
                    {
                        (*SoftFunc)(*static_cast<FSoftObjectPath*>(Value));
                    }
                    break;
                }

            case EPropertyTypeFlags::Struct:
                {
                    const auto* Struct = static_cast<const FStructProperty*>(Property);
                    VisitStructImpl(Struct->GetStruct(), Value, Func, SoftFunc);
                    break;
                }

            case EPropertyTypeFlags::InstancedStruct:
                {
                    // The held type is never repointed, and never needs to be: IsInstancableStructType refuses
                    // a CClass, so an instanced struct cannot hold one of the types a reinstance replaces.
                    auto* Instanced = static_cast<FInstancedStruct*>(Value);
                    VisitStructImpl(Instanced->GetScriptStruct(), Instanced->GetMutableMemory(), Func, SoftFunc);
                    break;
                }

            case EPropertyTypeFlags::Vector:
                {
                    const auto* Array = static_cast<const FArrayProperty*>(Property);
                    FProperty* Inner = Array->GetInternalProperty();
                    if (Inner == nullptr || Array->GetOps() == nullptr)
                    {
                        break;
                    }

                    const SIZE_T Count = Array->GetNum(Value);
                    for (SIZE_T Index = 0; Index < Count; ++Index)
                    {
                        VisitPropertyImpl(Inner, Array->GetAt(Value, Index), Func, SoftFunc);
                    }
                    break;
                }

            case EPropertyTypeFlags::Map:
                {
                    const auto* Map = static_cast<const FMapProperty*>(Property);
                    if (Map->GetOps() == nullptr)
                    {
                        break;
                    }

                    // A key is reported but never rewritten: changing one in place leaves it in the bucket its
                    // old hash chose, so the entry becomes unfindable. Remapping keys means rebuilding the map.
                    FSlotFunc ReadOnly = [&Func](CObject* Current) -> CObject*
                    {
                        Func(Current);
                        return Current;
                    };

                    const SIZE_T Count = Map->GetNum(Value);
                    for (SIZE_T Index = 0; Index < Count; ++Index)
                    {
                        if (FProperty* KeyProperty = Map->GetKeyProperty())
                        {
                            VisitPropertyImpl(KeyProperty, const_cast<void*>(Map->GetKeyAt(Value, Index)),
                                ReadOnly, SoftFunc);
                        }
                        if (FProperty* ValueProperty = Map->GetValueProperty())
                        {
                            VisitPropertyImpl(ValueProperty, Map->GetValueAt(Value, Index), Func, SoftFunc);
                        }
                    }
                    break;
                }

            case EPropertyTypeFlags::Optional:
                {
                    const auto* Optional = static_cast<const FOptionalProperty*>(Property);
                    if (Optional->GetInternalProperty() != nullptr && Optional->HasValue(Value))
                    {
                        VisitPropertyImpl(Optional->GetInternalProperty(), Optional->GetValue(Value), Func, SoftFunc);
                    }
                    break;
                }

            default:
                // A delegate binds a managed thunk rather than an object, so it holds nothing to repoint.
                break;
            }
        }

        void VisitStructImpl(const CStruct* Struct, void* Instance, FSlotFunc Func, const FSoftSlotFunc* SoftFunc)
        {
            if (Struct == nullptr || Instance == nullptr)
            {
                return;
            }

            for (FProperty* Property : Struct->GetProperties())
            {
                if (Property != nullptr)
                {
                    VisitPropertyImpl(Property, Property->GetValuePtr<void>(Instance), Func, SoftFunc);
                }
            }
        }
    }

    void FObjectReferenceVisitor::VisitStruct(const CStruct* Struct, void* Instance, FSlotFunc Func)
    {
        VisitStructImpl(Struct, Instance, Func, nullptr);
    }

    void FObjectReferenceVisitor::VisitProperty(const FProperty* Property, void* Value, FSlotFunc Func)
    {
        VisitPropertyImpl(Property, Value, Func, nullptr);
    }

    void FObjectReferenceVisitor::VisitStructWithSoft(const CStruct* Struct, void* Instance,
        FSlotFunc Func, FSoftSlotFunc SoftFunc)
    {
        VisitStructImpl(Struct, Instance, Func, &SoftFunc);
    }

    void FObjectReferenceVisitor::VisitPropertyWithSoft(const FProperty* Property, void* Value,
        FSlotFunc Func, FSoftSlotFunc SoftFunc)
    {
        VisitPropertyImpl(Property, Value, Func, &SoftFunc);
    }
}
