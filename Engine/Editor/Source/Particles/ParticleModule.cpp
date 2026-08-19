#include "ParticleModule.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    FString CParticleModule::LocalVar(int32 ModuleIndex, const char* Name)
    {
        return "m" + FString(Format("{}", ModuleIndex)) + "_" + Name;
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
