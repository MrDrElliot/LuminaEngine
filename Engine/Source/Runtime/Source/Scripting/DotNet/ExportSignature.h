#pragma once

#include "Platform/GenericPlatform.h"

// A width both sides derive from their own declaration, so a drifted signature fails at bind, not mid-call.

namespace Lumina::DotNet
{
    struct FExportSignatureEntry
    {
        const char* Name;
        int32       Size;
    };

    RUNTIME_API void  RegisterExportSignatures(const FExportSignatureEntry* Entries, int32 Count);
    RUNTIME_API int32 GetExportSignature(const char* Name, int32 Len);

    template <typename T>
    struct TArgWidth { static constexpr int32 Value = (int32)sizeof(T); };

    // A reference crosses as the address C# passes for it, so it is one pointer wide either way.
    template <typename T>
    struct TArgWidth<T&> { static constexpr int32 Value = (int32)sizeof(T*); };

    template <>
    struct TArgWidth<void> { static constexpr int32 Value = 0; };

    template <typename Ret, typename... Args>
    constexpr int32 SignatureSize(Ret (*)(Args...))
    {
        return TArgWidth<Ret>::Value + (TArgWidth<Args>::Value + ... + 0);
    }

    struct FExportSignatureRegistrar
    {
        template <int32 N>
        explicit FExportSignatureRegistrar(const FExportSignatureEntry (&Entries)[N])
        {
            RegisterExportSignatures(Entries, N);
        }
    };
}

#define LUMINA_DOTNET_SIG(Name) \
    { "LuminaSharp_" #Name, ::Lumina::DotNet::SignatureSize(&LuminaSharp_##Name) }

#define LE_SIG_CONCAT2(a, b) a##b
#define LE_SIG_CONCAT(a, b)  LE_SIG_CONCAT2(a, b)

// Named by counter because a unity build concatenates several of these tables into one translation unit.
#define LUMINA_DOTNET_SIGNATURES_IMPL(Id, ...)                                     \
    static const ::Lumina::DotNet::FExportSignatureEntry                           \
        LE_SIG_CONCAT(GExportSignatures_, Id)[] = { __VA_ARGS__ };                 \
    static const ::Lumina::DotNet::FExportSignatureRegistrar                       \
        LE_SIG_CONCAT(GExportSignatureReg_, Id)(LE_SIG_CONCAT(GExportSignatures_, Id))

// One table per file, listing the exports it defines so a drifted signature fails at bind.
#define LUMINA_DOTNET_SIGNATURES(...) LUMINA_DOTNET_SIGNATURES_IMPL(__COUNTER__, __VA_ARGS__)
