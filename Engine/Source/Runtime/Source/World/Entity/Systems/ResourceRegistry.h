#pragma once

#include "ModuleAPI.h"
#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Threading/Sync.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CStruct;
}

namespace Lumina::ECS
{
    class FRegistry;

    struct FResourceType
    {
        const CStruct* Struct = nullptr;
        FName          Name;
        uint32         TypeId = 0;
        void*          (*GetOrEmplace)(FRegistry&) = nullptr;
        void*          (*Find)(FRegistry&) = nullptr;
    };

    // Access ids for names that own no type start here, above every 16-bit component, event and resource id.
    inline constexpr uint32 FirstNamedAccessToken = 1u << 16;

    class FResourceTypeRegistry
    {
    public:

        static RUNTIME_API FResourceTypeRegistry& Get();

        // Idempotent per name, so a reloaded module rebinds its entry in place and handed-out pointers stay valid.
        RUNTIME_API void Register(const FResourceType& Type);

        NODISCARD RUNTIME_API const FResourceType* Find(const CStruct* Struct) const;
        NODISCARD RUNTIME_API const FResourceType* Find(FName Name) const;

        NODISCARD RUNTIME_API uint32 GetNamedAccessToken(FName Name);

        // Zero when the name never acquired a token.
        NODISCARD RUNTIME_API uint32 FindNamedAccessToken(FName Name) const;

        // NAME_None when the id is not a named token.
        NODISCARD RUNTIME_API FName GetNamedAccessTokenName(uint32 Token) const;

    private:

        TVector<TUniquePtr<FResourceType>> Types;
        TVector<FName>                     TokenNames;
        mutable FMutex                     Lock;
    };
}
