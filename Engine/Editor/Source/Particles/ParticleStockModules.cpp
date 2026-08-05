#include "ParticleStockModules.h"
#include "Containers/String.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"

namespace Lumina
{
    static FString ShapeId(EParticleEmitterShape Shape)
    {
        return FString(eastl::to_string((uint32)Shape)) + "u";
    }

    // Spawn modules

    void CParticleModule_SpawnLocation::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Off = LocalVar(ModuleIndex, "off");
        Compiler.EmitSpawn("float3 " + Off + " = SampleEmitterShape(Seed, " + ShapeId(Shape) + ", "
            + Compiler.Param("ShapeSize", ShapeSize) + ", radians(" + Compiler.Param("ConeAngle", ConeAngle) + "), "
            + "SimParams().EmitterForward.xyz, SimParams().EmitterRight.xyz, SimParams().EmitterUp.xyz);");
        Compiler.EmitSpawn("P.Position = SimParams().EmitterPosition.xyz + " + Off + ";");
    }

    void CParticleModule_InitialVelocity::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        switch (Mode)
        {
        case EParticleInitVelocityMode::Explicit:
        {
            const FString R = LocalVar(ModuleIndex, "r");
            Compiler.EmitSpawn("float3 " + R + " = float3(RandUnit(Seed), RandUnit(Seed), RandUnit(Seed));");
            Compiler.EmitSpawn("P.Velocity = lerp(" + Compiler.Param("VelocityMin", VelocityMin) + ", " + Compiler.Param("VelocityMax", VelocityMax) + ", " + R + ");");
            break;
        }
        case EParticleInitVelocityMode::Radial:
        {
            const FString D = LocalVar(ModuleIndex, "dir");
            const FString L = LocalVar(ModuleIndex, "len");
            Compiler.EmitSpawn("float3 " + D + " = P.Position - SimParams().EmitterPosition.xyz;");
            Compiler.EmitSpawn("float " + L + " = length(" + D + ");");
            Compiler.EmitSpawn(D + " = (" + L + " > 1e-5) ? (" + D + " / " + L + ") : RandOnUnitSphere(Seed);");
            Compiler.EmitSpawn("P.Velocity = " + D + " * lerp(" + Compiler.Param("SpeedMin", SpeedRange.x) + ", " + Compiler.Param("SpeedMax", SpeedRange.y) + ", RandUnit(Seed));");
            break;
        }
        case EParticleInitVelocityMode::Cone:
        {
            const FString D = LocalVar(ModuleIndex, "dir");
            Compiler.EmitSpawn("float3 " + D + " = RandDirectionInCone(Seed, SimParams().EmitterForward.xyz, "
                + "SimParams().EmitterRight.xyz, SimParams().EmitterUp.xyz, radians(" + Compiler.Param("ConeAngle", ConeAngle) + "));");
            Compiler.EmitSpawn("P.Velocity = " + D + " * lerp(" + Compiler.Param("SpeedMin", SpeedRange.x) + ", " + Compiler.Param("SpeedMax", SpeedRange.y) + ", RandUnit(Seed));");
            break;
        }
        }
    }

    void CParticleModule_InitialColor::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.EmitSpawn("P.Color = " + Compiler.Param("Color", Color) + ";");
    }

    void CParticleModule_InitialSize::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.EmitSpawn("P.Size = lerp(" + Compiler.Param("SizeMin", SizeRange.x) + ", " + Compiler.Param("SizeMax", SizeRange.y) + ", RandUnit(Seed));");
    }

    void CParticleModule_Lifetime::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.EmitSpawn("P.Lifetime = lerp(" + Compiler.Param("LifeMin", LifetimeRange.x) + ", " + Compiler.Param("LifeMax", LifetimeRange.y) + ", RandUnit(Seed));");
    }

    void CParticleModule_InitialRotation::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.EmitSpawn("P.Rotation = radians(lerp(" + Compiler.Param("RotMin", RotationRange.x) + ", " + Compiler.Param("RotMax", RotationRange.y) + ", RandUnit(Seed)));");
        Compiler.EmitSpawn("P.RotationSpeed = radians(lerp(" + Compiler.Param("RotSpeedMin", RotationSpeedRange.x) + ", " + Compiler.Param("RotSpeedMax", RotationSpeedRange.y) + ", RandUnit(Seed)));");
    }

    // Update modules

    void CParticleModule_GravityForce::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.EmitUpdate("P.Velocity += " + Compiler.Param("Gravity", Gravity) + " * DeltaTime;");
    }

    void CParticleModule_NonUniformSize::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        // Names matter: these are the strings in ParticleRenderAttribute::Names that the compiler resolves
        // to slots for the vertex shader. Declaring them under any other name would simulate fine and
        // render as an ordinary square sprite.
        Compiler.EmitSpawn(Compiler.Attribute("SizeScaleX", "1.0") + " = " + Compiler.Param("ScaleX", Scale.x) + ";");
        Compiler.EmitSpawn(Compiler.Attribute("SizeScaleY", "1.0") + " = " + Compiler.Param("ScaleY", Scale.y) + ";");
    }

    void CParticleModule_SetMass::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        // Declaring returns the addressed lvalue, so writing an attribute reads like writing a field.
        const FString Mass = Compiler.Attribute("Mass", "1.0");
        Compiler.EmitSpawn(Mass + " = lerp(" + Compiler.Param("MassMin", MassRange.x) + ", "
            + Compiler.Param("MassMax", MassRange.y) + ", RandUnit(Seed));");
    }

    void CParticleModule_Drag::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        if (bScaleByMass)
        {
            const FString Mass = Compiler.Attribute("Mass", "1.0");
            Compiler.EmitUpdate("P.Velocity *= exp(-" + Compiler.Param("Drag", Drag)
                + " / max(" + Mass + ", 1e-4) * DeltaTime);");
        }
        else
        {
            Compiler.EmitUpdate("P.Velocity *= exp(-" + Compiler.Param("Drag", Drag) + " * DeltaTime);");
        }
    }

    void CParticleModule_CurlNoiseForce::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString N = LocalVar(ModuleIndex, "turb");
        Compiler.EmitUpdate("float3 " + N + " = CurlishNoise(P.Position, TotalTime, " + Compiler.Param("NoiseScale", Scale) + ", " + Compiler.Param("NoiseSpeed", Speed) + ");");
        Compiler.EmitUpdate("P.Velocity += " + N + " * " + Compiler.Param("NoiseStrength", Strength) + " * DeltaTime;");
    }

    // Seeded so a freshly added module reproduces the old start/end default rather than dropping the
    // particle to an empty (white) ramp.
    CParticleModule_ColorOverLife::CParticleModule_ColorOverLife()
    {
        Gradient.AddKey(0.0f, FVector4(1.0f, 0.6f, 0.2f, 1.0f));
        Gradient.AddKey(1.0f, FVector4(1.0f, 0.0f, 0.0f, 0.0f));
    }

    void CParticleModule_ColorOverLife::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.EmitUpdate("P.Color = SampleGradientLUT(" + Compiler.ParamGradient("Gradient", Gradient) + ", LifeRatio);");
    }

    CParticleModule_SizeOverLife::CParticleModule_SizeOverLife()
    {
        // Multiplier, so 1 -> 0 reproduces the previous shrink-to-nothing default.
        Curve.Curve.AddKey(0.0f, 1.0f);
        Curve.Curve.AddKey(1.0f, 0.0f);
    }

    void CParticleModule_SizeOverLife::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        // Scales rather than replaces: the Spawn stack owns the base size, this owns the shape.
        Compiler.EmitUpdate("P.Size *= SampleCurveLUT(" + Compiler.ParamCurve("Curve", Curve) + ", LifeRatio);");
    }

    void CParticleModule_Trail::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        // Three scalars, not one vector: attributes are scalar-only, and these names are the ones
        // ParticleRenderAttribute::Names resolves to slots for the vertex shader.
        const FString PX = Compiler.Attribute("PrevPosX");
        const FString PY = Compiler.Attribute("PrevPosY");
        const FString PZ = Compiler.Attribute("PrevPosZ");

        // Seeded at the END of the spawn chunk -- the spawn stack is generated in full before the update
        // stack, so P.Position here is the one the location modules actually wrote. The attribute's
        // declared default cannot do this: defaults are emitted ahead of the spawn stack, where the
        // position is still whatever the recycled slot last held, and a newborn particle would draw a
        // streak stretching back to wherever the previous occupant of that slot died.
        Compiler.Emit(EParticleContext::Spawn, PX + " = P.Position.x;");
        Compiler.Emit(EParticleContext::Spawn, PY + " = P.Position.y;");
        Compiler.Emit(EParticleContext::Spawn, PZ + " = P.Position.z;");

        // An exponential chase rather than "wherever it was last frame": the tail then lags by a fixed
        // number of SECONDS whatever the frame rate, instead of a streak that shrinks as the frame rate
        // rises. It also tracks the position history, so on a curved path (gravity, curl noise) the streak
        // follows the arc -- which is the whole reason this exists next to velocity stretch, which can only
        // ever draw a straight line along the instantaneous heading.
        const FString Length = Compiler.Param("TrailTime", Math::Max(TrailLength, 0.0f));
        Compiler.EmitUpdate("{");
        Compiler.EmitUpdate("	const float TrailAlpha = saturate(DeltaTime / max(" + Length + ", 1e-4));");
        Compiler.EmitUpdate("	" + PX + " = lerp(" + PX + ", P.Position.x, TrailAlpha);");
        Compiler.EmitUpdate("	" + PY + " = lerp(" + PY + ", P.Position.y, TrailAlpha);");
        Compiler.EmitUpdate("	" + PZ + " = lerp(" + PZ + ", P.Position.z, TrailAlpha);");
        Compiler.EmitUpdate("}");
    }

    void CParticleModule_Integrate::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        Compiler.EmitUpdate("P.Position += P.Velocity * DeltaTime;");
        Compiler.EmitUpdate("P.Rotation += P.RotationSpeed * DeltaTime;");
    }
}
