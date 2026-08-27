#pragma once

#include "Containers/ContainerTraits.h"
#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Lumina
{
    class CStruct;
}

namespace Lumina::ECS
{
    // A dense index, so a storage lookup is an array index and never a hash of a type name.
    using FComponentTypeID = uint16;

    inline constexpr FComponentTypeID InvalidComponentTypeID = 0xFFFFu;

    // Everything the type-erased side needs, resolved once at registration rather than through a meta call.
    struct FComponentTypeInfo
    {
        FName Name;

        // Resolved on demand, since a type id is often first acquired before CObject exists.
        CStruct* (*ResolveStruct)() = nullptr;
        mutable CStruct* CachedStruct = nullptr;

        NODISCARD CStruct* GetStruct() const
        {
            if (CachedStruct == nullptr && ResolveStruct != nullptr)
            {
                CachedStruct = ResolveStruct();
            }
            return CachedStruct;
        }

        // Never resolves, so a worker thread can read it without racing the CObject system.
        NODISCARD CStruct* GetBoundStruct() const { return CachedStruct; }
        uint32   Size      = 0;
        uint32   Alignment = 0;

        // Removal leaves a tombstone rather than swapping, so an element address never moves.
        bool bInPlaceDelete = false;

        // Paged storage. Independent of removal, and chosen so a fat element never pays to be relocated.
        bool bPaged = false;

        // Elements per payload page. Always a power of two, so the pool indexes with a shift and a mask.
        uint32 PageSize = 1024;

        // A tag with no payload, so the storage allocates nothing.
        bool bEmpty = false;

        // Growth and compaction become a single memcpy when the type does not care where it lives.
        bool bTriviallyRelocatable = false;

        bool bTriviallyDestructible = false;

        FComponentTypeID TypeID = InvalidComponentTypeID;

        // The lifetime operations a type-erased path needs. Plain function pointers, so no storage carries a vtable.
        void (*DefaultConstruct)(void* Dest) = nullptr;
        void (*CopyConstruct)(void* Dest, const void* Source) = nullptr;
        void (*Destruct)(void* Dest) = nullptr;

        // Move-constructs into Dest and destroys Source. Null when a memcpy does the same job.
        void (*Relocate)(void* Dest, void* Source) = nullptr;
    };

    // Reserved up front so GetInfo never observes a moved buffer, which is what lets workers read it.
    inline constexpr size_t MaxComponentTypes = 2048;

    class FComponentTypeRegistry
    {
    public:

        static RUNTIME_API FComponentTypeRegistry& Get();

        RUNTIME_API FComponentTypeRegistry();

        // Binds the reflected type at startup, so no later read has to resolve one lazily.
        RUNTIME_API void BindStruct(FComponentTypeID TypeID, CStruct* Struct);

        // Idempotent per name, so a plugin reload and a second DLL both land on the same id.
        RUNTIME_API FComponentTypeID Acquire(const FComponentTypeInfo& Info);

        NODISCARD RUNTIME_API const FComponentTypeInfo& GetInfo(FComponentTypeID TypeID) const;
        NODISCARD RUNTIME_API FComponentTypeID FindByName(const FName& Name) const;
        NODISCARD RUNTIME_API FComponentTypeID FindByStruct(const CStruct* Struct) const;

        NODISCARD RUNTIME_API size_t Num() const;

    private:

        // Boxed, so a reference handed out for one type survives every later registration.
        TVector<TUniquePtr<FComponentTypeInfo>> Types;
    };

    template<typename T>
    concept CHasStaticStruct = requires { { T::StaticStruct() } -> std::convertible_to<CStruct*>; };

    // A pool stores objects, so a reference, a pointer or a cv-qualified spelling is a call-site mistake.
    template<typename T>
    concept CComponent =
        std::is_object_v<T> &&
        !std::is_pointer_v<T> &&
        !std::is_const_v<T> &&
        !std::is_volatile_v<T> &&
        !std::is_array_v<T>;

    enum class EComponentLayout : uint8
    {
        // Paged once the element is large enough that relocating a growing block outweighs a page indirection.
        Automatic,

        // One growing block. Fastest to read at random, and it relocates the payload as it grows.
        Packed,

        // One block per page. Growth allocates rather than relocates, at the cost of an indirection.
        Paged,
    };

    // What a component gets when it says nothing. A type overrides one by declaring a member of that name.
    struct FComponentTraitDefaults
    {
        static constexpr bool InPlaceDelete = false;
        static constexpr EComponentLayout Layout = EComponentLayout::Automatic;
        static constexpr uint32 PageSize = 1024;
    };

    template<typename T>
    NODISCARD consteval bool ReadInPlaceDelete()
    {
        if constexpr (requires { { T::InPlaceDelete } -> std::convertible_to<bool>; })
        {
            return T::InPlaceDelete;
        }
        else
        {
            return FComponentTraitDefaults::InPlaceDelete;
        }
    }

    template<typename T>
    NODISCARD consteval EComponentLayout ReadLayout()
    {
        if constexpr (requires { { T::Layout } -> std::convertible_to<EComponentLayout>; })
        {
            return T::Layout;
        }
        else
        {
            return FComponentTraitDefaults::Layout;
        }
    }

    template<typename T>
    NODISCARD consteval uint32 ReadPageSize()
    {
        if constexpr (requires { { T::PageSize } -> std::convertible_to<uint32>; })
        {
            return static_cast<uint32>(T::PageSize);
        }
        else
        {
            return FComponentTraitDefaults::PageSize;
        }
    }

    // Extracted from the compiler's own signature, so an unreflected component still has a stable name.
    template<typename T>
    NODISCARD consteval std::string_view GetRawTypeSignature()
    {
#if defined(__clang__) || defined(__GNUC__)
        return __PRETTY_FUNCTION__;
#else
        return __FUNCSIG__;
#endif
    }

    // Probed from a type every compiler spells the same, so clang-cl cannot be mistaken for MSVC.
    NODISCARD consteval size_t GetTypeSignaturePrefix()
    {
        return GetRawTypeSignature<int>().find("int");
    }

    NODISCARD consteval size_t GetTypeSignatureSuffix()
    {
        constexpr std::string_view Probe = "int";
        return GetRawTypeSignature<int>().size() - GetTypeSignaturePrefix() - Probe.size();
    }

    template<typename T>
    NODISCARD consteval std::string_view GetSignatureTypeName()
    {
        std::string_view Name = GetRawTypeSignature<T>();
        Name.remove_prefix(GetTypeSignaturePrefix());
        Name.remove_suffix(GetTypeSignatureSuffix());

        // MSVC spells the class key, which carries nothing a name needs.
        if (Name.starts_with("struct "))
        {
            Name.remove_prefix(7);
        }
        else if (Name.starts_with("class "))
        {
            Name.remove_prefix(6);
        }
        else if (Name.starts_with("enum "))
        {
            Name.remove_prefix(5);
        }

        return Name;
    }

    // A canary, so a signature-format change breaks the build rather than registering a garbage name.
    static_assert(GetSignatureTypeName<int>() == std::string_view("int"),
        "the type-name parser no longer matches this compiler's signature format");
    static_assert(GetSignatureTypeName<float>() == std::string_view("float"),
        "the type-name parser no longer matches this compiler's signature format");

    // Measured at 200k entities, relocation and paging cost the same somewhere under 32 bytes.
    inline constexpr uint32 PagedStorageSizeThreshold = 32;

    template<typename T>
    struct TComponentTraits
    {
        static constexpr bool bEmpty = std::is_empty_v<T>;

        //~ What the component asked for, or the default when it asked for nothing.

        static constexpr bool InPlaceDelete = ReadInPlaceDelete<T>();
        static constexpr EComponentLayout Layout = ReadLayout<T>();
        static constexpr uint32 PageSize = ReadPageSize<T>();

        // In-place delete needs a fixed address, and a fat element is paged to avoid the relocation.
        static constexpr bool bPaged = !bEmpty &&
            (InPlaceDelete
                || Layout == EComponentLayout::Paged
                || (Layout == EComponentLayout::Automatic && sizeof(T) > PagedStorageSizeThreshold));

        static_assert(!(InPlaceDelete && Layout == EComponentLayout::Packed),
            "a component cannot be packed and in-place-delete at once, because a packed pool relocates as it grows");

        static_assert(PageSize != 0u && (PageSize & (PageSize - 1u)) == 0u,
            "PageSize must be a non-zero power of two, because the pool indexes a page with a shift and a mask");

        static CStruct* (*GetStructResolver())()
        {
            if constexpr (CHasStaticStruct<T>)
            {
                return +[]() -> CStruct* { return T::StaticStruct(); };
            }
            else
            {
                return nullptr;
            }
        }

        // The compiler's own spelling, so a name is available before the CObject system exists.
        static FName GetName()
        {
            constexpr std::string_view Name = GetSignatureTypeName<T>();
            return FName(FStringView(Name.data(), Name.size()));
        }
    };

    template<typename T>
    FComponentTypeInfo MakeComponentTypeInfo()
    {
        FComponentTypeInfo Info;
        Info.Name          = TComponentTraits<T>::GetName();
        Info.ResolveStruct = TComponentTraits<T>::GetStructResolver();
        Info.Size      = TComponentTraits<T>::bEmpty ? 0u : static_cast<uint32>(sizeof(T));
        Info.Alignment = static_cast<uint32>(alignof(T));
        Info.bInPlaceDelete = TComponentTraits<T>::InPlaceDelete;
        Info.bPaged    = TComponentTraits<T>::bPaged;
        Info.bEmpty    = TComponentTraits<T>::bEmpty;
        Info.PageSize  = TComponentTraits<T>::PageSize;

        Info.bTriviallyRelocatable  = std::is_trivially_copyable_v<T> || TIsTriviallyRelocatable_V<T>;
        Info.bTriviallyDestructible = std::is_trivially_destructible_v<T>;

        if constexpr (!TComponentTraits<T>::bEmpty)
        {
            Info.Destruct = +[](void* Dest) { static_cast<T*>(Dest)->~T(); };

            if constexpr (std::is_default_constructible_v<T>)
            {
                Info.DefaultConstruct = +[](void* Dest) { new (Dest) T(); };
            }

            if constexpr (std::is_copy_constructible_v<T>)
            {
                Info.CopyConstruct = +[](void* Dest, const void* Source) { new (Dest) T(*static_cast<const T*>(Source)); };
            }

            if constexpr (!(std::is_trivially_copyable_v<T> || TIsTriviallyRelocatable_V<T>)
                && std::is_move_constructible_v<T>)
            {
                Info.Relocate = +[](void* Dest, void* Source)
                {
                    T* Moved = static_cast<T*>(Source);
                    new (Dest) T(std::move(*Moved));
                    Moved->~T();
                };
            }
        }

        return Info;
    }

    // A tag, which carries membership and nothing else. Reading one is meaningless, so the API forbids it.
    template<typename T>
    concept CTagComponent = CComponent<T> && TComponentTraits<T>::bEmpty;

    // A component with a value the caller can read and write.
    template<typename T>
    concept CDataComponent = CComponent<T> && !TComponentTraits<T>::bEmpty;

    template<typename T, typename... Ts>
    inline constexpr bool bIsOneOf = (std::is_same_v<T, Ts> || ...);

    template<typename T, typename... Ts>
    NODISCARD consteval size_t CountOccurrences()
    {
        return (static_cast<size_t>(std::is_same_v<T, Ts>) + ... + 0u);
    }

    // A repeated type in a view's pack is always a mistake, and it would silently double every lookup.
    template<typename... Ts>
    NODISCARD consteval bool AreAllDistinct()
    {
        return ((CountOccurrences<Ts, Ts...>() == 1u) && ... && true);
    }

    // The local static is a per-module cache of a process-wide id, so a duplicate across DLLs is harmless.
    template<typename T>
    NODISCARD FORCEINLINE FComponentTypeID GetComponentTypeID()
    {
        static const FComponentTypeID TypeID = FComponentTypeRegistry::Get().Acquire(MakeComponentTypeInfo<T>());
        return TypeID;
    }
}
