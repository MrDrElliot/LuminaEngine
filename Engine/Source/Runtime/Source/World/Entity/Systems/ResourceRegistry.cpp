#include "RuntimePCH.h"
#include "ResourceRegistry.h"

#include "Core/Object/Class.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/ExportSignature.h"
#include "World/World.h"

namespace Lumina::ECS
{
    FResourceTypeRegistry& FResourceTypeRegistry::Get()
    {
        static FResourceTypeRegistry Instance;
        return Instance;
    }

    void FResourceTypeRegistry::Register(const FResourceType& Type)
    {
        if (Type.Struct == nullptr)
        {
            return;
        }

        FResourceType Named = Type;
        Named.Name = Type.Struct->GetName();

        FScopeLock Guard(Lock);
        for (TUniquePtr<FResourceType>& Existing : Types)
        {
            if (Existing->Name == Named.Name)
            {
                *Existing = Named;
                return;
            }
        }
        Types.Add(MakeUnique<FResourceType>(Named));
    }

    const FResourceType* FResourceTypeRegistry::Find(const CStruct* Struct) const
    {
        if (Struct == nullptr)
        {
            return nullptr;
        }

        FScopeLock Guard(Lock);
        for (const TUniquePtr<FResourceType>& Existing : Types)
        {
            if (Existing->Struct == Struct)
            {
                return Existing.Get();
            }
        }
        return nullptr;
    }

    const FResourceType* FResourceTypeRegistry::Find(FName Name) const
    {
        FScopeLock Guard(Lock);
        for (const TUniquePtr<FResourceType>& Existing : Types)
        {
            if (Existing->Name == Name)
            {
                return Existing.Get();
            }
        }
        return nullptr;
    }

    uint32 FResourceTypeRegistry::GetNamedAccessToken(FName Name)
    {
        FScopeLock Guard(Lock);
        int32 Index = 0;
        if (TokenNames.Find(Name, Index))
        {
            return FirstNamedAccessToken + static_cast<uint32>(Index);
        }
        TokenNames.Add(Name);
        return FirstNamedAccessToken + static_cast<uint32>(TokenNames.Num() - 1);
    }

    uint32 FResourceTypeRegistry::FindNamedAccessToken(FName Name) const
    {
        FScopeLock Guard(Lock);
        int32 Index = 0;
        return TokenNames.Find(Name, Index) ? FirstNamedAccessToken + static_cast<uint32>(Index) : 0;
    }

    FName FResourceTypeRegistry::GetNamedAccessTokenName(uint32 Token) const
    {
        FScopeLock Guard(Lock);
        const uint32 Index = Token - FirstNamedAccessToken;
        return Token >= FirstNamedAccessToken && Index < static_cast<uint32>(TokenNames.Num()) ? TokenNames[static_cast<int32>(Index)] : NAME_None;
    }
}

LUMINA_DOTNET_EXPORT(const void*, FindResourceType)(const char* Name, int Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return nullptr;
    }
    return Lumina::ECS::FResourceTypeRegistry::Get().Find(Lumina::FName(Lumina::FStringView(Name, static_cast<size_t>(Len))));
}

LUMINA_DOTNET_EXPORT(void*, GetOrEmplaceResource)(uint64 World, const void* Type)
{
    Lumina::CWorld* Bound = reinterpret_cast<Lumina::CWorld*>(World);
    const auto* Resource = static_cast<const Lumina::ECS::FResourceType*>(Type);
    return (Bound != nullptr && Resource != nullptr) ? Resource->GetOrEmplace(Bound->GetEntityRegistry()) : nullptr;
}

// Looked up on every read like a C++ TRes, since the world may replace its context after a system binds.
LUMINA_DOTNET_EXPORT(void*, FindResource)(uint64 World, const void* Type)
{
    Lumina::CWorld* Bound = reinterpret_cast<Lumina::CWorld*>(World);
    const auto* Resource = static_cast<const Lumina::ECS::FResourceType*>(Type);
    return (Bound != nullptr && Resource != nullptr) ? Resource->Find(Bound->GetEntityRegistry()) : nullptr;
}

LUMINA_DOTNET_EXPORT(Lumina::uint32, GetNamedAccessToken)(const char* Name, int Len)
{
    if (Name == nullptr || Len <= 0)
    {
        return 0;
    }
    return Lumina::ECS::FResourceTypeRegistry::Get().GetNamedAccessToken(Lumina::FName(Lumina::FStringView(Name, static_cast<size_t>(Len))));
}

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(FindResourceType),
    LUMINA_DOTNET_SIG(GetOrEmplaceResource),
    LUMINA_DOTNET_SIG(FindResource),
    LUMINA_DOTNET_SIG(GetNamedAccessToken)
);
