#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "MCPToolTypes.h"

#include "MCPParticleTools.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EParticleEmitterAction : uint8
    {
        Add,
        AddAsset,
        Remove,
        MakeLocal,
        SaveAsAsset,
    };

    REFLECT()
    enum class EParticleModuleAction : uint8
    {
        Add,
        Remove,
        Move,
    };

    REFLECT()
    enum class EParticleUserParameterAction : uint8
    {
        Set,
        Remove,
    };

    REFLECT()
    struct MCPEDITOR_API SParticleModuleTypeInfo
    {
        GENERATED_BODY()

        // Type name to pass to particle.module Add.
        PROPERTY()
        FString Name;

        PROPERTY()
        FString DisplayName;

        PROPERTY()
        FString Category;

        // Spawn runs once per particle at birth, Update every frame, Grid per grid cell, Target per texel.
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

        // Include each type's parameter schema, which is large.
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

        // Position in its stage, which every module tool takes.
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

        // Path of the shared emitter asset, or empty when the emitter belongs to the system.
        PROPERTY()
        FString SharedAsset;

        // Editable emitter settings such as SpawnRate, MaxParticles, BurstCount and Material, as JSON.
        PROPERTY()
        FString Settings;

        PROPERTY()
        TVector<SParticleModuleInfo> SpawnModules;

        PROPERTY()
        TVector<SParticleModuleInfo> UpdateModules;

        // Simulation Stage modules among these start a new pass over the grid.
        PROPERTY()
        TVector<SParticleModuleInfo> GridModules;

        PROPERTY()
        TVector<SParticleModuleInfo> TargetModules;
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

        // The asset default as JSON: a number, a bool, or an object with X, Y, Z and W.
        PROPERTY()
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeParticleSystemResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        // Named values a module input binds to through its ParameterName, overridable per component from script.
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
    struct MCPEDITOR_API SParticleEmitterParams
    {
        GENERATED_BODY()

        PROPERTY()
        EParticleEmitterAction Action = EParticleEmitterAction::Add;

        PROPERTY()
        FString System;

        // Emitter index, as particle.describe reports it. Remove, MakeLocal and SaveAsAsset.
        PROPERTY()
        int32 Emitter = 0;

        // Add only. Display name for the new emitter; empty picks a unique one.
        PROPERTY()
        FString Name;

        // AddAsset only. GUID of the emitter asset to share.
        PROPERTY()
        FString Asset;

        // SaveAsAsset only. Virtual path of the new asset, such as /Game/Content/Effects/E_Sparks.lasset.
        PROPERTY()
        FString Path;

        // Add only. Start from the starter stack; off starts both stacks empty.
        PROPERTY()
        bool bStarterStack = true;
    };

    REFLECT()
    struct MCPEDITOR_API SParticleEmitterResult
    {
        GENERATED_BODY()

        // Add and AddAsset: index of the new emitter.
        PROPERTY()
        int32 Index = 0;

        // Add and AddAsset: name of the new emitter.
        PROPERTY()
        FString Name;

        // SaveAsAsset: GUID of the new emitter asset.
        PROPERTY()
        FString Guid;

        // SaveAsAsset: path of the new emitter asset.
        PROPERTY()
        FString Path;

        // Remove and MakeLocal.
        PROPERTY()
        bool bSucceeded = false;
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
    struct MCPEDITOR_API SParticleModuleParams
    {
        GENERATED_BODY()

        PROPERTY()
        EParticleModuleAction Action = EParticleModuleAction::Add;

        PROPERTY()
        FString System;

        PROPERTY()
        int32 Emitter = 0;

        // Remove and Move. Spawn, Update, Grid or Target.
        PROPERTY()
        FString Stage;

        // Position in the stage. For Add, negative appends, though Solve Forces and Velocity usually goes last.
        PROPERTY()
        int32 Index = -1;

        // Move only.
        PROPERTY()
        int32 NewIndex = 0;

        // Add only. Type name from particle.list_module_types, which decides the stage.
        PROPERTY()
        FString ModuleType;
    };

    REFLECT()
    struct MCPEDITOR_API SParticleModuleResult
    {
        GENERATED_BODY()

        // Add: the stage the module joined.
        PROPERTY()
        FString Stage;

        // Add: the module's position in its stage.
        PROPERTY()
        int32 Index = 0;

        // Add: the module's settable fields, as JSON.
        PROPERTY()
        FString Values;

        // Remove and Move.
        PROPERTY()
        bool bSucceeded = false;
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
    struct MCPEDITOR_API SParticleGraphParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString System;

        PROPERTY()
        int32 Emitter = 0;

        // Stage and Index of the Graph Module, as the module tools take them.
        PROPERTY()
        FString Stage;

        PROPERTY()
        int32 Index = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SParticleGraphResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SGraphNodeInfo> Nodes;

        // Node types particle.graph_add_node takes, including one per material formula.
        PROPERTY()
        TVector<FString> NodeTypes;
    };

    REFLECT()
    struct MCPEDITOR_API SAddParticleGraphNodeParams
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

        // A name from particle.graph_describe's NodeTypes, such as Set Particle or Inverse Lerp.
        PROPERTY()
        FString NodeType;

        PROPERTY()
        float X = 0.0f;

        PROPERTY()
        float Y = 0.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SAddParticleGraphNodeResult
    {
        GENERATED_BODY()

        PROPERTY()
        int64 Id = 0;

        PROPERTY()
        TVector<SGraphPinInfo> Pins;
    };

    REFLECT()
    struct MCPEDITOR_API SConnectParticleGraphParams
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
        int64 FromNode = 0;

        PROPERTY()
        FString FromPin;

        PROPERTY()
        int64 ToNode = 0;

        PROPERTY()
        FString ToPin;
    };

    REFLECT()
    struct MCPEDITOR_API SSetParticleGraphNodePropertyParams
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
        int64 Node = 0;

        // Field path on the node, such as Channel or Value.Constant.
        PROPERTY()
        FString Path;

        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SSetParticleArrayParams
    {
        GENERATED_BODY()

        // An entity with a particle system component, in the scene editor.tabs Target points at.
        PROPERTY()
        FString Entity;

        // The array name modules declare, such as a Spawn From Array module's Positions.
        PROPERTY()
        FString Name;

        PROPERTY()
        TVector<FVector4> Values;
    };

    REFLECT()
    struct MCPEDITOR_API SSampleParticleGridParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Entity;

        PROPERTY()
        int32 Emitter = 0;

        PROPERTY()
        FString Channel;

        PROPERTY()
        FVector3 Position = FVector3(0.0f);
    };

    REFLECT()
    struct MCPEDITOR_API SSampleParticleGridResult
    {
        GENERATED_BODY()

        PROPERTY()
        float Value = 0.0f;

        // False until a grid copy has come back, which needs Read Back Grid on the emitter and a few frames.
        PROPERTY()
        bool bHasSnapshot = false;

        PROPERTY()
        TVector<FString> Channels;
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
    struct MCPEDITOR_API SParticleUserParameterParams
    {
        GENERATED_BODY()

        PROPERTY()
        EParticleUserParameterAction Action = EParticleUserParameterAction::Set;

        PROPERTY()
        FString System;

        PROPERTY()
        FString Name;

        // Set only. Float, Int, Bool, Vec2, Vec3, Vec4 or Color.
        PROPERTY()
        FString Type;

        // Set only. A number, a bool, or an object with X, Y, Z and W, matching Type.
        PROPERTY(RawJson)
        FString Value;
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
        MCPEDITOR_API void RegisterParticleTools(FStringView Owner);
    }
}
