#include "MCPParticleTools.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentPropertyPath.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Agent/AgentToolSchema.h"
#include "Asset/AssetOps.h"
#include "MCPTextMatch.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Assets/Factories/Factory.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Particles/ParticleOps.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"

namespace Lumina::MCP
{
    // Named because the other tool files share these Register names and can land in one unity blob.
    namespace ParticleTools
    {
        enum class EParticleAccess : uint8
        {
            Read,
            Write,
        };

        bool ResolveSystem(const FString& Guid, EParticleAccess Access, CParticleSystem*& Out, FString& OutError)
        {
            CObject* Asset = nullptr;
            if (!Agent::ResolveAssetObject(FStringView(Guid), Asset, OutError))
            {
                return false;
            }

            Out = Cast<CParticleSystem>(Asset);
            if (Out == nullptr)
            {
                OutError = Lumina::Format("'{}' is a {}, not a particle system.", Guid, Asset->GetClass()->GetName());
                return false;
            }

            // The open editor keeps its own list of stacks, which an edit made here would silently diverge from.
            if (Access == EParticleAccess::Write)
            {
                const FString OpenIn = NodeGraphOps::FindOpenEditorName(Out);
                if (!OpenIn.empty())
                {
                    OutError = Lumina::Format("'{}' is open in {}, which would not see this change. Close it and try again.",
                        Out->GetName(), OpenIn);
                    return false;
                }
            }

            return true;
        }

        bool CheckEmitter(CParticleSystem* System, int32 Emitter, FString& OutError)
        {
            if (Emitter < 0 || Emitter >= (int32)System->Emitters.size() || System->Emitters[Emitter] == nullptr)
            {
                OutError = Lumina::Format("Emitter {} does not exist. This system has {} emitter(s).", Emitter, System->Emitters.size());
                return false;
            }

            return true;
        }

        bool ResolveModule(CParticleSystem* System, int32 Emitter, const FString& StageName, int32 Index,
            EParticleModuleStage& OutStage, CParticleModule*& OutModule, FString& OutError)
        {
            if (!CheckEmitter(System, Emitter, OutError))
            {
                return false;
            }

            if (!ParticleOps::ParseStage(FStringView(StageName), OutStage))
            {
                OutError = Lumina::Format("'{}' is not a stage. Use Spawn or Update.", StageName);
                return false;
            }

            TVector<TStrongObjectPtr<CParticleModule>>* Modules = ParticleOps::GetModules(System, Emitter, OutStage);
            if (Modules == nullptr || Index < 0 || Index >= (int32)Modules->size() || (*Modules)[Index] == nullptr)
            {
                OutError = Lumina::Format("There is no {} module {} on emitter {}.", StageName, Index, Emitter);
                return false;
            }

            OutModule = (*Modules)[Index].Get();
            return true;
        }

        // Only what the details panel shows, since the reflected emitter also carries its compiled shader.
        FString EditableValues(CStruct* Type, void* Data)
        {
            nlohmann::json Values;
            if (!Agent::WriteStruct(Type, Data, Values).IsValid() || !Values.is_object())
            {
                return FString();
            }

            for (FProperty* Property : Type->GetProperties())
            {
                if (Property != nullptr && !EnumHasAnyFlags(Property->Flags, EPropertyFlags::Editable))
                {
                    Values.erase(Property->GetPropertyName().ToString().c_str());
                }
            }

            return FString(Values.dump().c_str());
        }

        void CollectModules(CParticleSystem* System, int32 Emitter, EParticleModuleStage Stage, TVector<SParticleModuleInfo>& Out)
        {
            TVector<TStrongObjectPtr<CParticleModule>>* Modules = ParticleOps::GetModules(System, Emitter, Stage);
            if (Modules == nullptr)
            {
                return;
            }

            for (int32 Index = 0; Index < (int32)Modules->size(); ++Index)
            {
                CParticleModule* Module = (*Modules)[Index].Get();
                if (Module == nullptr)
                {
                    continue;
                }

                SParticleModuleInfo Info;
                Info.Index       = Index;
                Info.Type        = FString(Module->GetClass()->GetName().ToString().c_str());
                Info.DisplayName = Module->GetDisplayName();
                Info.bEnabled    = Module->bEnabled;
                Info.Values      = EditableValues(Module->GetClass(), Module);
                Out.push_back(Move(Info));
            }
        }

        void ClearStacks(CParticleSystem* System, int32 Emitter)
        {
            for (EParticleModuleStage Stage : { EParticleModuleStage::Spawn, EParticleModuleStage::Update })
            {
                if (TVector<TStrongObjectPtr<CParticleModule>>* Modules = ParticleOps::GetModules(System, Emitter, Stage))
                {
                    Modules->clear();
                }
            }
        }

        void MarkDirty(CParticleSystem* System)
        {
            if (CPackage* Package = System->GetPackage())
            {
                Package->MarkDirty();
            }
        }

        const char* ParameterTypeNames[] = { "Float", "Int", "Bool", "Vec2", "Vec3", "Vec4", "Color" };

        bool ParseParameterType(const FString& Name, EParticleParameterType& Out)
        {
            for (int32 Index = 0; Index < (int32)std::size(ParameterTypeNames); ++Index)
            {
                if (Name == ParameterTypeNames[Index])
                {
                    Out = (EParticleParameterType)Index;
                    return true;
                }
            }

            return false;
        }

        nlohmann::json ParameterValueJson(const FParticleParameter& Param)
        {
            switch (Param.Type)
            {
                case EParticleParameterType::Float: return Param.Scalar;
                case EParticleParameterType::Int:   return Param.Integer;
                case EParticleParameterType::Bool:  return Param.Boolean;
                default:
                    return nlohmann::json{ { "X", Param.Vector.x }, { "Y", Param.Vector.y }, { "Z", Param.Vector.z }, { "W", Param.Vector.w } };
            }
        }

        bool ReadParameterValue(const nlohmann::json& Value, FParticleParameter& Param, FString& OutError)
        {
            switch (Param.Type)
            {
                case EParticleParameterType::Float:
                    if (!Value.is_number())
                    {
                        OutError = "A Float parameter takes a number.";
                        return false;
                    }
                    Param.Scalar = Value.get<float>();
                    return true;
                case EParticleParameterType::Int:
                    if (!Value.is_number())
                    {
                        OutError = "An Int parameter takes a number.";
                        return false;
                    }
                    Param.Integer = Value.get<int32>();
                    return true;
                case EParticleParameterType::Bool:
                    if (!Value.is_boolean())
                    {
                        OutError = "A Bool parameter takes true or false.";
                        return false;
                    }
                    Param.Boolean = Value.get<bool>();
                    return true;
                default:
                    if (!Value.is_object())
                    {
                        OutError = "A vector or color parameter takes an object with X, Y, Z and W.";
                        return false;
                    }
                    Param.Vector = FVector4(Value.value("X", 0.0f), Value.value("Y", 0.0f), Value.value("Z", 0.0f), Value.value("W", Param.Type == EParticleParameterType::Color ? 1.0f : 0.0f));
                    return true;
            }
        }

        FString EmitterName(CParticleSystem* System, int32 Emitter)
        {
            return System->Emitters[Emitter]->EmitterName;
        }

        void RegisterListModuleTypes(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListParticleModuleTypesParams, SListParticleModuleTypesResult>(
                Owner, "particle.list_module_types",
                "List every module type that can go in an emitter's Spawn or Update stack.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListParticleModuleTypesParams& In, SListParticleModuleTypesResult& Out)
                {
                    for (CClass* Class : ParticleOps::GetModuleTypes())
                    {
                        CParticleModule* CDO = Class->GetDefaultObject<CParticleModule>();
                        SParticleModuleTypeInfo Info;
                        Info.Name        = FString(Class->GetName().ToString().c_str());
                        Info.DisplayName = CDO->GetDisplayName();
                        Info.Category    = CDO->GetCategory();
                        Info.Stage       = ParticleOps::StageName(CDO->GetStage());
                        Info.Description = CDO->GetTooltip();

                        if (!ContainsText(FStringView(Info.Name), In.Contains)
                            && !ContainsText(FStringView(Info.DisplayName), In.Contains)
                            && !ContainsText(FStringView(Info.Category), In.Contains)
                            && !ContainsText(FStringView(Info.Stage), In.Contains))
                        {
                            continue;
                        }

                        if (In.bIncludeSchema)
                        {
                            const Agent::FSchemaResult Schema = Agent::GenerateSchema(Class);
                            if (Schema.IsValid())
                            {
                                Info.ParameterSchema = FString(Schema.Schema.dump().c_str());
                            }
                        }

                        Out.Types.push_back(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} particle module type(s).", Out.Types.size()));
                });
        }

        void RegisterDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SParticleSystemParams, SDescribeParticleSystemResult>(
                Owner, "particle.describe",
                "Report a particle system's emitters, their settings and both module stacks with every module's values.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SParticleSystemParams& In, SDescribeParticleSystemResult& Out)
                {
                    CParticleSystem* System = nullptr;
                    FString Error;
                    if (!ResolveSystem(In.System, EParticleAccess::Read, System, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Name = FString(System->GetName().ToString().c_str());
                    for (const FParticleParameter& Param : System->UserParameters)
                    {
                        SParticleUserParameterInfo Info;
                        Info.Name  = FString(Param.Name.ToString().c_str());
                        Info.Type  = ParameterTypeNames[(int32)Param.Type];
                        Info.Value = FString(ParameterValueJson(Param).dump().c_str());
                        Out.UserParameters.push_back(Move(Info));
                    }

                    for (int32 Index = 0; Index < (int32)System->Emitters.size(); ++Index)
                    {
                        CParticleEmitter* Emitter = System->Emitters[Index].Get();
                        if (Emitter == nullptr)
                        {
                            continue;
                        }

                        SParticleEmitterInfo Info;
                        Info.Index     = Index;
                        Info.Name      = Emitter->EmitterName;
                        Info.bCompiled = !Emitter->ComputeShaderBinaries.empty();
                        Info.Settings  = EditableValues(Emitter->GetClass(), Emitter);
                        CollectModules(System, Index, EParticleModuleStage::Spawn, Info.SpawnModules);
                        CollectModules(System, Index, EParticleModuleStage::Update, Info.UpdateModules);
                        Out.Emitters.push_back(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("'{}' has {} emitter(s).", Out.Name, Out.Emitters.size()));
                });
        }

        void RegisterCreate(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateParticleSystemParams, SCreateParticleSystemResult>(
                Owner, "particle.create",
                "Create a particle system asset with one emitter, compiled and saved so it can be placed right away.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateParticleSystemParams& In, SCreateParticleSystemResult& Out)
                {
                    if (In.Name.empty())
                    {
                        return Agent::FToolResult::Error("A particle system needs a name.");
                    }

                    if (!AssetOps::IsAssetLocation(FStringView(In.Folder)))
                    {
                        return Agent::FToolResult::Error("Particle systems belong under /Game/Content, since nothing scans for assets elsewhere.");
                    }

                    if (!VFS::IsDirectory(In.Folder))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} is not a folder. Use assets.create_folder first.", In.Folder));
                    }

                    FFixedString Path = Paths::Combine(FStringView(In.Folder), FStringView(In.Name));
                    CPackage::AddPackageExt(Path);
                    if (VFS::Exists(Path))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} already exists.", Path));
                    }

                    CParticleSystem* System = CFactory::CreateNewOf<CParticleSystem>(FStringView(Path.c_str(), Path.size()));

                    // CreateNewOf skips the factory, which is what gives a new system its first emitter.
                    if (System != nullptr && System->Emitters.empty())
                    {
                        System->AddEmitter();
                    }

                    if (System == nullptr || System->Emitters.empty())
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not create a particle system at {}.", Path));
                    }

                    ParticleOps::FindOrCreateStack(System, 0);
                    if (!In.bStarterStack)
                    {
                        ClearStacks(System, 0);
                    }

                    FParticleEmitterCompileOutput Compiled;
                    if (In.bStarterStack)
                    {
                        ParticleOps::CompileEmitter(System, 0, Compiled);
                    }

                    if (!CPackage::SavePackage(System->GetPackage(), Path))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not save {}.", Path));
                    }

                    FAssetRegistry::Get().AssetCreated(System);
                    Out.Path = FString(Path.c_str());
                    Out.Guid = FString(System->GetGUID().ToString().c_str());
                    return Agent::FToolResult::Ok(Lumina::Format("Created {}.", Out.Path));
                });
        }

        void RegisterAddEmitter(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddParticleEmitterParams, SAddParticleEmitterResult>(
                Owner, "particle.add_emitter",
                "Add an emitter to a particle system, drawn after the ones it already has.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddParticleEmitterParams& In, SAddParticleEmitterResult& Out)
                {
                    CParticleSystem* System = nullptr;
                    FString Error;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, System, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CParticleEmitter* Emitter = System->AddEmitter();
                    if (Emitter == nullptr)
                    {
                        return Agent::FToolResult::Error("The system refused to add an emitter.");
                    }

                    if (!In.Name.empty())
                    {
                        Emitter->EmitterName = In.Name;
                    }

                    Out.Index = (int32)System->Emitters.size() - 1;
                    Out.Name  = Emitter->EmitterName;
                    ParticleOps::FindOrCreateStack(System, Out.Index);
                    if (!In.bStarterStack)
                    {
                        ClearStacks(System, Out.Index);
                    }

                    MarkDirty(System);
                    return Agent::FToolResult::Ok(Lumina::Format("Added emitter {} '{}'. Compile before it draws.", Out.Index, Out.Name));
                });
        }

        void RegisterRemoveEmitter(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SParticleEmitterParams, SParticleEditResult>(
                Owner, "particle.remove_emitter",
                "Remove an emitter from a particle system. The last emitter cannot be removed.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SParticleEmitterParams& In, SParticleEditResult& Out)
                {
                    CParticleSystem* System = nullptr;
                    FString Error;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, System, Error) || !CheckEmitter(System, In.Emitter, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (!System->RemoveEmitter(System->Emitters[In.Emitter].Get()))
                    {
                        return Agent::FToolResult::Error("That is the only emitter, and a system needs at least one.");
                    }

                    Out.bSucceeded = true;
                    MarkDirty(System);
                    return Agent::FToolResult::Ok(Lumina::Format("Removed emitter {}.", In.Emitter));
                });
        }

        void RegisterSetEmitterProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetParticleEmitterPropertyParams, SParticlePropertyResult>(
                Owner, "particle.set_emitter_property",
                "Set one emitter setting, such as SpawnRate, BurstCount, MaxParticles, Duration, bLooping, BlendMode or Material. Takes effect without a compile.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetParticleEmitterPropertyParams& In, SParticlePropertyResult& Out)
                {
                    CParticleSystem* System = nullptr;
                    FString Error;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, System, Error) || !CheckEmitter(System, In.Emitter, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CParticleEmitter* Emitter = System->Emitters[In.Emitter].Get();
                    Agent::FResolvedProperty Property;
                    if (!Agent::ResolvePropertyPath(Emitter->GetClass(), Emitter, FStringView(In.Path), Property, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    const nlohmann::json Value = nlohmann::json::parse(In.Value.c_str(), In.Value.c_str() + In.Value.size(), nullptr, false);
                    if (Value.is_discarded())
                    {
                        return Agent::FToolResult::Error("Value is not JSON. Strings need quotes, so a quoted name rather than a bare word.");
                    }

                    if (const Agent::FMarshalResult Check = Agent::ValidatePropertyValue(Value, Property.Property, FStringView(In.Path)); !Check.IsValid())
                    {
                        return Agent::FToolResult::Error(Check.Error);
                    }

                    nlohmann::json Before;
                    Agent::WriteProperty(Property.Property, Property.ValuePtr, Before);
                    Out.Previous = FString(Before.dump().c_str());

                    if (const Agent::FMarshalResult Applied = Agent::ReadProperty(Value, Property.Property, Property.ValuePtr, FStringView(In.Path)); !Applied.IsValid())
                    {
                        return Agent::FToolResult::Error(Applied.Error);
                    }

                    // A material or texture swap rebinds render state the renderer resolves on load.
                    Emitter->PostLoad();

                    nlohmann::json After;
                    Agent::WriteProperty(Property.Property, Property.ValuePtr, After);
                    Out.Current = FString(After.dump().c_str());
                    MarkDirty(System);
                    return Agent::FToolResult::Ok(Lumina::Format("Emitter {} {} is now {}.", In.Emitter, In.Path, Out.Current));
                });
        }

        void RegisterAddModule(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddParticleModuleParams, SAddParticleModuleResult>(
                Owner, "particle.add_module",
                "Add a module to an emitter. The module type decides whether it joins the Spawn or the Update stack.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddParticleModuleParams& In, SAddParticleModuleResult& Out)
                {
                    CParticleSystem* System = nullptr;
                    FString Error;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, System, Error) || !CheckEmitter(System, In.Emitter, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CClass* ModuleClass = ParticleOps::ResolveModuleType(FStringView(In.ModuleType));
                    if (ModuleClass == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No particle module type is named '{}'. Use particle.list_module_types.", In.ModuleType));
                    }

                    CParticleModule* Module = ParticleOps::AddModule(System, In.Emitter, ModuleClass, In.Index);
                    if (Module == nullptr)
                    {
                        return Agent::FToolResult::Error("The emitter refused that module.");
                    }

                    const EParticleModuleStage Stage = Module->GetStage();
                    TVector<TStrongObjectPtr<CParticleModule>>* Modules = ParticleOps::GetModules(System, In.Emitter, Stage);
                    for (int32 Index = 0; Modules != nullptr && Index < (int32)Modules->size(); ++Index)
                    {
                        if ((*Modules)[Index].Get() == Module)
                        {
                            Out.Index = Index;
                        }
                    }

                    Out.Stage  = ParticleOps::StageName(Stage);
                    Out.Values = EditableValues(Module->GetClass(), Module);
                    MarkDirty(System);
                    return Agent::FToolResult::Ok(Lumina::Format("Added {} as {} module {} on '{}'. Compile before it takes effect.",
                        Module->GetDisplayName(), Out.Stage, Out.Index, EmitterName(System, In.Emitter)));
                });
        }

        void RegisterRemoveModule(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SParticleModuleParams, SParticleEditResult>(
                Owner, "particle.remove_module",
                "Remove a module from an emitter's stack.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SParticleModuleParams& In, SParticleEditResult& Out)
                {
                    CParticleSystem* System = nullptr;
                    FString Error;
                    EParticleModuleStage Stage;
                    CParticleModule* Module = nullptr;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, System, Error)
                        || !ResolveModule(System, In.Emitter, In.Stage, In.Index, Stage, Module, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.bSucceeded = ParticleOps::RemoveModule(System, In.Emitter, Stage, In.Index);
                    MarkDirty(System);
                    return Agent::FToolResult::Ok(Lumina::Format("Removed {} module {}. Compile before it takes effect.", In.Stage, In.Index));
                });
        }

        void RegisterMoveModule(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SMoveParticleModuleParams, SParticleEditResult>(
                Owner, "particle.move_module",
                "Move a module to another position in its stack, since modules run top to bottom.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SMoveParticleModuleParams& In, SParticleEditResult& Out)
                {
                    CParticleSystem* System = nullptr;
                    FString Error;
                    EParticleModuleStage Stage;
                    CParticleModule* Module = nullptr;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, System, Error)
                        || !ResolveModule(System, In.Emitter, In.Stage, In.Index, Stage, Module, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (!ParticleOps::MoveModule(System, In.Emitter, Stage, In.Index, In.NewIndex))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} is not a position in that stack.", In.NewIndex));
                    }

                    Out.bSucceeded = true;
                    MarkDirty(System);
                    return Agent::FToolResult::Ok(Lumina::Format("Moved {} module {} to {}. Compile before it takes effect.", In.Stage, In.Index, In.NewIndex));
                });
        }

        void RegisterSetModuleProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetParticleModulePropertyParams, SParticlePropertyResult>(
                Owner, "particle.set_module_property",
                "Set one input on a module, such as Color.Constant, or bind it to a user parameter by setting Color.ParameterName. A value change on a compiled emitter applies at once, and bNeedsCompile says when it did not.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetParticleModulePropertyParams& In, SParticlePropertyResult& Out)
                {
                    CParticleSystem* System = nullptr;
                    FString Error;
                    EParticleModuleStage Stage;
                    CParticleModule* Module = nullptr;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, System, Error)
                        || !ResolveModule(System, In.Emitter, In.Stage, In.Index, Stage, Module, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Agent::FResolvedProperty Property;
                    if (!Agent::ResolvePropertyPath(Module->GetClass(), Module, FStringView(In.Path), Property, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    const nlohmann::json Value = nlohmann::json::parse(In.Value.c_str(), In.Value.c_str() + In.Value.size(), nullptr, false);
                    if (Value.is_discarded())
                    {
                        return Agent::FToolResult::Error("Value is not JSON. Strings need quotes, so a quoted name rather than a bare word.");
                    }

                    if (const Agent::FMarshalResult Check = Agent::ValidatePropertyValue(Value, Property.Property, FStringView(In.Path)); !Check.IsValid())
                    {
                        return Agent::FToolResult::Error(Check.Error);
                    }

                    nlohmann::json Before;
                    Agent::WriteProperty(Property.Property, Property.ValuePtr, Before);
                    Out.Previous = FString(Before.dump().c_str());

                    if (const Agent::FMarshalResult Applied = Agent::ReadProperty(Value, Property.Property, Property.ValuePtr, FStringView(In.Path)); !Applied.IsValid())
                    {
                        return Agent::FToolResult::Error(Applied.Error);
                    }

                    nlohmann::json After;
                    Agent::WriteProperty(Property.Property, Property.ValuePtr, After);
                    Out.Current = FString(After.dump().c_str());

                    // An enum picks a code branch rather than a slot value, which only a recompile can change.
                    Out.bNeedsCompile = !ParticleOps::RefreshModuleParams(System, In.Emitter);
                    MarkDirty(System);
                    return Agent::FToolResult::Ok(Lumina::Format("{} module {} {} is now {}{}", In.Stage, In.Index, In.Path, Out.Current,
                        Out.bNeedsCompile ? ". Compile before it takes effect." : ", live."));
                });
        }

        void RegisterSetUserParameter(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetParticleUserParameterParams, SParticleEditResult>(
                Owner, "particle.set_user_parameter",
                "Declare or change a named parameter on the system. Bind a module input to it by setting that input's ParameterName with particle.set_module_property, then override it per component from script.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetParticleUserParameterParams& In, SParticleEditResult& Out)
                {
                    CParticleSystem* System = nullptr;
                    FString Error;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, System, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (In.Name.empty())
                    {
                        return Agent::FToolResult::Error("A parameter needs a name.");
                    }

                    FParticleParameter Param;
                    Param.Name = FName(In.Name.c_str());
                    if (!ParseParameterType(In.Type, Param.Type))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("'{}' is not a parameter type. Use Float, Int, Bool, Vec2, Vec3, Vec4 or Color.", In.Type));
                    }

                    const nlohmann::json Value = nlohmann::json::parse(In.Value.c_str(), In.Value.c_str() + In.Value.size(), nullptr, false);
                    if (Value.is_discarded() || !ReadParameterValue(Value, Param, Error))
                    {
                        return Agent::FToolResult::Error(Value.is_discarded() ? FString("Value is not JSON.") : Error);
                    }

                    bool bReplaced = false;
                    for (FParticleParameter& Existing : System->UserParameters)
                    {
                        if (Existing.Name == Param.Name)
                        {
                            Existing.CopyFrom(Param);
                            bReplaced = true;
                        }
                    }

                    if (!bReplaced)
                    {
                        System->UserParameters.push_back(Param);
                    }

                    Out.bSucceeded = true;
                    MarkDirty(System);
                    return Agent::FToolResult::Ok(Lumina::Format("{} {} is {}.", In.Type, In.Name, ParameterValueJson(Param).dump()));
                });
        }

        void RegisterRemoveUserParameter(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SRemoveParticleUserParameterParams, SParticleEditResult>(
                Owner, "particle.remove_user_parameter",
                "Remove a named parameter from the system. Inputs bound to it fall back to their constant.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SRemoveParticleUserParameterParams& In, SParticleEditResult& Out)
                {
                    CParticleSystem* System = nullptr;
                    FString Error;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, System, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    const FName Name(In.Name.c_str());
                    for (auto It = System->UserParameters.begin(); It != System->UserParameters.end(); ++It)
                    {
                        if (It->Name == Name)
                        {
                            System->UserParameters.erase(It);
                            Out.bSucceeded = true;
                            MarkDirty(System);
                            return Agent::FToolResult::Ok(Lumina::Format("Removed {}.", In.Name));
                        }
                    }

                    return Agent::FToolResult::Error(Lumina::Format("There is no parameter named {}.", In.Name));
                });
        }

        void RegisterCompile(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SParticleSystemParams, SCompileParticleSystemResult>(
                Owner, "particle.compile",
                "Compile every emitter's module stacks into its shader and report what failed. Save with assets.save.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SParticleSystemParams& In, SCompileParticleSystemResult& Out)
                {
                    CParticleSystem* System = nullptr;
                    FString Error;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, System, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.bSucceeded = true;

                    // Stopping at the first failure would leave the later emitters without a shader.
                    for (int32 Index = 0; Index < (int32)System->Emitters.size(); ++Index)
                    {
                        if (System->Emitters[Index] == nullptr)
                        {
                            continue;
                        }

                        FParticleEmitterCompileOutput Compiled;
                        Out.bSucceeded &= ParticleOps::CompileEmitter(System, Index, Compiled);
                        const FString Prefix = EmitterName(System, Index) + ": ";
                        for (const FString& Item : Compiled.Errors)
                        {
                            Out.Errors.push_back(Prefix + Item);
                        }
                        for (const FString& Item : Compiled.Notes)
                        {
                            Out.Notes.push_back(Prefix + Item);
                        }
                    }

                    MarkDirty(System);
                    return Out.bSucceeded
                        ? Agent::FToolResult::Ok(Lumina::Format("Compiled {} emitter(s).", System->Emitters.size()))
                        : Agent::FToolResult::Ok(Lumina::Format("{} error(s) while compiling.", Out.Errors.size()));
                });
        }
    }

    void RegisterParticleTools(FStringView Owner)
    {
        ParticleTools::RegisterListModuleTypes(Owner);
        ParticleTools::RegisterDescribe(Owner);
        ParticleTools::RegisterCreate(Owner);
        ParticleTools::RegisterAddEmitter(Owner);
        ParticleTools::RegisterRemoveEmitter(Owner);
        ParticleTools::RegisterSetEmitterProperty(Owner);
        ParticleTools::RegisterAddModule(Owner);
        ParticleTools::RegisterRemoveModule(Owner);
        ParticleTools::RegisterMoveModule(Owner);
        ParticleTools::RegisterSetModuleProperty(Owner);
        ParticleTools::RegisterSetUserParameter(Owner);
        ParticleTools::RegisterRemoveUserParameter(Owner);
        ParticleTools::RegisterCompile(Owner);
    }
}
