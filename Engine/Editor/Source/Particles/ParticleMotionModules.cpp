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
            + (bCollideWithTerrain ? "true" : "false") + ")";
        Compiler.EmitUpdate("if (" + Hit + ")");
        Compiler.EmitUpdate("{");
        Compiler.EmitUpdate("\tRaiseParticleEvent(PARTICLE_EVENT_COLLISION, P);");
        if (bKillOnHit)
        {
            Compiler.EmitUpdate("\tP.Age = P.Lifetime;");
        }
        Compiler.EmitUpdate("}");
    }
}
