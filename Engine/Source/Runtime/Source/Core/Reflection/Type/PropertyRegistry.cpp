#include "RuntimePCH.h"
#include "PropertyRegistry.h"

#include "LuminaTypes.h"
#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Properties/ArrayProperty.h"
#include "Properties/ClassProperty.h"
#include "Properties/DelegateProperty.h"
#include "Properties/EnumProperty.h"
#include "Properties/InstancedStructProperty.h"
#include "Properties/MapProperty.h"
#include "Properties/ObjectProperty.h"
#include "Properties/OptionalProperty.h"
#include "Properties/SoftObjectProperty.h"
#include "Properties/StringProperty.h"
#include "Properties/StructProperty.h"
#include "Properties/SubStructProperty.h"

namespace Lumina
{
    namespace
    {
        template<typename TPropertyType, typename TParams>
        FProperty* ConstructKind(const FPropertyOwner& Owner, const FPropertyParams* Params)
        {
            const TParams* Typed = static_cast<const TParams*>(Params);

            // The accessor pair lives on the base params, so this branch is the same for every kind.
            FProperty* Property = (Params->GetterFunc != nullptr || Params->SetterFunc != nullptr)
                ? static_cast<FProperty*>(Owner.Build<TPropertyWithSetterAndGetter<TPropertyType>>(Typed))
                : static_cast<FProperty*>(Owner.Build<TPropertyType>(Typed));

            // The kind states its footprint here and the constructor sets it; a drift between the two would
            // mis-plan a script layout while every native path kept working, so prove they agree.
            DEBUG_ASSERT(GetPropertyKindOps(Params->TypeFlags).Size == 0
                || GetPropertyKindOps(Params->TypeFlags).Size == Property->GetElementSize());

            return Property;
        }

        template<typename TParams>
        void ReadKindMetadata(const FPropertyParams* Params, uint16& OutNum, const FMetaDataPairParam*& OutArray)
        {
            const TParams* Typed = static_cast<const TParams*>(Params);
            OutNum = Typed->NumMetaData;
            OutArray = Typed->MetaDataArray;
        }

        struct FPropertyKindTable
        {
            FPropertyKindOps Entries[(size_t)EPropertyTypeFlags::Count];
            FPropertyKindOps Unregistered;

            // A kind whose footprint is the C++ type it stores.
            template<typename TPropertyType, typename TParams, typename TCppType>
            void Add(EPropertyTypeFlags Kind, bool bArithmetic = false, bool bBaseParamsOnly = false)
            {
                FPropertyKindOps& Entry = Entries[(size_t)Kind];
                Entry.Construct       = &ConstructKind<TPropertyType, TParams>;
                Entry.GetMetadata     = &ReadKindMetadata<TParams>;
                Entry.Size            = sizeof(TCppType);
                Entry.Alignment       = alignof(TCppType);
                Entry.bArithmetic     = bArithmetic;
                Entry.bBaseParamsOnly = bBaseParamsOnly;
            }

            // A kind whose footprint depends on a type resolved at registration, so it cannot be stated here.
            template<typename TPropertyType, typename TParams>
            void AddResolved(EPropertyTypeFlags Kind, uint8 NumInnerParams = 0)
            {
                FPropertyKindOps& Entry = Entries[(size_t)Kind];
                Entry.Construct      = &ConstructKind<TPropertyType, TParams>;
                Entry.GetMetadata    = &ReadKindMetadata<TParams>;
                Entry.NumInnerParams = NumInnerParams;
            }

            FPropertyKindTable()
            {
                Add<FBoolProperty,   FNumericPropertyParams, bool>  (EPropertyTypeFlags::Bool,   true, true);
                Add<FInt8Property,   FNumericPropertyParams, int8>  (EPropertyTypeFlags::Int8,   true, true);
                Add<FInt16Property,  FNumericPropertyParams, int16> (EPropertyTypeFlags::Int16,  true, true);
                Add<FInt32Property,  FNumericPropertyParams, int32> (EPropertyTypeFlags::Int32,  true, true);
                Add<FInt64Property,  FNumericPropertyParams, int64> (EPropertyTypeFlags::Int64,  true, true);
                Add<FUInt8Property,  FNumericPropertyParams, uint8> (EPropertyTypeFlags::UInt8,  true, true);
                Add<FUInt16Property, FNumericPropertyParams, uint16>(EPropertyTypeFlags::UInt16, true, true);
                Add<FUInt32Property, FNumericPropertyParams, uint32>(EPropertyTypeFlags::UInt32, true, true);
                Add<FUInt64Property, FNumericPropertyParams, uint64>(EPropertyTypeFlags::UInt64, true, true);
                Add<FFloatProperty,  FNumericPropertyParams, float> (EPropertyTypeFlags::Float,  true, true);
                Add<FDoubleProperty, FNumericPropertyParams, double>(EPropertyTypeFlags::Double, true, true);

                Add<FStringProperty,     FStringPropertyParams,     FString>             (EPropertyTypeFlags::String, false, true);
                Add<FNameProperty,       FNamePropertyParams,       FName>               (EPropertyTypeFlags::Name,   false, true);
                Add<FObjectProperty,     FObjectPropertyParams,     TObjectPtr<CObject>> (EPropertyTypeFlags::Object);
                Add<FSoftObjectProperty, FSoftObjectPropertyParams, FSoftObjectPath>     (EPropertyTypeFlags::SoftObject);
                Add<FClassProperty,      FClassPropertyParams,      void*>               (EPropertyTypeFlags::Class);
                Add<FSubStructProperty,  FSubStructPropertyParams,  void*>               (EPropertyTypeFlags::SubStruct);
                Add<FDelegateProperty,   FDelegatePropertyParams,   FScriptDelegate>     (EPropertyTypeFlags::Delegate);
                Add<FInstancedStructProperty, FInstancedStructPropertyParams, FInstancedStruct>(EPropertyTypeFlags::InstancedStruct);

                // A struct is as wide as the struct it names, which is only known once that is resolved.
                AddResolved<FStructProperty, FStructPropertyParams>(EPropertyTypeFlags::Struct);

                // The emitter writes each of these an inner params entry, which the recursion consumes.
                AddResolved<FEnumProperty,     FEnumPropertyParams>    (EPropertyTypeFlags::Enum,     1);
                AddResolved<FArrayProperty,    FArrayPropertyParams>   (EPropertyTypeFlags::Vector,   1);
                AddResolved<FOptionalProperty, FOptionalPropertyParams>(EPropertyTypeFlags::Optional, 1);
                AddResolved<FMapProperty,      FMapPropertyParams>     (EPropertyTypeFlags::Map,      2);
            }
        };
    }

    const FPropertyKindOps& GetPropertyKindOps(EPropertyTypeFlags Kind)
    {
        static const FPropertyKindTable Table;

        const size_t Index = (size_t)Kind;
        if (Index >= (size_t)EPropertyTypeFlags::Count)
        {
            return Table.Unregistered;
        }
        return Table.Entries[Index];
    }
}
