#pragma once

#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/RenderResource.h"
#include "ParticleSystemComponent.generated.h"

namespace Lumina
{
    REFLECT(Component, Category = "Effects")
    struct RUNTIME_API SParticleSystemComponent
    {
        GENERATED_BODY()

        SParticleSystemComponent() = default;
        
        
        SParticleSystemComponent(const SParticleSystemComponent& Other)
            : ParticleSystem(Other.ParticleSystem)
            , EmitterOffset(Other.EmitterOffset)
            , SpawnRateMultiplier(Other.SpawnRateMultiplier)
            , TimeScale(Other.TimeScale)
            , bEmit(Other.bEmit)
            , bBurstOnSpawn(Other.bBurstOnSpawn)
            , ParameterOverrides(Other.ParameterOverrides)
        {
        }

        SParticleSystemComponent& operator=(const SParticleSystemComponent& Other)
        {
            if (this != &Other)
            {
                ParticleSystem      = Other.ParticleSystem;
                EmitterOffset       = Other.EmitterOffset;
                SpawnRateMultiplier = Other.SpawnRateMultiplier;
                TimeScale           = Other.TimeScale;
                bEmit               = Other.bEmit;
                bBurstOnSpawn       = Other.bBurstOnSpawn;
                ParameterOverrides  = Other.ParameterOverrides;
                // Transient activation intents never carry across a copy; the render scene
                // allocates fresh GPU/sim state keyed by the new entity.
                bForceBurst         = false;
                bForceReset         = false;
            }
            return *this;
        }

        SParticleSystemComponent(SParticleSystemComponent&&) noexcept            = default;
        SParticleSystemComponent& operator=(SParticleSystemComponent&&) noexcept = default;

        /** The particle system asset that drives this emitter. */
        PROPERTY(Editable, Replicated, Category = "Particle System")
        TObjectPtr<CParticleSystem> ParticleSystem;

        /** Local-space offset applied to the emitter origin relative to the entity transform. */
        PROPERTY(Editable, Category = "Particle System")
        FVector3 EmitterOffset = FVector3(0.0f);

        /** Scales the asset's continuous spawn rate. 0 disables spawning, 1 uses the asset value. */
        PROPERTY(Editable, Category = "Particle System", ClampMin = 0.0f)
        float SpawnRateMultiplier = 1.0f;

        /** Scales the asset's simulation time step. Useful for slow-motion effects. */
        PROPERTY(Editable, Category = "Particle System", ClampMin = 0.0f)
        float TimeScale = 1.0f;

        /** Whether the emitter is currently spawning new particles. Existing particles keep simulating. */
        PROPERTY(Editable, Replicated, Category = "Particle System")
        bool bEmit = true;

        /** Auto-trigger the asset's BurstCount the first frame the component is active. */
        PROPERTY(Editable, Category = "Particle System")
        bool bBurstOnSpawn = true;

        // Per-instance overrides for asset-declared parameters; only diverging entries live here, reads
        // fall back to the asset.
        PROPERTY()
        TVector<FParticleParameter> ParameterOverrides;

        // Game-thread activation intents consumed by Extract into ParticleGPUStates; transient. bForceBurst
        // re-arms the burst; bForceReset also clears live particles.
        bool bForceBurst = false;
        bool bForceReset = false;

        // Turn the emitter on. bReset clears live particles on the next tick and re-fires the burst.
        FUNCTION(Script)
        void Activate(bool bReset = false)
        {
            bEmit       = true;
            bForceBurst = true;
            if (bReset)
            {
                bForceReset = true;
            }
        }

        // Stop emitting; existing particles keep simulating until they expire, so trails fade instead of popping.
        FUNCTION(Script)
        void Deactivate() { bEmit = false; }

        /** True while the emitter is spawning new particles. */
        FUNCTION(Script)
        bool IsActive() const { return bEmit; }

        //~ Begin User Parameters
        /** True if this component or its asset declares a parameter with the given name. */
        FUNCTION(Script)
        bool HasParameter(FName Name) const
        {
            return FindParameter(Name) != nullptr;
        }

        FUNCTION(Script) 
        float GetFloat(const FName& Name, float Default = 0.0f) const;
        
        FUNCTION(Script) 
        int32 GetInt(const FName& Name, int32 Default = 0) const;
        
        FUNCTION(Script) 
        bool GetBool(const FName& Name, bool Default = false) const;
        
        FUNCTION(Script) 
        FVector2 GetVec2(const FName& Name) const;
        
        FUNCTION(Script) 
        FVector3 GetVec3(const FName& Name) const;
        
        FUNCTION(Script)
        FVector4 GetVec4(const FName& Name) const;
        
        FUNCTION(Script)
        FVector4 GetColor(const FName& Name) const;

        FUNCTION(Script) 
        void SetFloat(const FName& Name, float Value);
        
        FUNCTION(Script) 
        void SetInt(const FName& Name, int32 Value);
        
        FUNCTION(Script)
        void SetBool(const FName& Name, bool Value);
        
        FUNCTION(Script) 
        void SetVec2(const FName& Name, FVector2 Value);
        
        FUNCTION(Script) 
        void SetVec3(const FName& Name, FVector3 Value);
        
        FUNCTION(Script) 
        void SetVec4(const FName& Name, FVector4 Value);
        
        FUNCTION(Script) 
        void SetColor(const FName& Name, FVector4 Value);

        /** Drop the override for this parameter, reverting to the asset default. */
        FUNCTION(Script) 
        void ResetParameter(const FName& Name);

    private:

        /** Resolve a parameter by name, preferring component overrides over the asset's default. */
        const FParticleParameter* FindParameter(const FName& Name) const;

        // Get-or-create the override entry for (Name, type). Returns nullptr if the asset declares the
        // parameter with a different type (programmer error).
        FParticleParameter* GetOrCreateOverride(FName Name, EParticleParameterType ExpectedType);
        //~ End User Parameters
    };
}
