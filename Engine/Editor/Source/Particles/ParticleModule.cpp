#include "ParticleModule.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include <cctype>

namespace Lumina
{
    FString CParticleModule::LocalVar(int32 ModuleIndex, const char* Name)
    {
        return "m" + FString(Format("{}", ModuleIndex)) + "_" + Name;
    }

    bool CParticleModule::IsIdentifier(const FString& Name)
    {
        if (Name.empty() || std::isdigit((unsigned char)Name[0]))
        {
            return false;
        }

        for (char Character : Name)
        {
            if (!std::isalnum((unsigned char)Character) && Character != '_')
            {
                return false;
            }
        }
        return true;
    }

    FString CParticleModule::Lit(float V)
    {
        return FString(Format("{}", V));
    }

    FString CParticleModule::Lit(const FVector2& V)
    {
        return "float2(" + Lit(V.x) + ", " + Lit(V.y) + ")";
    }

    FString CParticleModule::Lit(const FVector3& V)
    {
        return "float3(" + Lit(V.x) + ", " + Lit(V.y) + ", " + Lit(V.z) + ")";
    }

    FString CParticleModule::Lit(const FVector4& V)
    {
        return "float4(" + Lit(V.x) + ", " + Lit(V.y) + ", " + Lit(V.z) + ", " + Lit(V.w) + ")";
    }
}
