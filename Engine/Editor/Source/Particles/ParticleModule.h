#pragma once

#include <imgui.h>
#include "Containers/String.h"
#include "Core/Object/Object.h"
#include "Core/Math/Math.h"
#include "ParticleModule.generated.h"

namespace Lumina
{
    class FParticleCompiler;

    /** Stage of the per-particle simulation a module contributes code to. */
    enum class EParticleModuleStage : uint8
    {
        Spawn,      // Runs once when a particle is born; writes initial attributes.
        Update,     // Runs every frame on live particles; applies forces / over-life curves.
    };

    // Exported: a plugin or game editor module defines its own modules by deriving from this, which needs
    // the base's vtable and StaticClass out of the Editor DLL. Discovery alone is not enough -- reflection
    // would find the subclass, but it could not have linked in the first place.
    // One behavior in an emitter stack (Niagara-style): typed PROPERTY() inputs + emitted HLSL into the Spawn/Update compute, reading/writing P.* attributes and SimParams.
    // Editor-time authoring concept: serialized into the asset, but the runtime only consumes the compiled shader.
    REFLECT()
    class EDITOR_API CParticleModule : public CObject
    {
        GENERATED_BODY()

    public:

        /** Stack section this module belongs to. */
        virtual EParticleModuleStage GetStage() const { return EParticleModuleStage::Update; }

        /** Short label shown on the stack row and in the add-module palette. */
        virtual FString GetDisplayName() const { return "Module"; }

        /** Palette grouping (e.g. "Location", "Velocity", "Forces", "Color"). */
        virtual FString GetCategory() const { return "General"; }

        /** One-line description shown as a tooltip in the palette. */
        virtual FString GetTooltip() const { return ""; }

        /** Accent color for the stack row header. */
        virtual uint32 GetAccentColor() const { return IM_COL32(90, 90, 95, 255); }

        /** Emit this module's HLSL into the compiler's active (stage-matched) chunk. */
        virtual void Generate(FParticleCompiler& Compiler, int32 ModuleIndex) {}

        /** Whether this class appears in the add-module palette. The palette is discovered by reflection,
         *  so every CParticleModule subclass shows up by default -- including an intermediate base a plugin
         *  defines to share code between several real modules, which is not something anyone can usefully
         *  add to a stack. Such a base overrides this to false; concrete modules never need to. */
        virtual bool IsPaletteVisible() const { return true; }

        /** When false the module is skipped at compile time (kept in the stack for quick toggling). */
        PROPERTY(Editable, Category = "Module")
        bool bEnabled = true;

    protected:

        /** Unique HLSL local-variable name for this module instance, e.g. m3_dir. */
        static FString LocalVar(int32 ModuleIndex, const char* Name);

        /** Float / vector literal formatting for baking input values into the shader. */
        static FString Lit(float V);
        static FString Lit(const FVector2& V);
        static FString Lit(const FVector3& V);
        static FString Lit(const FVector4& V);
    };
}
