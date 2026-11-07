#include "AgentCorePCH.h"
#include "World/ECS/Registry.h"
#include "Agent/AgentEntityToken.h"

#include "Containers/StringFormat.h"
#include "Core/Threading/Atomic.h"

namespace Lumina::Agent
{
    namespace AgentEntityTokenInternal
    {
        // Never reused, so a registry allocated where a closed one lived still gets a scope of its own.
        TAtomic<uint64> GNextScope { 1 };

        struct FTokenScope
        {
            uint64 Serial = GNextScope.fetch_add(1, std::memory_order_relaxed);
        };

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

    FString FEntityTokens::Mint(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
        {
            return FString();
        }

        const uint64 Serial = Registry.Ctx().GetOrEmplace<FTokenScope>().Serial;

        // The version rides along in the handle, which is what makes a recycled slot detectable.
        const uint64 Bits = static_cast<uint64>(static_cast<uint32>(Entity));

        return Lumina::Format("{}{}{:x}{}{:x}", GPrefix, GSeparator, Serial, GSeparator, Bits);
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

        uint64 Serial = 0;
        uint64 Bits   = 0;

        if (!ParseHex(Rest.Mid(0, Split), Serial) || !ParseHex(Rest.Mid(Split + 1), Bits))
        {
            return Reject("That is not an entity id.");
        }

        const FTokenScope* Scope = Registry.Ctx().Find<FTokenScope>();
        if (Scope == nullptr || Scope->Serial != Serial)
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
