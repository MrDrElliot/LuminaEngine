#include "EditorPCH.h"
#include "Agent/AgentEntityToken.h"

#include "Containers/StringFormat.h"
#include "Core/Threading/Atomic.h"

namespace Lumina::Agent
{
    namespace
    {
        // Bumped on a world swap so a token from the old one cannot resolve against the new.
        TAtomic<uint64> GEpoch { 1 };

        constexpr FStringView GPrefix = "ent";
        constexpr char GSeparator = '_';

        bool ParseHex(FStringView Text, uint64& OutValue)
        {
            if (Text.empty() || Text.size() > 16)
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

    uint64 FEntityTokens::GetEpoch()
    {
        return GEpoch.load(std::memory_order_acquire);
    }

    void FEntityTokens::InvalidateAll()
    {
        GEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    FString FEntityTokens::Mint(const FEntityRegistry& Registry, FEntity Entity)
    {
        if (Entity == entt::null || !Registry.valid(Entity))
        {
            return FString();
        }

        // The version rides along in the handle, which is what makes a recycled slot detectable.
        const uint64 Bits = static_cast<uint64>(static_cast<uint32>(Entity));

        return Lumina::Format("{}{}{:x}{}{:x}", GPrefix, GSeparator, GetEpoch(), GSeparator, Bits);
    }

    bool FEntityTokens::Resolve(const FEntityRegistry& Registry, FStringView Token,
        FEntity& OutEntity, FString& OutError)
    {
        OutEntity = entt::null;

        const auto Reject = [&OutError](FStringView Reason)
        {
            OutError = FString(Reason.data(), Reason.size());
            return false;
        };

        if (Token.size() < GPrefix.size() + 4 || Token.substr(0, GPrefix.size()) != GPrefix)
        {
            return Reject("That is not an entity id.");
        }

        FStringView Rest = Token.substr(GPrefix.size());
        if (Rest.empty() || Rest[0] != GSeparator)
        {
            return Reject("That is not an entity id.");
        }

        Rest = Rest.substr(1);

        const size_t Split = Rest.find(GSeparator);
        if (Split == FStringView::npos)
        {
            return Reject("That is not an entity id.");
        }

        uint64 Epoch = 0;
        uint64 Bits  = 0;

        if (!ParseHex(Rest.substr(0, Split), Epoch) || !ParseHex(Rest.substr(Split + 1), Bits))
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

        const FEntity Candidate = static_cast<FEntity>(static_cast<uint32>(Bits));

        if (Candidate == entt::null || !Registry.valid(Candidate))
        {
            return Reject("That entity no longer exists.");
        }

        OutEntity = Candidate;
        return true;
    }
}
