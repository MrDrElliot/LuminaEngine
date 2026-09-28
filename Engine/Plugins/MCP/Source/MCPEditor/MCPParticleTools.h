#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPParticleTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SParticleModuleTypeInfo
    {
        GENERATED_BODY()

        // Type name to pass to particle.add_module.
        PROPERTY()
        FString Name;

        PROPERTY()
        FString DisplayName;

        PROPERTY()
        FString Category;

        // Spawn modules run once per particle at birth, Update modules every frame after.
        PROPERTY()
        FString Stage;

        PROPERTY()
        FString Description;

        // JSON Schema of the module's settable fields, for particle.set_module_property.
        PROPERTY()
        FString ParameterSchema;
    };

    REFLECT()
    struct MCPEDITOR_API SListParticleModuleTypesParams
    {
        GENERATED_BODY()

        // Only types whose name, category or stage contains this. Empty lists every one.
        PROPERTY()
        FString Contains;

        // Include each type's parameter schema, which is large. Off by default.
        PROPERTY()
        bool bIncludeSchema = false;
    };

    REFLECT()
    struct MCPEDITOR_API SListParticleModuleTypesResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SParticleModuleTypeInfo> Types;
    };

    REFLECT()
    struct MCPEDITOR_API SParticleModuleInfo
    {
        GENERATED_BODY()

        // Position in its stage, which is what every module tool takes.
        PROPERTY()
        int32 Index = 0;

        PROPERTY()
        FString Type;

        PROPERTY()
        FString DisplayName;

        PROPERTY()
        bool bEnabled = true;

        // Current values of the module's settable fields, as JSON.
        PROPERTY()
        FString Values;
    };

    REFLECT()
    struct MCPEDITOR_API SParticleEmitterInfo
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Index = 0;

        PROPERTY()
        FString Name;

        // Whether the emitter has a compiled shader. An uncompiled emitter draws nothing.
        PROPERTY()
        bool bCompiled = false;

        // Editable emitter settings such as SpawnRate, MaxParticles, BurstCount and Material, as JSON.
        PROPERTY()
        FString Settings;

        PROPERTY()
        TVector<SParticleModuleInfo> SpawnModules;

        PROPERTY()
        TVector<SParticleModuleInfo> UpdateModules;
    };

    REFLECT()
    struct MCPEDITOR_API SParticleSystemParams
    {
        GENERATED_BODY()

        // GUID of the particle system, from assets.search or particle.create.
        PROPERTY()
        FString System;
    };

    REFLECT()
    struct MCPEDITOR_API SParticleUserParameterInfo
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        // Float, Int, Bool, Vec2, Vec3, Vec4 or Color.
        PROPERTY()
        FString Type;

        // The asset default as JSON, a number, a bool, or an object with X, Y, Z and W.
        PROPERTY()
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeParticleSystemResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        // Named values a module input binds to through its ParameterName, and a component can override from script.
        PROPERTY()
        TVector<SParticleUserParameterInfo> UserParameters;

        PROPERTY()
        TVector<SParticleEmitterInfo> Emitters;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateParticleSystemParams
    {
        GENERATED_BODY()

        // Folder to create it in, such as /Game/Content/Effects.
        PROPERTY()
        FString Folder;

        PROPERTY()
        FString Name;

        // Start from the starter stack, a point burst with gravity and a fade. Off starts both stacks empty.
        PROPERTY()
        bool bStarterStack = true;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateParticleSystemResult
    {
        GENERATED_BODY()

        // GUID to pass to every other particle tool.
        PROPERTY()
        FString Guid;

        PROPERTY()
        FString Path;
    };

    REFLECT()
    struct MCPEDITOR_API SAddParticleEmitterParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString System;

        // Display name for the new emitter. Empty picks a unique one.
        PROPERTY()
        FString Name;

        PROPERTY()
        bool bStarterStack = true;
    };

    REFLECT()
    struct MCPEDITOR_API SParticleEmitterParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString System;

        // Emitter index, as particle.describe reports it.
        PROPERTY()
        int32 Emitter = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SAddParticleEmitterResult
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Index = 0;

        PROPERTY()
        FString Name;
    };

    REFLECT()
    struct MCPEDITOR_API SSetParticleEmitterPropertyParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString System;

        PROPERTY()
        int32 Emitter = 0;

        // Field on the emitter, such as SpawnRate, BurstCount, MaxParticles, Duration, bLooping or Material.
        PROPERTY()
        FString Path;

        // The new value as JSON. An asset reference such as Material or Texture takes the asset's GUID string.
        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SAddParticleModuleParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString System;

        PROPERTY()
        int32 Emitter = 0;

        // Module type name, from particle.list_module_types. The type decides its stage.
        PROPERTY()
        FString ModuleType;

        // Position within its stage. Negative appends, but Solve Forces and Velocity usually belongs last.
        PROPERTY()
        int32 Index = -1;
    };

    REFLECT()
    struct MCPEDITOR_API SAddParticleModuleResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Stage;

        PROPERTY()
        int32 Index = 0;

        PROPERTY()
        FString Values;
    };

    REFLECT()
    struct MCPEDITOR_API SParticleModuleParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString System;

        PROPERTY()
        int32 Emitter = 0;

        // Spawn or Update.
        PROPERTY()
        FString Stage;

        PROPERTY()
        int32 Index = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SMoveParticleModuleParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString System;

        PROPERTY()
        int32 Emitter = 0;

        PROPERTY()
        FString Stage;

        PROPERTY()
        int32 Index = 0;

        PROPERTY()
        int32 NewIndex = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SSetParticleModulePropertyParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString System;

        PROPERTY()
        int32 Emitter = 0;

        PROPERTY()
        FString Stage;

        PROPERTY()
        int32 Index = 0;

        // Field path on the module, such as Color, Radius or bEnabled.
        PROPERTY()
        FString Path;

        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SParticlePropertyResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Previous;

        PROPERTY()
        FString Current;

        // True when the edit changed the generated code, so particle.compile has to run before it shows.
        PROPERTY()
        bool bNeedsCompile = false;
    };

    REFLECT()
    struct MCPEDITOR_API SSetParticleUserParameterParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString System;

        PROPERTY()
        FString Name;

        // Float, Int, Bool, Vec2, Vec3, Vec4 or Color.
        PROPERTY()
        FString Type;

        // A number, a bool, or an object with X, Y, Z and W, matching Type.
        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SRemoveParticleUserParameterParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString System;

        PROPERTY()
        FString Name;
    };

    REFLECT()
    struct MCPEDITOR_API SParticleEditResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bSucceeded = false;
    };

    REFLECT()
    struct MCPEDITOR_API SCompileParticleSystemResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bSucceeded = false;

        PROPERTY()
        TVector<FString> Errors;

        PROPERTY()
        TVector<FString> Notes;
    };

    namespace MCP
    {
        void RegisterParticleTools(FStringView Owner);
    }
}
