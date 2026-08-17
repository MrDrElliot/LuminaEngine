#pragma once
#include "Class.h"

namespace Lumina
{
    // False for a CClass, which describes a CObject and derives from CStruct but is never value storage.
    RUNTIME_API bool IsInstancableStructType(const CStruct* Type);

    // A value type an FInstancedStruct may own: reflected, and NOT a CObject. A CObject has identity
    // and is referenced through TObjectPtr, never copied into an inline value slot.
    template<typename T>
    concept InstancableStruct = !eastl::is_base_of_v<CObject, T> && requires { T::StaticStruct(); };

    // Owns a heap instance of a reflected CStruct chosen at runtime.
    struct FInstancedStruct
    {
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

        CStruct* GetScriptStruct() const { return ScriptStruct; }
        void* GetMutableMemory() const { return InstanceMemory; }
        const void* GetMemory() const { return InstanceMemory; }
        bool IsValid() const { return ScriptStruct != nullptr && InstanceMemory != nullptr; }

        RUNTIME_API bool Identical(const FInstancedStruct& Other) const;

        // Typed access; returns null when the stored type isn't T or derived from T.
        template<InstancableStruct T>
        T* GetMutablePtr()
        {
            return (ScriptStruct && ScriptStruct->IsChildOf(T::StaticStruct())) ? static_cast<T*>(reinterpret_cast<void*>(InstanceMemory)) : nullptr;
        }

        template<InstancableStruct T>
        const T* GetPtr() const
        {
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

        CStruct* ScriptStruct = nullptr;
        uint8* InstanceMemory = nullptr;
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
