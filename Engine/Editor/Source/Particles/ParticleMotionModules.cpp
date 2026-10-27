#include "ParticleMotionModules.h"
#include "Containers/String.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"

namespace Lumina
{
    // Unit curves by default, so a freshly added module applies its strength evenly until someone shapes it.
    static void SetFlatCurve(SCurve& Curve, float Value)
    {
        Curve.Curve.AddKey(0.0f, Value);
        Curve.Curve.AddKey(1.0f, Value);
    }

    static FString OverLifeExpr(FParticleCompiler& Compiler, const char* Name, const SCurve& Curve)
    {
        return "SampleCurveLUT(" + Compiler.ParamCurve(Name, Curve) + ", LifeRatio)";
    }

    void CParticleModule_SpeedLimit::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Limit = LocalVar(ModuleIndex, "limit");
        const FString Speed = LocalVar(ModuleIndex, "speed");
        Compiler.EmitUpdate("const float " + Limit + " = " + Compiler.Param("MaxSpeed", MaxSpeed) + " * EmitterScale();");
        Compiler.EmitUpdate("const float " + Speed + " = length(P.Velocity);");
        Compiler.EmitUpdate("if (" + Speed + " > " + Limit + ") { P.Velocity *= " + Limit + " / " + Speed + "; }");
    }

    CParticleModule_RadialAcceleration::CParticleModule_RadialAcceleration()
    {
        SetFlatCurve(OverLife, 1.0f);
    }

    void CParticleModule_RadialAcceleration::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Out = LocalVar(ModuleIndex, "out");
        Compiler.EmitUpdate("float3 " + Out + " = P.Position - SimParams().EmitterPosition.xyz;");
        Compiler.EmitUpdate(Out + " = dot(" + Out + ", " + Out + ") > 1e-10 ? normalize(" + Out + ") : float3(0.0, 0.0, 0.0);");
        Compiler.EmitUpdate("P.Velocity += " + Out + " * (" + Compiler.Param("Strength", Strength) + " * "
            + OverLifeExpr(Compiler, "OverLife", OverLife) + " * EmitterScale() * DeltaTime);");
    }

    CParticleModule_TangentialAcceleration::CParticleModule_TangentialAcceleration()
    {
        SetFlatCurve(OverLife, 1.0f);
    }

    void CParticleModule_TangentialAcceleration::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Around = LocalVar(ModuleIndex, "around");
        Compiler.EmitUpdate("float3 " + Around + " = cross(SimParams().EmitterUp.xyz, P.Position - SimParams().EmitterPosition.xyz);");
        Compiler.EmitUpdate(Around + " = dot(" + Around + ", " + Around + ") > 1e-10 ? normalize(" + Around + ") : float3(0.0, 0.0, 0.0);");
        Compiler.EmitUpdate("P.Velocity += " + Around + " * (" + Compiler.Param("Strength", Strength) + " * "
            + OverLifeExpr(Compiler, "OverLife", OverLife) + " * EmitterScale() * DeltaTime);");
    }

    CParticleModule_OrbitVelocity::CParticleModule_OrbitVelocity()
    {
        SetFlatCurve(OverLife, 1.0f);
    }

    void CParticleModule_OrbitVelocity::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Angle = LocalVar(ModuleIndex, "angle");
        const FString Arm   = LocalVar(ModuleIndex, "arm");
        const FString Axis  = LocalVar(ModuleIndex, "axis");
        Compiler.EmitUpdate("const float " + Angle + " = 6.2831853 * " + Compiler.Param("TurnsPerSecond", TurnsPerSecond) + " * "
            + OverLifeExpr(Compiler, "OverLife", OverLife) + " * DeltaTime;");
        Compiler.EmitUpdate("const float3 " + Axis + " = SimParams().EmitterUp.xyz;");
        Compiler.EmitUpdate("const float3 " + Arm + " = P.Position - SimParams().EmitterPosition.xyz;");
        Compiler.EmitUpdate("P.Position = SimParams().EmitterPosition.xyz + " + Arm + " * cos(" + Angle + ") + cross(" + Axis + ", " + Arm
            + ") * sin(" + Angle + ") + " + Axis + " * dot(" + Axis + ", " + Arm + ") * (1.0 - cos(" + Angle + "));");
    }

    CParticleModule_VelocityOverLife::CParticleModule_VelocityOverLife()
    {
        SetFlatCurve(OverLife, 1.0f);
    }

    void CParticleModule_VelocityOverLife::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.EmitUpdate("P.Position += EmitterToWorld(" + Compiler.Param("Velocity", Velocity) + ") * ("
            + OverLifeExpr(Compiler, "OverLife", OverLife) + " * EmitterScale() * DeltaTime);");
    }

    CParticleModule_SpinOverLife::CParticleModule_SpinOverLife()
    {
        SetFlatCurve(OverLife, 1.0f);
    }

    void CParticleModule_SpinOverLife::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.EmitUpdate("P.Rotation += radians(" + Compiler.Param("DegreesPerSecond", DegreesPerSecond) + " * "
            + OverLifeExpr(Compiler, "OverLife", OverLife) + ") * DeltaTime;");
    }

    CParticleModule_DampingOverLife::CParticleModule_DampingOverLife()
    {
        SetFlatCurve(OverLife, 1.0f);
    }

    void CParticleModule_DampingOverLife::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.EmitUpdate("P.Velocity *= exp(-" + Compiler.Param("Damping", Damping) + " * "
            + OverLifeExpr(Compiler, "OverLife", OverLife) + " * DeltaTime);");
    }

    CParticleModule_ScaleOverLife::CParticleModule_ScaleOverLife()
    {
        SetFlatCurve(Width, 1.0f);
        SetFlatCurve(Height, 1.0f);
    }

    void CParticleModule_ScaleOverLife::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        // These names are what ParticleRenderAttribute::Names resolves to slots for the vertex shader.
        Compiler.EmitUpdate(Compiler.Attribute("SizeScaleX", "1.0") + " = " + OverLifeExpr(Compiler, "Width", Width) + ";");
        Compiler.EmitUpdate(Compiler.Attribute("SizeScaleY", "1.0") + " = " + OverLifeExpr(Compiler, "Height", Height) + ";");
    }

    void CParticleModule_HueVariation::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.EmitSpawn("P.Color.rgb = RotateHue(P.Color.rgb, RandSigned(Seed) * 6.2831853 * " + Compiler.Param("Variation", Variation) + ");");
    }

    void CParticleModule_AttractorForce::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.EmitUpdate("ApplyAttractors(P.Position, P.Velocity, DeltaTime, " + Compiler.Param("Scale", Scale) + ");");
    }

    void CParticleModule_ShapeCollision::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Hit = "CollideWithShapes(P.Position, P.Velocity, " + Compiler.Param("Restitution", Restitution) + ", "
            + Compiler.Param("Friction", Friction) + ", " + Compiler.Param("Radius", Radius) + " * EmitterScale(), "
            + (bCollideWithTerrain ? "true" : "false") + ", HitNormal)";
        Compiler.EmitUpdate("{");
        Compiler.EmitUpdate("\tconst float3 ImpactVelocity = P.Velocity;");
        Compiler.EmitUpdate("\tfloat3 HitNormal;");
        Compiler.EmitUpdate("\tif (" + Hit + ")");
        Compiler.EmitUpdate("\t{");
        Compiler.EmitUpdate("\t\tRaiseParticleCollision(P, HitNormal, max(-dot(ImpactVelocity, HitNormal), 0.0));");
        if (bKillOnHit)
        {
            Compiler.EmitUpdate("\t\tP.Age = P.Lifetime;");
        }
        Compiler.EmitUpdate("\t}");
        Compiler.EmitUpdate("}");
    }

    void CParticleModule_ParticleSeparation::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.UseNeighborGrid();
        const FString Sums = LocalVar(ModuleIndex, "sums");
        Compiler.EmitUpdate("const FNeighborSums " + Sums + " = GatherNeighbors(Index, P.Position, " + Compiler.Param("Radius", Radius) + ");");
        Compiler.EmitUpdate("P.Velocity += " + Sums + ".Away * " + Compiler.Param("Strength", Strength) + " * DeltaTime;");
    }

    void CParticleModule_Boids::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.UseNeighborGrid();
        const FString Sums  = LocalVar(ModuleIndex, "sums");
        const FString Speed = LocalVar(ModuleIndex, "speed");
        const FString Limit = LocalVar(ModuleIndex, "limit");
        Compiler.EmitUpdate("const FNeighborSums " + Sums + " = GatherNeighbors(Index, P.Position, " + Compiler.Param("Radius", Radius) + ");");
        Compiler.EmitUpdate("P.Velocity += " + Sums + ".Away * " + Compiler.Param("Separation", Separation) + " * DeltaTime;");
        Compiler.EmitUpdate("if (" + Sums + ".Count > 0.0)");
        Compiler.EmitUpdate("{");
        Compiler.EmitUpdate("\tP.Velocity += (" + Sums + ".Velocity / " + Sums + ".Count - P.Velocity) * saturate(" + Compiler.Param("Alignment", Alignment) + " * DeltaTime);");
        Compiler.EmitUpdate("\tP.Velocity += (" + Sums + ".Position / " + Sums + ".Count - P.Position) * " + Compiler.Param("Cohesion", Cohesion) + " * DeltaTime;");
        Compiler.EmitUpdate("}");
        Compiler.EmitUpdate("const float " + Speed + " = length(P.Velocity);");
        Compiler.EmitUpdate("const float " + Limit + " = " + Compiler.Param("MaxSpeed", MaxSpeed) + ";");
        Compiler.EmitUpdate("if (" + Speed + " > " + Limit + ") { P.Velocity *= " + Limit + " / " + Speed + "; }");
    }

    void CParticleModule_PointForce::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString ToPoint = LocalVar(ModuleIndex, "topoint");
        const FString Dist = LocalVar(ModuleIndex, "dist");
        const FString Reach = LocalVar(ModuleIndex, "reach");
        const FString Fade = LocalVar(ModuleIndex, "fade");
        const FString Mass = bScaleByMass ? " / max(" + Compiler.Attribute("Mass", "1.0") + ", 1e-4)" : FString();

        Compiler.EmitUpdate("const float3 " + ToPoint + " = SimParams().EmitterPosition.xyz + EmitterToWorld(" + Compiler.Param("Offset", Offset)
            + ") * EmitterScale() - P.Position;");
        Compiler.EmitUpdate("const float " + Dist + " = length(" + ToPoint + ");");
        Compiler.EmitUpdate("const float " + Reach + " = " + Compiler.Param("Radius", Radius) + " * EmitterScale();");
        Compiler.EmitUpdate("const float " + Fade + " = " + Reach + " > 0.0 ? pow(saturate(1.0 - " + Dist + " / " + Reach + "), "
            + Compiler.Param("Falloff", Falloff) + ") : 1.0;");
        Compiler.EmitUpdate("P.Velocity += " + ToPoint + " / max(" + Dist + ", 1e-4) * " + Compiler.Param("Strength", Strength) + " * EmitterScale() * "
            + Fade + Mass + " * DeltaTime;");
        Compiler.EmitUpdate("if (" + Dist + " < " + Compiler.Param("KillRadius", KillRadius) + " * EmitterScale()) { P.Age = P.Lifetime; }");
    }

    void CParticleModule_WindForce::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Dir = LocalVar(ModuleIndex, "winddir");
        const FString Gust = LocalVar(ModuleIndex, "gust");
        const FString Pull = LocalVar(ModuleIndex, "pull");
        const FString Mass = bScaleByMass ? " / max(" + Compiler.Attribute("Mass", "1.0") + ", 1e-4)" : FString();
        const FString Direction3 = Compiler.Param("Direction", Direction);

        Compiler.EmitUpdate("const float3 " + Dir + " = " + Direction3 + " / max(length(" + Direction3 + "), 1e-4);");

        // The field drifts downwind, so a gust is seen to travel through the effect rather than flicker in place.
        Compiler.EmitUpdate("const float " + Gust + " = 1.0 + " + Compiler.Param("Gustiness", Gustiness) + " * ValueNoise(P.Position * "
            + Compiler.Param("GustScale", GustScale) + " - " + Dir + " * TotalTime * " + Compiler.Param("GustFrequency", GustFrequency) + ");");
        Compiler.EmitUpdate("const float " + Pull + " = 1.0 - exp(-" + Compiler.Param("Resistance", Resistance) + Mass + " * DeltaTime);");
        Compiler.EmitUpdate("P.Velocity += (" + Dir + " * " + Compiler.Param("Speed", Speed) + " * EmitterScale() * " + Gust + " - P.Velocity) * " + Pull + ";");
    }

    void CParticleModule_PlaneCollision::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString NormalParam = Compiler.Param("Normal", Normal);
        Compiler.EmitUpdate("{");
        Compiler.EmitUpdate("\tconst float3 PlaneNormal = " + NormalParam + " / max(length(" + NormalParam + "), 1e-4);");
        Compiler.EmitUpdate("\tconst float3 PlaneOrigin = SimParams().EmitterPosition.xyz + PlaneNormal * " + Compiler.Param("Height", Height) + " * EmitterScale();");
        Compiler.EmitUpdate("\tconst float Gap = dot(P.Position - PlaneOrigin, PlaneNormal) - " + Compiler.Param("Radius", Radius) + " * EmitterScale();");
        Compiler.EmitUpdate("\tif (Gap < 0.0)");
        Compiler.EmitUpdate("\t{");
        Compiler.EmitUpdate("\t\tP.Position -= PlaneNormal * Gap;");
        Compiler.EmitUpdate("\t\tconst float IntoPlane = -dot(P.Velocity, PlaneNormal);");
        Compiler.EmitUpdate("\t\tif (IntoPlane > 0.0)");
        Compiler.EmitUpdate("\t\t{");
        Compiler.EmitUpdate("\t\t\tReflectOffSurface(P.Velocity, PlaneNormal, " + Compiler.Param("Restitution", Restitution) + ", " + Compiler.Param("Friction", Friction) + ");");
        Compiler.EmitUpdate("\t\t\tRaiseParticleCollision(P, PlaneNormal, IntoPlane);");
        if (bKillOnHit)
        {
            Compiler.EmitUpdate("\t\t\tP.Age = P.Lifetime;");
        }
        Compiler.EmitUpdate("\t\t}");
        Compiler.EmitUpdate("\t}");
        Compiler.EmitUpdate("}");
    }
}
