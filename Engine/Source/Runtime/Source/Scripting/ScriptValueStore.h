#pragma once

#include "Containers/Array.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "ScriptValueStore.generated.h"

namespace Lumina
{
    class CScriptStruct;
    class FArchive;

    // Per-instance value buffer for a C# script's [Property] fields, laid out by the script's minted CScriptStruct.
    REFLECT()
    struct RUNTIME_API SScriptValueStore
    {
        GENERATED_BODY()

        SScriptValueStore() = default;
        ~SScriptValueStore();
        SScriptValueStore(const SScriptValueStore& Other);
        SScriptValueStore& operator=(const SScriptValueStore& Other);
        SScriptValueStore(SScriptValueStore&& Other) noexcept;
        SScriptValueStore& operator=(SScriptValueStore&& Other) noexcept;

        bool Serialize(FArchive& Ar);

        // Point the store at a layout, materializing a live buffer and migrating in place on layout change.
        void EnsureLayout(const CScriptStruct* NewLayout);

        const CScriptStruct* GetLayout() const { return Layout.Get(); }
        void* GetBuffer() const { return Buffer; }

        void Reset();

        TObjectPtr<CScriptStruct> Layout;       // keeps the layout alive while a buffer uses it
        uint8*                    Buffer = nullptr;
        TVector<uint8>            Serialized;    // pre-materialize bytes, empty once live
    };
}
