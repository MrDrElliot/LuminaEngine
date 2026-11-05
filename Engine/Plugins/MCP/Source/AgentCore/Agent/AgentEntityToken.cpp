#include "AgentCorePCH.h"
#include "World/ECS/Registry.h"
#include "Agent/AgentEntityToken.h"

#include "Containers/StringFormat.h"
#include "Core/Threading/Atomic.h"

namespace Lumina::Agent
{
    namespace AgentEntityTokenInternal
    {
        // Bumped on a world swap so a token from the old one cannot resolve against the new.
        TAtomic<uint64> GEpoch { 1 };

        constexpr FStringView GPrefix = "ent";
        constexpr char GSeparator = '_';

        bool ParseHex(FStringView Text, uint64& OutValue)
        {
            if (Text.IsEmpty() || Text.Len() > 16)
            {
                return false;
            }

            uint64 Value = 0;
            for (char Character : Text)
            {
                uint64 Digit = 0;
                if (Character >= '0' && Character <= '9')      { Digit = static_cast<uint64>(Character - '0'); }
                else if (Character >= 'a' && Character <= 'f') { Digit = static_cast<uint64>(Character - 'a') + 10; }
                else { return false; }

                Value = (Value << 4) | Digit;
            }

            OutValue = Value;
            return true;
        }
    }

    using namespace AgentEntityTokenInternal;

    uint64 FEntityTokens::GetEpoch()
    {
        return GEpoch.load(std::memory_order_acquire);
    }

    void FEntityTokens::InvalidateAll()
    {
        GEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    FString FEntityTokens::Mint(const ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
        {
            return FString();
        }

        // The version rides along in the handle, which is what makes a recycled slot detectable.
        const uint64 Bits = static_cast<uint64>(static_cast<uint32>(Entity));

        return Lumina::Format("{}{}{:x}{}{:x}", GPrefix, GSeparator, GetEpoch(), GSeparator, Bits);
    }

    bool FEntityTokens::Resolve(const ECS::FRegistry& Registry, FStringView Token,
        ECS::FEntity& OutEntity, FString& OutError)
    {
        OutEntity = ECS::NullEntity;

        const auto Reject = [&OutError](FStringView Reason)
        {
            OutError = FString(Reason.GetData(), Reason.Len());
            return false;
        };

        if (Token.Len() < GPrefix.Len() + 4 || Token.Mid(0, GPrefix.Len()) != GPrefix)
        {
            return Reject("That is not an entity id.");
        }

        FStringView Rest = Token.Mid(GPrefix.Len());
        if (Rest.IsEmpty() || Rest[0] != GSeparator)
        {
            return Reject("That is not an entity id.");
        }

        Rest = Rest.Mid(1);

        const int32 Split = Rest.Find(GSeparator);
        if (Split == Constants::kIndexNone)
        {
            return Reject("That is not an entity id.");
        }

        uint64 Epoch = 0;
        uint64 Bits  = 0;

        if (!ParseHex(Rest.Mid(0, Split), Epoch) || !ParseHex(Rest.Mid(Split + 1), Bits))
        {
            return Reject("That is not an entity id.");
        }

        if (Epoch != GetEpoch())
        {
            return Reject("That entity id came from a world that is no longer open.");
        }

        if (Bits > 0xFFFFFFFFull)
        {
            return Reject("That is not an entity id.");
        }

        const ECS::FEntity Candidate = static_cast<ECS::FEntity>(static_cast<uint32>(Bits));

        if (Candidate == ECS::NullEntity || !Registry.IsValid(Candidate))
        {
            return Reject("That entity no longer exists.");
        }

        OutEntity = Candidate;
        return true;
    }
}
