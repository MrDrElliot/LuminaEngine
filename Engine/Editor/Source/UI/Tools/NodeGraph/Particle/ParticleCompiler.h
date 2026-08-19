#pragma once

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Assets/AssetTypes/Curve/Gradient.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "ParticlePin.h"

namespace Lumina
{
    class CParticleGraphNode;

    /** Samples per baked curve/gradient LUT. Must match PARTICLE_LUT_SAMPLES in ParticleSimCommon.slang:
     *  the shader indexes the table with this stride and would read into the next parameter otherwise. */
    inline constexpr int32 kParticleLUTSamples = 16;

    /** Which function body a node is being emitted into; drives demand-driven emission into the right scope. */
    enum class EParticleContext : uint8
    {
        Spawn,
        Update,
    };

    /** Resolved input pin value; Value is an HLSL expression (literal default or upstream variable name). */
    struct FParticleInputValue
    {
        FString             Value;
        EParticlePinType    Type = EParticlePinType::Float;
    };

    /** Graph-to-HLSL compiler: demand-first walk from the output, each node emitted once per context.
     *  SpawnChunks/UpdateChunks are spliced into ParticleSimulateTemplate.slang.
     *
     *  Exported alongside CParticleModule: Param/Attribute/Emit* are the entire authoring surface a custom
     *  module calls from Generate(), and most of them are out-of-line. */
    class EDITOR_API FParticleCompiler
    {
    public:

        FParticleCompiler() = default;

        FString BuildShader() const;

        //~ Module parameter block ------------------------------------------------------------------
        //
        // Module inputs used to be baked into the shader text as literals, which made every value edit a
        // full Slang recompile -- and a recompile swaps the FShaderEntry the dispatch binds, which is what
        // restarted the preview. It also meant nothing was parameterizable at runtime, because the values
        // only existed inside compiled SPIR-V.
        //
        // Instead a module registers each value input here and gets back an HLSL expression indexing a
        // float4 array uploaded per frame. The generated code then depends only on the module ORDER and
        // COUNT, never on the values -- so a value edit is a buffer write, and only a structural change
        // needs the shader rebuilt.
        //
        // One float4 per parameter, read back through a swizzle. That wastes up to 12 bytes on a scalar,
        // which at a realistic stack size is well under a kilobyte; packing sub-float4 would make slot
        // assignment order-dependent for no meaningful saving.

        /** Registers a module input and returns the HLSL expression that reads it.
         *
         *  The overload every stock module uses. A bound input still writes its authored constant into the
         *  slot and additionally records a binding, which the runtime uses to overwrite that slot from the
         *  component's parameters each frame. Binding therefore leaves the emitted code byte-identical, so
         *  it takes the value-only refresh path rather than a Slang rebuild. */
        FString Param(const char* DebugName, const SParticleParam& Value);

        /** Registers a value and returns the HLSL expression that reads it. DebugName is for diagnostics
         *  only -- slots are positional, so the same stack always produces the same layout. */
        FString Param(const char* DebugName, float Value);
        FString Param(const char* DebugName, const FVector2& Value);
        FString Param(const char* DebugName, const FVector3& Value);
        FString Param(const char* DebugName, const FVector4& Value);

        /** Bakes a curve/gradient into a fixed-resolution lookup table in consecutive slots and returns the
         *  BASE SLOT index, which the caller passes to SampleCurveLUT / SampleGradientLUT along with the
         *  time to sample at (the time varies per use site, so it cannot be folded in here).
         *
         *  A LUT rather than evaluating the keys on the GPU: the sample count is fixed, so reshaping a
         *  curve only rewrites slot values and still takes the no-recompile path. Evaluating keys directly
         *  would put the key COUNT in the generated code and make every key add a shader rebuild.
         *
         *  The returned index is a literal in the emitted code, so inserting a module upstream shifts it,
         *  changes the code hash, and correctly forces a rebuild. */
        FString ParamCurve(const char* DebugName, const SCurve& Value);
        FString ParamGradient(const char* DebugName, const SGradient& Value);

        //~ Per-particle attributes -----------------------------------------------------------------
        //
        // The core FGPUParticle is a fixed 64 bytes and exactly full, so a module that needs to carry its
        // own per-particle value (mass, a custom fade, a drag coefficient) had nowhere to put it. Declared
        // attributes live in a PARALLEL buffer indexed by the same particle index, which keeps the core
        // layout -- and therefore ParticleVertex.slang, which indexes it with a compile-time struct --
        // untouched.
        //
        // Scalars only for now. A float3 is three declarations; making vectors first-class needs a
        // non-lvalue read path (float3(B[i],B[i+1],B[i+2])) and a matching write helper, which is a
        // worthwhile follow-up but not needed to prove the mechanism.

        /** Declares (or re-finds) a scalar attribute and returns the addressed expression, which is an
         *  LVALUE -- assign to it to write, read it to sample:
         *
         *      const FString Mass = Compiler.Attribute("Mass", "1.0");
         *      Compiler.EmitSpawn(Mass + " = 2.0;");
         *      Compiler.EmitUpdate("P.Velocity /= max(" + Mass + ", 1e-4);");
         *
         *  Declaration is idempotent by name, so a spawn module and an update module that both name
         *  "Mass" share one slot -- that is how a value is handed between stages.
         *
         *  DefaultExpr seeds the attribute on spawn. Without it a module reading an attribute nobody wrote
         *  would silently get zero, which for something like mass is a divide-by-almost-zero rather than an
         *  obvious failure. */
        FString Attribute(const char* Name, const char* DefaultExpr = "0.0");

        /** Floats per particle in the attribute buffer. Always at least 1: an empty buffer would make the
         *  generated struct and the allocation degenerate for no benefit. */
        uint32 GetAttributeFloatCount() const;

        /** Slot of a declared attribute by name, or -1. Lets the renderer bind attributes it knows by
         *  name without the vertex shader needing the generated layout. */
        int32 FindAttributeSlot(const char* Name) const;

        /** Packed slot values in layout order; uploaded verbatim as the shader's ModuleParams array. */
        const TVector<FVector4>& GetParamValues() const { return ParamValues; }

        /** Slots a named user parameter drives, in the order they were registered. Stored on the emitter
         *  next to the values and read by the runtime; must be copied wherever GetParamValues() is, or a
         *  binding added in the editor would sit in the asset doing nothing. */
        const TVector<SParticleParamBinding>& GetParamBindings() const { return ParamBindings; }

        /** Hash of the emitted HLSL. Values no longer appear in the generated code, so editing one leaves
         *  this identical and the value-only path is safe; anything that changes the code -- reordering,
         *  toggling, or an input that selects a different branch (an emitter-shape enum, a velocity mode)
         *  -- changes it and forces a real rebuild. Hashing the slot layout alone would miss exactly those
         *  enum cases and silently keep running the stale shader. */
        uint64 GetGeneratedCodeHash() const;

        void EmitSpawn(const FString& Line)  { SpawnChunks  += "\t" + Line + "\n"; }
        void EmitUpdate(const FString& Line) { UpdateChunks += "\t" + Line + "\n"; }

        /** Emits into the given context regardless of the current context. */
        void Emit(EParticleContext Context, const FString& Line)
        {
            if (Context == EParticleContext::Spawn)
            {
                SpawnChunks += "\t" + Line + "\n";
            }
            else
            {
                UpdateChunks += "\t" + Line + "\n";
            }
        }

        /** Emits into the currently active context. */
        void EmitCurrent(const FString& Line) { Emit(CurrentContext, Line); }

        /** Resolves an input pin to an HLSL expression: connected emits the upstream node once and returns
         *  its full name; unconnected returns the pin default literal. */
        FParticleInputValue GetInputValue(CParticleInput* Pin);

        FParticleInputValue GetInputFloat(CParticleInput* Pin,  float Default = 0.0f);
        FParticleInputValue GetInputFloat3(CParticleInput* Pin, const FVector3& Default = FVector3(0.0f));
        FParticleInputValue GetInputFloat4(CParticleInput* Pin, const FVector4& Default = FVector4(1.0f));

        /** Scalar / float3 / float4 coercion helpers. Used by nodes that need a specific type. */
        static FString Coerce(const FParticleInputValue& Value, EParticlePinType Target);
        static FString TypeName(EParticlePinType Type);

        /** Ensures a node is emitted in the given context, running GenerateDefinition on first call. Idempotent. */
        void EnsureEmitted(CParticleGraphNode* Node, EParticleContext Context);

        void SetContext(EParticleContext Context) { CurrentContext = Context; }
        EParticleContext GetContext() const       { return CurrentContext; }

        bool HasErrors() const                                          { return !Errors.empty(); }
        void AddError(const EdNodeGraph::FError& Error)                 { Errors.push_back(Error); }
        const TVector<EdNodeGraph::FError>& GetErrors() const           { return Errors; }

    private:

        /** Appends a slot, folds its width into the layout hash, and returns "MP(i)" + swizzle. */
        FString AddParamSlot(const char* DebugName, const FVector4& Value, uint32 Components);

        FString SpawnChunks;
        FString UpdateChunks;

        struct FAttributeDecl
        {
            FString Name;
            FString DefaultExpr;
        };

        TVector<FVector4>              ParamValues;
        TVector<SParticleParamBinding> ParamBindings;
        TVector<FAttributeDecl>        Attributes;   // index == float offset in the attribute buffer

        TVector<EdNodeGraph::FError> Errors;

        THashSet<CParticleGraphNode*> EmittedSpawn;
        THashSet<CParticleGraphNode*> EmittedUpdate;

        EParticleContext CurrentContext = EParticleContext::Spawn;
    };
}
