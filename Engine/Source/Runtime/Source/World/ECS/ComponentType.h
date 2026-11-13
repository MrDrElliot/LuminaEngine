#pragma once

#include <atomic>

#include "Containers/ContainerTraits.h"
#include "Memory/Construct.h"
#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Memory/SmartPtr.h"
#include "Core/Threading/Sync.h"
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
    class FSparseSet;
    class FRegistry;

    // Hands a run of newly committed events of one type to whatever entity delegates they name.
    using FEventFanOut = void (*)(FRegistry& Registry, const void* Events, uint32 Count);

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

        // A copy is a memcpy, which is what lets an event queue take a send in one block.
        bool bTriviallyCopyable = false;

        // Laid out at runtime from a C# type, so its pools are tracked and laid out again when the type is reshaped.
        bool bRuntimeLayout = false;

        FComponentTypeID TypeID = InvalidComponentTypeID;

        // The lifetime operations a type-erased path needs, handed the info so a runtime layout reaches its struct.
        void (*DefaultConstruct)(const FComponentTypeInfo& Info, void* Dest) = nullptr;
        void (*CopyConstruct)(const FComponentTypeInfo& Info, void* Dest, const void* Source) = nullptr;
        void (*Destruct)(const FComponentTypeInfo& Info, void* Dest) = nullptr;

        // Move-constructs into Dest and destroys Source. Null when a memcpy does the same job.
        void (*Relocate)(const FComponentTypeInfo& Info, void* Dest, void* Source) = nullptr;

        // Set for an event type that delivers to entity delegates, run at the sync point its queue commits on.
        FEventFanOut EventFanOut = nullptr;
    };

    // Carries one element's value from an old layout of its type into a default-constructed new one.
    using FMigrateComponentValue = void (*)(const FComponentTypeInfo& From, const void* FromValue, const FComponentTypeInfo& To, void* ToValue);

    // Reserved up front so GetInfo never observes a moved buffer, which is what lets workers read it.
    inline constexpr size_t MaxComponentTypes = 2048;

    class FComponentTypeRegistry
    {
    public:

        static RUNTIME_API FComponentTypeRegistry& Get();

        RUNTIME_API FComponentTypeRegistry();

        // Binds the reflected type at startup, so no later read has to resolve one lazily.
        RUNTIME_API void BindStruct(FComponentTypeID TypeID, CStruct* Struct);

        RUNTIME_API void SetEventFanOut(FComponentTypeID TypeID, FEventFanOut FanOut);

        // Idempotent per name, so a plugin reload and a second DLL both land on the same id.
        RUNTIME_API FComponentTypeID Acquire(const FComponentTypeInfo& Info);

        NODISCARD RUNTIME_API const FComponentTypeInfo& GetInfo(FComponentTypeID TypeID) const;
        NODISCARD RUNTIME_API FComponentTypeID FindByName(const FName& Name) const;
        NODISCARD RUNTIME_API FComponentTypeID FindByStruct(const CStruct* Struct) const;

        NODISCARD RUNTIME_API int32 Num() const;

        // Gives a runtime-layout type its new layout, and every live pool of it the same, carrying each value across.
        RUNTIME_API void Relayout(FComponentTypeID TypeID, const FComponentTypeInfo& Info, FMigrateComponentValue Migrate);

        RUNTIME_API void TrackPool(FSparseSet* Pool);
        RUNTIME_API void UntrackPool(FSparseSet* Pool);

        // Never freed or reused, because a script view may still read the slot after its pool is destroyed.
        NODISCARD RUNTIME_API uint32* AcquireLayoutEpochSlot();

        RUNTIME_API uint64 AcquireWriteEpochBase();

        NODISCARD RUNTIME_API uint32 GetChangeTick() const;
        RUNTIME_API uint32 AdvanceChangeTick();

    private:

        NODISCARD FComponentTypeID FindByNameLocked(const FName& Name) const;

        // Boxed, so a reference handed out for one type survives every later registration.
        TVector<TUniquePtr<FComponentTypeInfo>> Types;

        // Loader threads first-touch types in parallel, and an unguarded append hands two types one id and one pool.
        mutable FMutex TypesLock;

        // Only pools of runtime-layout types, the ones a reshape has to reach.
        TVector<FSparseSet*> RuntimeLayoutPools;
        FMutex PoolLock;

        static constexpr uint32 LayoutEpochSlotsPerChunk = 1024;
        FMutex LayoutEpochLock;
        uint32* LayoutEpochChunk = nullptr;
        uint32 LayoutEpochUsed = LayoutEpochSlotsPerChunk;

        std::atomic<uint32> NextWriteEpochPool{ 1 };
        std::atomic<uint32> ChangeClock{ 1 };
    };

    template<typename T>
    concept CHasStaticStruct = requires { { T::StaticStruct() } -> std::convertible_to<CStruct*>; };

    // Read<T> reads like const T; Write<T> walks hand out TMut<T>, which stamps a row only on mutable access, while a plain T walks untracked.
    template<typename T>
    struct Read
    {
        using Component = T;
        static constexpr bool bWrite = false;
    };

    template<typename T>
    struct Write
    {
        using Component = T;
        static constexpr bool bWrite = true;
    };

    // One Write<T> row. Const access never stamps; mutable access stores the walk's tick into the row's change-tick slot.
    template<typename T>
    class TMut
    {
    public:

        FORCEINLINE TMut(T& InValue, uint32* InTick, uint32 InNow)
            : Value(&InValue)
            , Tick(InTick)
            , Now(InNow)
        {}

        NODISCARD FORCEINLINE const T& Get() const { return *Value; }
        NODISCARD FORCEINLINE const T* operator -> () const { return Value; }
        NODISCARD FORCEINLINE const T& operator * () const { return *Value; }
        FORCEINLINE operator const T& () const { return *Value; }

        NODISCARD FORCEINLINE T& GetMut() { Stamp(); return *Value; }
        NODISCARD FORCEINLINE T* operator -> () { Stamp(); return Value; }
        NODISCARD FORCEINLINE T& operator * () { Stamp(); return *Value; }

        // Returns whether it wrote, so an unchanged value leaves Changed<T> quiet.
        FORCEINLINE bool SetIfChanged(const T& NewValue) { return AssignIfChanged(*Value, NewValue); }

        template<typename TField>
        FORCEINLINE bool SetIfChanged(TField T::* Field, const std::type_identity_t<TField>& NewValue) { return AssignIfChanged(Value->*Field, NewValue); }

        // For a write no Changed<T> reader should see, such as runtime bookkeeping inside the component.
        NODISCARD FORCEINLINE T& BypassChangeDetection() const { return *Value; }

    private:

        template<typename TDest>
        FORCEINLINE bool AssignIfChanged(TDest& Dest, const TDest& NewValue)
        {
            if (Dest == NewValue)
            {
                return false;
            }
            Dest = NewValue;
            Stamp();
            return true;
        }

        // Relaxed, since parallel walks stamp disjoint rows and an untracked pool's rows share one sink.
        FORCEINLINE void Stamp() { std::atomic_ref<uint32>(*Tick).store(Now, std::memory_order_relaxed); }

        T* Value;
        uint32* Tick;
        uint32 Now;
    };

    template<typename T> inline constexpr bool bIsAccessWrapper           = false;
    template<typename T> inline constexpr bool bIsAccessWrapper<Read<T>>  = true;
    template<typename T> inline constexpr bool bIsAccessWrapper<Write<T>> = true;

    template<typename T> inline constexpr bool bIsWriteAccess           = false;
    template<typename T> inline constexpr bool bIsWriteAccess<Write<T>> = true;

    // Only entities whose T was written, or added, since a query last ran; T need not be an include.
    template<typename T>
    struct Changed
    {
        using Component = T;
    };

    template<typename T>
    struct Added
    {
        using Component = T;
    };

    // Skips entities holding T, in a view or a query: View<ECS::Read<A>, ECS::Without<SDisabledTag>>().
    template<typename T>
    struct Without
    {
        using Component = T;
    };

    // Keeps only entities holding T without reading it, so it declares no access and runs beside writers of T.
    template<typename T>
    struct With
    {
        using Component = T;
    };

    // Hands a query callback a pointer that is null when the entity lacks T, without changing which entities match; a const T or Read<T> spelling reads.
    template<typename T>
    struct Optional
    {
        using Spelling = T;
    };

    template<typename T> inline constexpr bool bIsOptionalTerm              = false;
    template<typename T> inline constexpr bool bIsOptionalTerm<Optional<T>> = true;

    template<typename T> inline constexpr bool bIsChangedTerm             = false;
    template<typename T> inline constexpr bool bIsChangedTerm<Changed<T>> = true;
    template<typename T> inline constexpr bool bIsAddedTerm               = false;
    template<typename T> inline constexpr bool bIsAddedTerm<Added<T>>     = true;
    template<typename T> inline constexpr bool bIsWithoutTerm             = false;
    template<typename T> inline constexpr bool bIsWithoutTerm<Without<T>> = true;
    template<typename T> inline constexpr bool bIsWithTerm                = false;
    template<typename T> inline constexpr bool bIsWithTerm<With<T>>       = true;

    template<typename T>
    inline constexpr bool bIsMembershipTerm = bIsWithoutTerm<T> || bIsWithTerm<T>;

    // A view's membership filter entry: a plain T excludes holders of T, With<T> requires it.
    template<typename T> struct TMembershipFilter          { using Component = T; static constexpr bool bRequired = false; };
    template<typename T> struct TMembershipFilter<With<T>> { using Component = T; static constexpr bool bRequired = true; };

    template<typename T>
    inline constexpr bool bIsFilterTerm = bIsChangedTerm<T> || bIsAddedTerm<T>;

    template<typename T>
    inline constexpr bool bIsQueryTerm = bIsAccessWrapper<T> || bIsFilterTerm<T> || bIsMembershipTerm<T> || bIsOptionalTerm<T>;

    // What a view include stores for each spelling: Read<T> is a const include, Write<T> and bare T are mutable ones.
    template<typename T> struct TAccessViewType           { using Type = T; };
    template<typename T> struct TAccessViewType<Read<T>>  { using Type = const T; };
    template<typename T> struct TAccessViewType<Write<T>> { using Type = T; };

    template<typename T>
    using TViewType = typename TAccessViewType<T>::Type;

    // A view keeps an include as TViewType spells it, except a Write<T> stays spelled so its walks hand out TMut<T>.
    template<typename T> struct TViewIncludeSpelling           { using Type = TViewType<T>; };
    template<typename T> struct TViewIncludeSpelling<Write<T>> { using Type = Write<T>; };

    template<typename T>
    using TViewInclude = typename TViewIncludeSpelling<T>::Type;

    // A pool stores objects, so a reference, a pointer, a cv-qualified spelling or an access wrapper is a call-site mistake.
    template<typename T>
    concept CComponent =
        std::is_object_v<T> &&
        !std::is_pointer_v<T> &&
        !std::is_const_v<T> &&
        !std::is_volatile_v<T> &&
        !std::is_array_v<T> &&
        !bIsQueryTerm<T>;

    // A view may spell an include const or wrap it in Read or Write, which says whether it marks the pool written.
    template<typename T>
    concept CViewComponent = CComponent<std::remove_const_t<TViewType<T>>>;

    template<typename T>
    using TBareComponent = std::remove_const_t<TViewType<T>>;

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

        // A component whose methods read other entities (a transform reads its parents), so a filter cannot prove two systems apart on it.
        static constexpr bool ReachesOtherEntities = false;
    };

    template<typename T>
    NODISCARD consteval bool ReadReachesOtherEntities()
    {
        if constexpr (requires { { T::ReachesOtherEntities } -> std::convertible_to<bool>; })
        {
            return T::ReachesOtherEntities;
        }
        else
        {
            return FComponentTraitDefaults::ReachesOtherEntities;
        }
    }

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

    template<typename T>
    struct TComponentTraits
    {
        static constexpr bool bEmpty = std::is_empty_v<T>;

        //~ What the component asked for, or the default when it asked for nothing.

        static constexpr bool InPlaceDelete = ReadInPlaceDelete<T>();
        static constexpr EComponentLayout Layout = ReadLayout<T>();
        static constexpr uint32 PageSize = ReadPageSize<T>();
        static constexpr bool bReachesOtherEntities = ReadReachesOtherEntities<T>();

        // Paged by default, so a held reference survives an emplace into the same pool mid-iteration.
        static constexpr bool bPaged = !bEmpty &&
            (InPlaceDelete
                || Layout == EComponentLayout::Paged
                || Layout == EComponentLayout::Automatic);

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
            return FName(FStringView(Name.data(), static_cast<int32>(Name.size())));
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
        Info.bTriviallyCopyable     = std::is_trivially_copyable_v<T>;

        if constexpr (!TComponentTraits<T>::bEmpty)
        {
            Info.Destruct = +[](const FComponentTypeInfo&, void* Dest) { static_cast<T*>(Dest)->~T(); };

            if constexpr (std::is_default_constructible_v<T>)
            {
                Info.DefaultConstruct = +[](const FComponentTypeInfo&, void* Dest) { Memory::ConstructAt(static_cast<T*>(Dest)); };
            }

            if constexpr (std::is_copy_constructible_v<T>)
            {
                Info.CopyConstruct = +[](const FComponentTypeInfo&, void* Dest, const void* Source) { Memory::ConstructAt(static_cast<T*>(Dest), *static_cast<const T*>(Source)); };
            }

            if constexpr (!(std::is_trivially_copyable_v<T> || TIsTriviallyRelocatable_V<T>)
                && std::is_move_constructible_v<T>)
            {
                Info.Relocate = +[](const FComponentTypeInfo&, void* Dest, void* Source)
                {
                    T* Moved = static_cast<T*>(Source);
                    Memory::ConstructAt(static_cast<T*>(Dest), Move(*Moved));
                    Memory::DestroyAt(Moved);
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
        // Constant-initialized, so unlike a lazy static this carries no thread-safe-init guard on every read.
        static std::atomic<FComponentTypeID> Cached{ InvalidComponentTypeID };

        FComponentTypeID TypeID = Cached.load(std::memory_order_acquire);
        if (TypeID == InvalidComponentTypeID) [[unlikely]]
        {
            // Acquire is idempotent per name under its lock, so two threads racing here resolve the same id.
            TypeID = FComponentTypeRegistry::Get().Acquire(MakeComponentTypeInfo<T>());
            Cached.store(TypeID, std::memory_order_release);
        }
        return TypeID;
    }
}
