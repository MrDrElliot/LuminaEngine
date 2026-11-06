#pragma once

#include <array>
#include <type_traits>
#include <utility>
#include "Containers/ContainerOps.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Object/SubclassOf.h"
#include "Core/Reflection/ReflectionDesc.h"
#include "World/ECS/Entity.h"
#include "World/Entity/Components/ComponentRef.h"

namespace Lumina
{
    class CObject;

    // An object argument in a call frame, held as a handle to CObject because the class it names may only be forward declared.
    template<typename T>
    struct TFrameObject : TStrongObjectPtr<CObject>
    {
        using TStrongObjectPtr<CObject>::TStrongObjectPtr;
        using TStrongObjectPtr<CObject>::operator=;
    };

    template<typename T>
    struct TPropertyTypeOf;

    // The descriptor for a C++ type, shared by every property of that type.
    template<typename T>
    inline constexpr const FPropertyType* PropertyTypeOf = &TPropertyTypeOf<std::remove_cvref_t<T>>::Value;

    namespace PropertyTypeTraits
    {
        template<typename T>
        consteval EPropertyFlags FlagsOf()
        {
            const EPropertyFlags Trivial = std::is_trivial_v<T> && std::is_standard_layout_v<T> ? EPropertyFlags::Trivial : EPropertyFlags::None;
            const EPropertyFlags Builtin = std::is_fundamental_v<T> ? EPropertyFlags::Builtin : EPropertyFlags::None;
            return Trivial | Builtin;
        }

        template<typename T>
        consteval EPropertyTypeFlags ArithmeticKind()
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                return EPropertyTypeFlags::Bool;
            }
            else if constexpr (std::is_floating_point_v<T>)
            {
                return sizeof(T) == sizeof(float) ? EPropertyTypeFlags::Float : EPropertyTypeFlags::Double;
            }
            else if constexpr (std::is_signed_v<T>)
            {
                constexpr EPropertyTypeFlags Kinds[] = { EPropertyTypeFlags::Int8, EPropertyTypeFlags::Int16, EPropertyTypeFlags::None, EPropertyTypeFlags::Int32,
                                                         EPropertyTypeFlags::None, EPropertyTypeFlags::None, EPropertyTypeFlags::None, EPropertyTypeFlags::Int64 };
                return Kinds[sizeof(T) - 1];
            }
            else
            {
                constexpr EPropertyTypeFlags Kinds[] = { EPropertyTypeFlags::UInt8, EPropertyTypeFlags::UInt16, EPropertyTypeFlags::None, EPropertyTypeFlags::UInt32,
                                                         EPropertyTypeFlags::None, EPropertyTypeFlags::None, EPropertyTypeFlags::None, EPropertyTypeFlags::UInt64 };
                return Kinds[sizeof(T) - 1];
            }
        }

        template<typename T>
        consteval FPropertyType Plain(EPropertyTypeFlags Kind)
        {
            return FPropertyType{ .Kind = Kind, .Flags = FlagsOf<T>() };
        }

        template<typename TValue, typename TClass>
        consteval FPropertyType ObjectOf(EPropertyTypeFlags Kind)
        {
            return FPropertyType{ .Kind = Kind, .Flags = FlagsOf<TValue>(), .Link = { .Class = &StaticClass<std::remove_cv_t<TClass>> } };
        }

        template<typename TValue, typename TStruct>
        consteval FPropertyType StructOf(EPropertyTypeFlags Kind)
        {
            return FPropertyType{ .Kind = Kind, .Flags = FlagsOf<TValue>(), .Link = { .Struct = &StaticStruct<std::remove_cv_t<TStruct>> } };
        }

        template<typename TPack, size_t Index>
        consteval uint32 ArgumentOffset()
        {
            if constexpr (Index == 0)      { return (uint32)TPack::Off0(); }
            else if constexpr (Index == 1) { return (uint32)TPack::Off1(); }
            else if constexpr (Index == 2) { return (uint32)TPack::Off2(); }
            else if constexpr (Index == 3) { return (uint32)TPack::Off3(); }
            else if constexpr (Index == 4) { return (uint32)TPack::Off4(); }
            else                           { return (uint32)TPack::Off5(); }
        }

        template<typename TPack, size_t... Indices>
        consteval std::array<uint32, sizeof...(Indices)> ArgumentOffsets(std::index_sequence<Indices...>)
        {
            return { ArgumentOffset<TPack, Indices>()... };
        }
    }

    // Anything not matched below is a reflected struct held by value.
    template<typename T>
    struct TPropertyTypeOf
    {
        static_assert(std::is_class_v<T>, "This type cannot be reflected as a property.");
        static constexpr FPropertyType Value = PropertyTypeTraits::StructOf<T, T>(EPropertyTypeFlags::Struct);
    };

    template<typename T> requires std::is_arithmetic_v<T>
    struct TPropertyTypeOf<T>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::Plain<T>(PropertyTypeTraits::ArithmeticKind<T>());
    };

    template<typename T> requires std::is_enum_v<T>
    struct TPropertyTypeOf<T>
    {
        static constexpr const FPropertyType* Inner[] = { PropertyTypeOf<std::underlying_type_t<T>> };
        static constexpr FPropertyType Value{ .Kind = EPropertyTypeFlags::Enum, .Flags = PropertyTypeTraits::FlagsOf<T>(),
                                              .Link = { .Enum = &StaticEnum<T> }, .Inner = Inner, .NumInner = 1 };
    };

    template<typename T>
    struct TPropertyTypeOf<TComponentRef<T>>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::StructOf<TComponentRef<T>, T>(EPropertyTypeFlags::ComponentRef);
    };

    template<>
    struct TPropertyTypeOf<FString>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::Plain<FString>(EPropertyTypeFlags::String);
    };

    template<>
    struct TPropertyTypeOf<FName>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::Plain<FName>(EPropertyTypeFlags::Name);
    };

    template<>
    struct TPropertyTypeOf<ECS::FEntity>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::Plain<ECS::FEntity>(EPropertyTypeFlags::Entity);
    };

    template<typename T> requires std::is_class_v<T>
    struct TPropertyTypeOf<T*>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::ObjectOf<T*, T>(EPropertyTypeFlags::Object);
    };

    template<>
    struct TPropertyTypeOf<CClass*>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::ObjectOf<CClass*, CObject>(EPropertyTypeFlags::Class);
    };

    template<typename T>
    struct TPropertyTypeOf<TStrongObjectPtr<T>>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::ObjectOf<TStrongObjectPtr<T>, T>(EPropertyTypeFlags::Object);
    };

    template<typename T>
    struct TPropertyTypeOf<TWeakObjectPtr<T>>
    {
        static_assert(!sizeof(TWeakObjectPtr<T>*), "A TWeakObjectPtr is a handle half the size of the strong pointer an object property reads and writes, so reflect a TStrongObjectPtr instead.");
    };

    template<typename T>
    struct TPropertyTypeOf<TFrameObject<T>>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::ObjectOf<TFrameObject<T>, T>(EPropertyTypeFlags::Object);
    };

    template<typename T>
    struct TPropertyTypeOf<TSubclassOf<T>>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::ObjectOf<TSubclassOf<T>, T>(EPropertyTypeFlags::Class);
    };

    template<typename T>
    struct TPropertyTypeOf<TSoftObjectPtr<T>>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::ObjectOf<TSoftObjectPtr<T>, T>(EPropertyTypeFlags::SoftObject);
    };

    template<>
    struct TPropertyTypeOf<FSoftObjectPath>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::ObjectOf<FSoftObjectPath, CObject>(EPropertyTypeFlags::SoftObject);
    };

    template<typename T>
    struct TPropertyTypeOf<TSubStructOf<T>>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::StructOf<TSubStructOf<T>, T>(EPropertyTypeFlags::SubStruct);
    };

    template<typename T>
    struct TPropertyTypeOf<TInstancedStruct<T>>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::StructOf<TInstancedStruct<T>, T>(EPropertyTypeFlags::InstancedStruct);
    };

    template<>
    struct TPropertyTypeOf<FInstancedStruct>
    {
        static constexpr FPropertyType Value = PropertyTypeTraits::Plain<FInstancedStruct>(EPropertyTypeFlags::InstancedStruct);
    };

    template<typename T, size_t InlineCapacity, ContainerAllocatorType TAllocator>
    struct TPropertyTypeOf<Containers::TVector<T, InlineCapacity, TAllocator>>
    {
        using TContainer = Containers::TVector<T, InlineCapacity, TAllocator>;
        static constexpr const FPropertyType* Inner[] = { PropertyTypeOf<T> };
        static constexpr FPropertyType Value{ .Kind = EPropertyTypeFlags::Vector, .Flags = PropertyTypeTraits::FlagsOf<TContainer>(),
                                              .Link = { .Vector = &GetVectorOpsFor<TContainer> }, .Inner = Inner, .NumInner = 1 };
    };

    template<typename K, typename V, typename THasher, typename TEqual, ContainerAllocatorType TAllocator, size_t InlineCapacity>
    struct TPropertyTypeOf<Containers::TBasicHashMap<Containers::TMapPolicy<K, V>, THasher, TEqual, TAllocator, InlineCapacity>>
    {
        using TContainer = Containers::TBasicHashMap<Containers::TMapPolicy<K, V>, THasher, TEqual, TAllocator, InlineCapacity>;
        static constexpr const FPropertyType* Inner[] = { PropertyTypeOf<K>, PropertyTypeOf<V> };
        static constexpr FPropertyType Value{ .Kind = EPropertyTypeFlags::Map, .Flags = PropertyTypeTraits::FlagsOf<TContainer>(),
                                              .Link = { .Map = &GetMapOpsFor<TContainer> }, .Inner = Inner, .NumInner = 2 };
    };

    template<typename T>
    struct TPropertyTypeOf<Containers::TOptional<T>>
    {
        using TContainer = Containers::TOptional<T>;
        static constexpr const FPropertyType* Inner[] = { PropertyTypeOf<T> };
        static constexpr FPropertyType Value{ .Kind = EPropertyTypeFlags::Optional, .Flags = PropertyTypeTraits::FlagsOf<TContainer>(),
                                              .Link = { .Optional = &GetOptionalOpsFor<TContainer> }, .Inner = Inner, .NumInner = 1 };
    };

    template<typename... TArgs>
    struct TPropertyTypeOf<TScriptDelegate<TArgs...>>
    {
        static constexpr std::array<const FPropertyType*, sizeof...(TArgs)> Inner = { PropertyTypeOf<TArgs>... };
        static constexpr std::array<uint32, sizeof...(TArgs)> Offsets =
            PropertyTypeTraits::ArgumentOffsets<TArgPack<TArgs...>>(std::index_sequence_for<TArgs...>());

        // A delegate is bound at runtime, so it is never saved and never assigned through reflection.
        static constexpr FPropertyType Value{ .Kind = EPropertyTypeFlags::Delegate,
                                              .Flags = EPropertyFlags::NoSerialize | EPropertyFlags::ReadOnly,
                                              .Inner = Inner.data(), .InnerOffsets = Offsets.data(), .NumInner = (uint8)sizeof...(TArgs) };
    };
}
