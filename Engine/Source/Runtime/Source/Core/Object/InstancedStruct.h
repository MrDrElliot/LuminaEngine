#pragma once
#include "Class.h"

namespace Lumina
{
    // False for a CClass, which describes a CObject and derives from CStruct but is never value storage.
    RUNTIME_API bool IsInstancableStructType(const CStruct* Type);

    // A value type an FInstancedStruct may own: reflected, and NOT a CObject. A CObject has identity
    // and is referenced through TObjectPtr, never copied into an inline value slot.
#if defined(REFLECTION_PARSER)
    // GENERATED_BODY is stubbed while parsing, so requiring StaticStruct() would reject every struct.
    template<typename T>
    concept InstancableStruct = true;
#else
    template<typename T>
    concept InstancableStruct = !eastl::is_base_of_v<CObject, T> && requires { T::StaticStruct(); };
#endif

    // Owns a heap instance of a reflected CStruct chosen at runtime.
    struct FInstancedStruct
    {
        // An instance this small lives inline, so a small block costs no allocation and no extra chase.
        static constexpr SIZE_T kInlineSize  = 32;
        static constexpr SIZE_T kInlineAlign = 16;

        FInstancedStruct() = default;
        explicit FInstancedStruct(CStruct* InStruct) { InitializeAs(InStruct); }

        RUNTIME_API ~FInstancedStruct();
        RUNTIME_API FInstancedStruct(const FInstancedStruct& Other);
        RUNTIME_API FInstancedStruct(FInstancedStruct&& Other) noexcept;
        RUNTIME_API FInstancedStruct& operator=(const FInstancedStruct& Other);
        RUNTIME_API FInstancedStruct& operator=(FInstancedStruct&& Other) noexcept;

        // Allocates and default-constructs an instance of InStruct; passing null clears.
        RUNTIME_API void InitializeAs(CStruct* InStruct);

        // Destroys the instance and frees its memory.
        RUNTIME_API void Reset();

        CStruct* GetScriptStruct() const { EnsureCurrentType(); return ScriptStruct; }
        void* GetMutableMemory() { EnsureCurrentType(); return InstanceMemory; }
        const void* GetMemory() const { EnsureCurrentType(); return InstanceMemory; }
        bool IsValid() const { EnsureCurrentType(); return ScriptStruct != nullptr && InstanceMemory != nullptr; }

        // Re-mints and migrates by field name when a script reload replaced the stored type.
        RUNTIME_API void EnsureCurrentType() const;

        RUNTIME_API bool Identical(const FInstancedStruct& Other) const;

        // Typed access; returns null when the stored type isn't T or derived from T.
        template<InstancableStruct T>
        T* GetMutablePtr()
        {
            EnsureCurrentType();
            return (ScriptStruct && ScriptStruct->IsChildOf(T::StaticStruct())) ? static_cast<T*>(reinterpret_cast<void*>(InstanceMemory)) : nullptr;
        }

        template<InstancableStruct T>
        const T* GetPtr() const
        {
            EnsureCurrentType();
            return (ScriptStruct && ScriptStruct->IsChildOf(T::StaticStruct())) ? static_cast<const T*>(reinterpret_cast<const void*>(InstanceMemory)) : nullptr;
        }

        // Replaces the value with a fresh default-constructed T and returns it.
        template<InstancableStruct T>
        T& InitializeAs()
        {
            InitializeAs(T::StaticStruct());
            return *static_cast<T*>(reinterpret_cast<void*>(InstanceMemory));
        }

    private:

        void CopyFrom(const FInstancedStruct& Other);

        // Brings up storage for Type without constructing it; sets bInline and InstanceMemory.
        void AllocateFor(const CStruct* Type) const;

        // Destroys through Type when non-null, then releases the bytes iff they were heap.
        static void ReleaseStorage(CStruct* Type, uint8* Memory, bool bWasInline);

        // Mutable so a const read can re-mint a type a script reload replaced underneath it.
        mutable CStruct* ScriptStruct = nullptr;
        mutable uint8* InstanceMemory = nullptr;

        // Stable identity of the stored type, so it survives the re-mint its pointer does not.
        FName TypeIdentity;
        mutable uint64 SeededGeneration = 0;

        mutable bool bInline = false;
        alignas(kInlineAlign) mutable uint8 InlineStorage[kInlineSize] = {};
    };

    // An FInstancedStruct constrained to T or a struct derived from T.
    template<InstancableStruct T>
    struct TInstancedStruct : public FInstancedStruct
    {
        TInstancedStruct() = default;
        TInstancedStruct(const FInstancedStruct& Other) : FInstancedStruct(Other) {}

        T* GetMutable() { return FInstancedStruct::GetMutablePtr<T>(); }
        const T* Get() const { return FInstancedStruct::GetPtr<T>(); }

        // The compile-time base struct every assignable value must derive from.
        static CStruct* StaticBaseStruct() { return T::StaticStruct(); }
    };
}
