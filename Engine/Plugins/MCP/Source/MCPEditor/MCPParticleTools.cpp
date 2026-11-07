#include "MCPParticleTools.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentEntityToken.h"
#include "Agent/AgentObjectEdit.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Agent/AgentToolSchema.h"
#include "Asset/AssetOps.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Assets/Factories/Factory.h"
#include "Containers/Algorithm.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "MCPGraphTools.h"
#include "MCPServer.h"
#include "MCPTextMatch.h"
#include "Particles/ParticleGraphModules.h"
#include "Particles/ParticleOps.h"
#include "Session/SessionOps.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/ParticleSystemComponent.h"

namespace Lumina::MCP
{
    namespace MCPParticleToolsInternal
    {
        enum class EParticleAccess : uint8
        {
            Read,
            Write,
        };

        // Held keeps a wrapped emitter asset's transient system alive for the call, since nothing else references it.
        bool ResolveSystem(const FString& Guid, EParticleAccess Access, TStrongObjectPtr<CParticleSystem>& Held, CParticleSystem*& Out, FString& OutError)
        {
            CObject* Asset = nullptr;
            if (!ResolveEditableAsset(FStringView(Guid), Asset, OutError))
            {
                return false;
            }

            CParticleEmitter* EmitterAsset = Cast<CParticleEmitter>(Asset);
            if (EmitterAsset == nullptr && Cast<CParticleSystem>(Asset) == nullptr)
            {
                OutError = Lumina::Format("'{}' is a {}, not a particle system or emitter.", Guid, Asset->GetClass()->GetName());
                return false;
            }

            if (EmitterAsset != nullptr)
            {
                Held = ParticleOps::WrapEmitter(EmitterAsset);
            }
            Out = Held ? Held.Get() : Cast<CParticleSystem>(Asset);

            // The open editor keeps its own list of stacks, which an edit made here would silently diverge from.
            if (Access == EParticleAccess::Write)
            {
                const FString OpenIn = NodeGraphOps::FindOpenEditorName(Asset);
                if (!OpenIn.IsEmpty())
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
            if (Emitter < 0 || Emitter >= System->Emitters.Num() || System->Emitters[Emitter] == nullptr)
            {
                OutError = Lumina::Format("Emitter {} does not exist. This system has {} emitter(s).", Emitter, System->Emitters.Num());
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
                OutError = Lumina::Format("'{}' is not a stage. Use Spawn, Update, Grid or Target.", StageName);
                return false;
            }

            TVector<TStrongObjectPtr<CParticleModule>>* Modules = ParticleOps::GetModules(System, Emitter, OutStage);
            if (Modules == nullptr || Index < 0 || Index >= Modules->Num() || (*Modules)[Index] == nullptr)
            {
                OutError = Lumina::Format("There is no {} module {} on emitter {}.", StageName, Index, Emitter);
                return false;
            }

            OutModule = (*Modules)[Index].Get();
            return true;
        }

        bool ResolveGraph(const FString& SystemId, int32 Emitter, const FString& StageName, int32 Index, EParticleAccess Access,
                          TStrongObjectPtr<CParticleSystem>& Held, CParticleSystem*& System, CParticleNodeGraph*& Graph, FString& OutError)
        {
            EParticleModuleStage Stage;
            CParticleModule* Module = nullptr;
            if (!ResolveSystem(SystemId, Access, Held, System, OutError) || !ResolveModule(System, Emitter, StageName, Index, Stage, Module, OutError))
            {
                return false;
            }

            CParticleModule_Graph* GraphModule = Cast<CParticleModule_Graph>(Module);
            if (GraphModule == nullptr)
            {
                OutError = Lumina::Format("{} module {} is a {}, not a Graph Module.", StageName, Index, Module->GetDisplayName());
                return false;
            }

            Graph = GraphModule->GetGraph();
            return true;
        }

        bool ResolveGraphNodeType(FStringView Name, CClass*& OutClass, FName& OutVariant)
        {
            OutVariant = FName();
            for (CClass* Class : NodeGraphOps::GetPlaceableNodeTypes(CParticleNodeGraph::StaticClass()))
            {
                CEdGraphNode* CDO = Class->GetDefaultObject<CEdGraphNode>();
                TVector<FGraphNodeVariant> Variants;
                CDO->GetVariants(Variants);
                if (Variants.IsEmpty() && (EqualsIgnoreCase(FStringView(*Class->GetName().ToString()), Name) || EqualsIgnoreCase(CDO->GetNodeDisplayName(), Name)))
                {
                    OutClass = Class;
                    return true;
                }

                for (const FGraphNodeVariant& Variant : Variants)
                {
                    if (EqualsIgnoreCase(FStringView(Variant.DisplayName), Name) || EqualsIgnoreCase(FStringView(*Variant.Name.ToString()), Name))
                    {
                        OutClass   = Class;
                        OutVariant = Variant.Name;
                        return true;
                    }
                }
            }
            return false;
        }

        bool ResolveParticleComponent(const FString& Token, SParticleSystemComponent*& OutComponent, FString& OutError)
        {
            ECS::FRegistry* Registry = SessionOps::GetSceneRegistry(OutError);
            ECS::FEntity Entity = ECS::NullEntity;
            if (Registry == nullptr || !Agent::FEntityTokens::Resolve(*Registry, FStringView(Token), Entity, OutError))
            {
                return false;
            }

            OutComponent = Registry->TryGet<SParticleSystemComponent>(Entity);
            if (OutComponent == nullptr)
            {
                OutError = "That entity has no particle system component.";
                return false;
            }
            return true;
        }

        FString EditableValues(CStruct* Type, void* Data)
        {
            FJsonValue Values;
            if (!Agent::WriteStruct(Type, Data, Values).IsValid() || !Values.IsObject())
            {
                return FString();
            }

            for (FProperty* Property : Type->GetProperties())
            {
                if (Property != nullptr && !EnumHasAnyFlags(Property->Flags, EPropertyFlags::Editable))
                {
                    Values.Remove(FStringView(*Property->GetPropertyName().ToString()));
                }
            }

            return Values.ToString();
        }

        void CollectModules(CParticleSystem* System, int32 Emitter, EParticleModuleStage Stage, TVector<SParticleModuleInfo>& Out)
        {
            TVector<TStrongObjectPtr<CParticleModule>>* Modules = ParticleOps::GetModules(System, Emitter, Stage);
            if (Modules == nullptr)
            {
                return;
            }

            for (int32 Index = 0; Index < Modules->Num(); ++Index)
            {
                CParticleModule* Module = (*Modules)[Index].Get();
                if (Module == nullptr)
                {
                    continue;
                }

                SParticleModuleInfo Info;
                Info.Index       = Index;
                Info.Type        = FString(*Module->GetClass()->GetName().ToString());
                Info.DisplayName = Module->GetDisplayName();
                Info.bEnabled    = Module->bEnabled;
                Info.Values      = EditableValues(Module->GetClass(), Module);
                Out.Add(Move(Info));
            }
        }

        void ClearStacks(CParticleSystem* System, int32 Emitter)
        {
            for (EParticleModuleStage Stage : ParticleOps::GetStages())
            {
                if (TVector<TStrongObjectPtr<CParticleModule>>* Modules = ParticleOps::GetModules(System, Emitter, Stage))
                {
                    Modules->Reset();
                }
            }
        }

        // A shared emitter asset lives in its own package, and an emitter asset's wrapping system has none.
        void MarkSystemDirty(CParticleSystem* System, int32 Emitter = Constants::kIndexNone)
        {
            MarkAssetDirty(System);
            if (Emitter >= 0 && Emitter < System->Emitters.Num())
            {
                MarkAssetDirty(System->Emitters[Emitter].Get());
            }
        }

        FString EmitterName(CParticleSystem* System, int32 Emitter)
        {
            return System->Emitters[Emitter]->EmitterName;
        }

        // bPostLoad rebinds render state such as a swapped material or texture.
        bool ApplyPropertyJson(CObject* Object, const FString& Path, const FString& ValueText, bool bPostLoad, SParticlePropertyResult& Out, FString& OutError)
        {
            FJsonValue Value;
            if (!FJsonValue::Parse(ValueText, Value))
            {
                OutError = "Value is not JSON. Strings need quotes, so a quoted name rather than a bare word.";
                return false;
            }

            const Agent::FObjectEditResult Edit = Agent::SetObjectProperty(
                Object->GetClass(), Object, Object, FStringView(Path), Value, "Set Particle Property (agent)");
            if (!Edit.IsValid())
            {
                OutError = Edit.Error;
                return false;
            }

            Out.Previous = Edit.Previous;
            Out.Current  = Edit.Current;
            if (bPostLoad)
            {
                Object->PostLoad();
            }
            return true;
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

        FJsonValue ParameterValueJson(const SParticleParameter& Param)
        {
            switch (Param.Type)
            {
                case EParticleParameterType::Float: return Param.Scalar;
                case EParticleParameterType::Int:   return Param.Integer;
                case EParticleParameterType::Bool:  return Param.Boolean;
                default:
                    return FJsonValue::MakeObject({ { "X", Param.Vector.x }, { "Y", Param.Vector.y }, { "Z", Param.Vector.z }, { "W", Param.Vector.w } });
            }
        }

        bool ReadParameterValue(const FJsonValue& Value, SParticleParameter& Param, FString& OutError)
        {
            switch (Param.Type)
            {
                case EParticleParameterType::Float:
                    if (!Value.IsNumber())
                    {
                        OutError = "A Float parameter takes a number.";
                        return false;
                    }
                    Param.Scalar = Value.AsFloat();
                    return true;
                case EParticleParameterType::Int:
                    if (!Value.IsNumber())
                    {
                        OutError = "An Int parameter takes a number.";
                        return false;
                    }
                    Param.Integer = Value.AsInt32();
                    return true;
                case EParticleParameterType::Bool:
                    if (!Value.IsBool())
                    {
                        OutError = "A Bool parameter takes true or false.";
                        return false;
                    }
                    Param.Boolean = Value.AsBool();
                    return true;
                default:
                    if (!Value.IsObject())
                    {
                        OutError = "A vector or color parameter takes an object with X, Y, Z and W.";
                        return false;
                    }
                    for (const FJsonMember& Member : Value.Members())
                    {
                        if ((Member.Key == "X" || Member.Key == "Y" || Member.Key == "Z" || Member.Key == "W") && !Member.Value.IsNumber())
                        {
                            OutError = "A vector or color parameter takes an object with X, Y, Z and W.";
                            return false;
                        }
                    }
                    Param.Vector = FVector4(Value.GetValue("X", 0.0f), Value.GetValue("Y", 0.0f), Value.GetValue("Z", 0.0f), Value.GetValue("W", Param.Type == EParticleParameterType::Color ? 1.0f : 0.0f));
                    return true;
            }
        }

        Agent::FToolResult AddEmitter(CParticleSystem* System, const SParticleEmitterParams& In, SParticleEmitterResult& Out)
        {
            CParticleEmitter* Emitter = System->AddEmitter();
            if (Emitter == nullptr)
            {
                return Agent::FToolResult::Error("The system refused to add an emitter.");
            }

            if (!In.Name.IsEmpty())
            {
                Emitter->EmitterName = In.Name;
            }

            Out.Index = System->Emitters.Num() - 1;
            Out.Name  = Emitter->EmitterName;
            ParticleOps::FindOrCreateStack(System, Out.Index);
            if (!In.bStarterStack)
            {
                ClearStacks(System, Out.Index);
            }

            MarkSystemDirty(System);
            return Agent::FToolResult::Ok(Lumina::Format("Added emitter {} '{}'. Compile before it draws.", Out.Index, Out.Name));
        }

        Agent::FToolResult AddEmitterAsset(CParticleSystem* System, const SParticleEmitterParams& In, SParticleEmitterResult& Out)
        {
            CObject* Asset = nullptr;
            FString Error;
            if (!Agent::ResolveAssetObject(FStringView(In.Asset), Asset, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            CParticleEmitter* Emitter = Cast<CParticleEmitter>(Asset);
            if (Emitter == nullptr || !Emitter->IsAsset())
            {
                return Agent::FToolResult::Error(Lumina::Format("'{}' is not a particle emitter asset.", In.Asset));
            }

            System->Emitters.Add(Emitter);
            Out.Index = System->Emitters.Num() - 1;
            Out.Name  = Emitter->EmitterName;
            MarkSystemDirty(System);
            return Agent::FToolResult::Ok(Lumina::Format("Shared emitter asset '{}' as emitter {}.", *Emitter->GetName(), Out.Index));
        }

        Agent::FToolResult RemoveEmitter(CParticleSystem* System, const SParticleEmitterParams& In, SParticleEmitterResult& Out)
        {
            if (!System->RemoveEmitter(System->Emitters[In.Emitter].Get()))
            {
                return Agent::FToolResult::Error("That is the only emitter, and a system needs at least one.");
            }

            Out.bSucceeded = true;
            MarkSystemDirty(System);
            return Agent::FToolResult::Ok(Lumina::Format("Removed emitter {}.", In.Emitter));
        }

        Agent::FToolResult MakeEmitterLocal(CParticleSystem* System, const SParticleEmitterParams& In, SParticleEmitterResult& Out)
        {
            if (System->Emitters[In.Emitter]->GetPackage() == System->GetPackage())
            {
                return Agent::FToolResult::Error(Lumina::Format("Emitter {} already belongs to the system.", In.Emitter));
            }

            Out.bSucceeded = ParticleOps::MakeEmitterLocal(System, In.Emitter) != nullptr;
            MarkSystemDirty(System);
            return Out.bSucceeded ? Agent::FToolResult::Ok(Lumina::Format("Emitter {} is now the system's own copy.", In.Emitter))
                                  : Agent::FToolResult::Error("The copy failed.");
        }

        Agent::FToolResult SaveEmitterAsAsset(CParticleSystem* System, const SParticleEmitterParams& In, SParticleEmitterResult& Out)
        {
            FString Error;
            if (!IsWellFormedAssetPath(In.Path, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            if (!AssetOps::IsAssetLocation(FStringView(In.Path)))
            {
                return Agent::FToolResult::Error("Assets belong under /Game/Content, since nothing scans for assets elsewhere.");
            }

            CParticleEmitter* Asset = ParticleOps::SaveEmitterAsAsset(System, In.Emitter, FStringView(*In.Path, In.Path.Len()));
            if (Asset == nullptr)
            {
                return Agent::FToolResult::Error(Lumina::Format("Could not save emitter {} to '{}'.", In.Emitter, In.Path));
            }

            Out.Guid = FString(*Asset->GetGUID().ToString());
            Out.Path = FString(*Asset->GetPackage()->GetPackagePath());
            MarkSystemDirty(System);
            return Agent::FToolResult::Ok(Lumina::Format("Saved emitter {} as {}.", In.Emitter, Out.Path));
        }

        Agent::FToolResult EmitterTool(const SParticleEmitterParams& In, SParticleEmitterResult& Out)
        {
            TStrongObjectPtr<CParticleSystem> Held;
            CParticleSystem* System = nullptr;
            FString Error;
            if (!ResolveSystem(In.System, EParticleAccess::Write, Held, System, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            const bool bNeedsEmitter = In.Action != EParticleEmitterAction::Add && In.Action != EParticleEmitterAction::AddAsset;
            if (bNeedsEmitter && !CheckEmitter(System, In.Emitter, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            switch (In.Action)
            {
                case EParticleEmitterAction::Add:         return AddEmitter(System, In, Out);
                case EParticleEmitterAction::AddAsset:    return AddEmitterAsset(System, In, Out);
                case EParticleEmitterAction::Remove:      return RemoveEmitter(System, In, Out);
                case EParticleEmitterAction::MakeLocal:   return MakeEmitterLocal(System, In, Out);
                case EParticleEmitterAction::SaveAsAsset: return SaveEmitterAsAsset(System, In, Out);
            }
            return Agent::FToolResult::Error("Unknown emitter action.");
        }

        Agent::FToolResult AddModule(CParticleSystem* System, const SParticleModuleParams& In, SParticleModuleResult& Out)
        {
            FString Error;
            if (!CheckEmitter(System, In.Emitter, Error))
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
            for (int32 Index = 0; Modules != nullptr && Index < Modules->Num(); ++Index)
            {
                if ((*Modules)[Index].Get() == Module)
                {
                    Out.Index = Index;
                }
            }

            Out.Stage  = ParticleOps::StageName(Stage);
            Out.Values = EditableValues(Module->GetClass(), Module);
            MarkSystemDirty(System, In.Emitter);
            return Agent::FToolResult::Ok(Lumina::Format("Added {} as {} module {} on '{}'. Compile before it takes effect.",
                Module->GetDisplayName(), Out.Stage, Out.Index, EmitterName(System, In.Emitter)));
        }

        Agent::FToolResult RemoveModule(CParticleSystem* System, const SParticleModuleParams& In, SParticleModuleResult& Out)
        {
            FString Error;
            EParticleModuleStage Stage;
            CParticleModule* Module = nullptr;
            if (!ResolveModule(System, In.Emitter, In.Stage, In.Index, Stage, Module, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            Out.bSucceeded = ParticleOps::RemoveModule(System, In.Emitter, Stage, In.Index);
            MarkSystemDirty(System, In.Emitter);
            return Agent::FToolResult::Ok(Lumina::Format("Removed {} module {}. Compile before it takes effect.", In.Stage, In.Index));
        }

        Agent::FToolResult MoveModule(CParticleSystem* System, const SParticleModuleParams& In, SParticleModuleResult& Out)
        {
            FString Error;
            EParticleModuleStage Stage;
            CParticleModule* Module = nullptr;
            if (!ResolveModule(System, In.Emitter, In.Stage, In.Index, Stage, Module, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            if (!ParticleOps::MoveModule(System, In.Emitter, Stage, In.Index, In.NewIndex))
            {
                return Agent::FToolResult::Error(Lumina::Format("{} is not a position in that stack.", In.NewIndex));
            }

            Out.bSucceeded = true;
            MarkSystemDirty(System, In.Emitter);
            return Agent::FToolResult::Ok(Lumina::Format("Moved {} module {} to {}. Compile before it takes effect.", In.Stage, In.Index, In.NewIndex));
        }

        Agent::FToolResult ModuleTool(const SParticleModuleParams& In, SParticleModuleResult& Out)
        {
            TStrongObjectPtr<CParticleSystem> Held;
            CParticleSystem* System = nullptr;
            FString Error;
            if (!ResolveSystem(In.System, EParticleAccess::Write, Held, System, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            switch (In.Action)
            {
                case EParticleModuleAction::Add:    return AddModule(System, In, Out);
                case EParticleModuleAction::Remove: return RemoveModule(System, In, Out);
                case EParticleModuleAction::Move:   return MoveModule(System, In, Out);
            }
            return Agent::FToolResult::Error("Unknown module action.");
        }

        Agent::FToolResult SetUserParameter(CParticleSystem* System, const SParticleUserParameterParams& In, SParticleEditResult& Out)
        {
            if (In.Name.IsEmpty())
            {
                return Agent::FToolResult::Error("A parameter needs a name.");
            }

            SParticleParameter Param;
            Param.Name = FName(*In.Name);
            if (!ParseParameterType(In.Type, Param.Type))
            {
                return Agent::FToolResult::Error(Lumina::Format("'{}' is not a parameter type. Use Float, Int, Bool, Vec2, Vec3, Vec4 or Color.", In.Type));
            }

            FJsonValue Value;
            FString Error;
            const bool bParsed = FJsonValue::Parse(In.Value, Value);
            if (!bParsed || !ReadParameterValue(Value, Param, Error))
            {
                return Agent::FToolResult::Error(!bParsed ? FString("Value is not JSON.") : Error);
            }

            bool bReplaced = false;
            for (SParticleParameter& Existing : System->UserParameters)
            {
                if (Existing.Name == Param.Name)
                {
                    Existing.CopyFrom(Param);
                    bReplaced = true;
                }
            }

            if (!bReplaced)
            {
                System->UserParameters.Add(Param);
            }

            Out.bSucceeded = true;
            MarkSystemDirty(System);
            return Agent::FToolResult::Ok(Lumina::Format("{} {} is {}.", In.Type, In.Name, ParameterValueJson(Param).ToString()));
        }

        Agent::FToolResult RemoveUserParameter(CParticleSystem* System, const SParticleUserParameterParams& In, SParticleEditResult& Out)
        {
            const FName Name(*In.Name);
            for (int32 i = 0; i < System->UserParameters.Num(); ++i)
            {
                if (System->UserParameters[i].Name == Name)
                {
                    System->UserParameters.RemoveAt(i);
                    Out.bSucceeded = true;
                    MarkSystemDirty(System);
                    return Agent::FToolResult::Ok(Lumina::Format("Removed {}.", In.Name));
                }
            }

            return Agent::FToolResult::Error(Lumina::Format("There is no parameter named {}.", In.Name));
        }

        Agent::FToolResult UserParameterTool(const SParticleUserParameterParams& In, SParticleEditResult& Out)
        {
            TStrongObjectPtr<CParticleSystem> Held;
            CParticleSystem* System = nullptr;
            FString Error;
            if (!ResolveSystem(In.System, EParticleAccess::Write, Held, System, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            switch (In.Action)
            {
                case EParticleUserParameterAction::Set:    return SetUserParameter(System, In, Out);
                case EParticleUserParameterAction::Remove: return RemoveUserParameter(System, In, Out);
            }
            return Agent::FToolResult::Error("Unknown user parameter action.");
        }

        void RegisterParticleListModuleTypes(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListParticleModuleTypesParams, SListParticleModuleTypesResult>(
                Owner, "particle.list_module_types",
                "List every module type that can go in an emitter's Spawn, Update, Grid or Target stack.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListParticleModuleTypesParams& In, SListParticleModuleTypesResult& Out)
                {
                    for (CClass* Class : ParticleOps::GetModuleTypes())
                    {
                        CParticleModule* CDO = Class->GetDefaultObject<CParticleModule>();
                        SParticleModuleTypeInfo Info;
                        Info.Name        = FString(*Class->GetName().ToString());
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
                                Info.ParameterSchema = Schema.Schema.ToString();
                            }
                        }

                        Out.Types.Add(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} particle module type(s).", Out.Types.Num()));
                });
        }

        void RegisterParticleDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SParticleSystemParams, SDescribeParticleSystemResult>(
                Owner, "particle.describe",
                "Report a particle system's user parameters, emitters, their settings and every module stack with each module's values.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SParticleSystemParams& In, SDescribeParticleSystemResult& Out)
                {
                    TStrongObjectPtr<CParticleSystem> Held;
                    CParticleSystem* System = nullptr;
                    FString Error;
                    if (!ResolveSystem(In.System, EParticleAccess::Read, Held, System, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Name = FString(*System->GetName().ToString());
                    for (const SParticleParameter& Param : System->UserParameters)
                    {
                        SParticleUserParameterInfo Info;
                        Info.Name  = FString(*Param.Name.ToString());
                        Info.Type  = ParameterTypeNames[(int32)Param.Type];
                        Info.Value = ParameterValueJson(Param).ToString();
                        Out.UserParameters.Add(Move(Info));
                    }

                    for (int32 Index = 0; Index < System->Emitters.Num(); ++Index)
                    {
                        CParticleEmitter* Emitter = System->Emitters[Index].Get();
                        if (Emitter == nullptr)
                        {
                            continue;
                        }

                        SParticleEmitterInfo Info;
                        Info.Index       = Index;
                        Info.Name        = Emitter->EmitterName;
                        Info.bCompiled   = Emitter->HasCustomComputeShader();
                        Info.SharedAsset = Emitter->GetPackage() != System->GetPackage() ? FString(*Emitter->GetPackage()->GetPackagePath()) : FString();
                        Info.Settings    = EditableValues(Emitter->GetClass(), Emitter);
                        CollectModules(System, Index, EParticleModuleStage::Spawn, Info.SpawnModules);
                        CollectModules(System, Index, EParticleModuleStage::Update, Info.UpdateModules);
                        CollectModules(System, Index, EParticleModuleStage::Grid, Info.GridModules);
                        CollectModules(System, Index, EParticleModuleStage::Target, Info.TargetModules);
                        Out.Emitters.Add(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("'{}' has {} emitter(s).", Out.Name, Out.Emitters.Num()));
                });
        }

        void RegisterParticleCreate(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateParticleSystemParams, SCreateParticleSystemResult>(
                Owner, "particle.create",
                "Create a particle system asset with one emitter, compiled and saved so it can be placed right away.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateParticleSystemParams& In, SCreateParticleSystemResult& Out)
                {
                    FFixedString Path;
                    FString Error;
                    if (!MakeNewAssetPath(In.Folder, In.Name, Path, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    // Held strongly, since the export table only watches it and a compile job releasing its ref would free it.
                    const TStrongObjectPtr<CParticleSystem> System(CFactoryRegistry::Get().FindFactory(CParticleSystem::StaticClass())->TryCreateNew<CParticleSystem>(FStringView(*Path, Path.Len())));
                    if (!System || System->Emitters.IsEmpty())
                    {
                        if (System)
                        {
                            CFactory::DiscardNew(System.Get());
                        }
                        return Agent::FToolResult::Error(Lumina::Format("Could not create a particle system at {}.", Path));
                    }

                    ParticleOps::FindOrCreateStack(System.Get(), 0);
                    if (!In.bStarterStack)
                    {
                        ClearStacks(System.Get(), 0);
                    }

                    FParticleEmitterCompileOutput Compiled;
                    if (In.bStarterStack)
                    {
                        ParticleOps::CompileEmitter(System.Get(), 0, Compiled);
                    }

                    if (!CPackage::SavePackage(System->GetPackage(), Path))
                    {
                        CFactory::DiscardNew(System.Get());
                        return Agent::FToolResult::Error(Lumina::Format("Could not save {}.", Path));
                    }

                    FAssetRegistry::Get().AssetCreated(System.Get());
                    Out.Path = FString(*Path);
                    Out.Guid = FString(*System->GetGUID().ToString());
                    return Agent::FToolResult::Ok(Lumina::Format("Created {}.", Out.Path));
                });
        }

        void RegisterParticleCompile(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SParticleSystemParams, SCompileParticleSystemResult>(
                Owner, "particle.compile",
                "Compile every emitter's module stacks into its shader and report what failed. Save with assets.save.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SParticleSystemParams& In, SCompileParticleSystemResult& Out)
                {
                    TStrongObjectPtr<CParticleSystem> Held;
                    CParticleSystem* System = nullptr;
                    FString Error;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, Held, System, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.bSucceeded = true;

                    // Stopping at the first failure would leave the later emitters without a shader.
                    for (int32 Index = 0; Index < System->Emitters.Num(); ++Index)
                    {
                        if (System->Emitters[Index] == nullptr)
                        {
                            continue;
                        }

                        FParticleEmitterCompileOutput Compiled;
                        Out.bSucceeded &= ParticleOps::CompileEmitter(System, Index, Compiled);
                        MarkSystemDirty(System, Index);
                        const FString Prefix = EmitterName(System, Index) + ": ";
                        for (const FString& Item : Compiled.Errors)
                        {
                            Out.Errors.Add(Prefix + Item);
                        }
                        for (const FString& Item : Compiled.Notes)
                        {
                            Out.Notes.Add(Prefix + Item);
                        }
                    }

                    return Out.bSucceeded
                        ? Agent::FToolResult::Ok(Lumina::Format("Compiled {} emitter(s).", System->Emitters.Num()))
                        : Agent::FToolResult::Ok(Lumina::Format("{} error(s) while compiling.", Out.Errors.Num()));
                });
        }

        void RegisterParticleEmitter(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SParticleEmitterParams, SParticleEmitterResult>(
                Owner, "particle.emitter",
                "Edit a system's emitters: Add a new one, AddAsset to share an emitter asset, Remove one (never the last), "
                "MakeLocal to copy a shared asset into the system, or SaveAsAsset to save one as a shared asset.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                &EmitterTool);
        }

        void RegisterParticleSetEmitterProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetParticleEmitterPropertyParams, SParticlePropertyResult>(
                Owner, "particle.set_emitter_property",
                "Set one emitter setting, such as SpawnRate, BurstCount, MaxParticles, Duration, bLooping, BlendMode or Material. Takes effect without a compile.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetParticleEmitterPropertyParams& In, SParticlePropertyResult& Out)
                {
                    TStrongObjectPtr<CParticleSystem> Held;
                    CParticleSystem* System = nullptr;
                    FString Error;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, Held, System, Error) || !CheckEmitter(System, In.Emitter, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (!ApplyPropertyJson(System->Emitters[In.Emitter].Get(), In.Path, In.Value, true, Out, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    MarkSystemDirty(System, In.Emitter);
                    return Agent::FToolResult::Ok(Lumina::Format("Emitter {} {} is now {}.", In.Emitter, In.Path, Out.Current));
                });
        }

        void RegisterParticleModule(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SParticleModuleParams, SParticleModuleResult>(
                Owner, "particle.module",
                "Add, Remove or Move a module in an emitter's stack. Add takes ModuleType, which picks the stage; Remove and Move "
                "take Stage and Index. Modules run top to bottom. Compile afterwards.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                &ModuleTool);
        }

        void RegisterParticleSetModuleProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetParticleModulePropertyParams, SParticlePropertyResult>(
                Owner, "particle.set_module_property",
                "Set one module input, such as Color.Constant, or bind it to a user parameter via Color.ParameterName. "
                "Applies live on a compiled emitter unless bNeedsCompile is true.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetParticleModulePropertyParams& In, SParticlePropertyResult& Out)
                {
                    TStrongObjectPtr<CParticleSystem> Held;
                    CParticleSystem* System = nullptr;
                    FString Error;
                    EParticleModuleStage Stage;
                    CParticleModule* Module = nullptr;
                    if (!ResolveSystem(In.System, EParticleAccess::Write, Held, System, Error)
                        || !ResolveModule(System, In.Emitter, In.Stage, In.Index, Stage, Module, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (!ApplyPropertyJson(Module, In.Path, In.Value, false, Out, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    // An enum picks a code branch rather than a slot value, which only a recompile can change.
                    Out.bNeedsCompile = !ParticleOps::RefreshModuleParams(System, In.Emitter);
                    MarkSystemDirty(System, In.Emitter);
                    return Agent::FToolResult::Ok(Lumina::Format("{} module {} {} is now {}{}", In.Stage, In.Index, In.Path, Out.Current,
                        Out.bNeedsCompile ? ". Compile before it takes effect." : ", live."));
                });
        }

        void RegisterParticleUserParameter(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SParticleUserParameterParams, SParticleEditResult>(
                Owner, "particle.user_parameter",
                "Set declares or changes a named system parameter; Remove deletes it, and inputs bound to it fall back to their constant. "
                "Bind an input by setting its ParameterName with particle.set_module_property.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                &UserParameterTool);
        }

        void RegisterParticleGraphDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SParticleGraphParams, SParticleGraphResult>(
                Owner, "particle.graph_describe",
                "Report a Graph Module's nodes, pins and links, and every node type it can take.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SParticleGraphParams& In, SParticleGraphResult& Out)
                {
                    TStrongObjectPtr<CParticleSystem> Held;
                    CParticleSystem* System = nullptr;
                    CParticleNodeGraph* Graph = nullptr;
                    FString Error;
                    if (!ResolveGraph(In.System, In.Emitter, In.Stage, In.Index, EParticleAccess::Read, Held, System, Graph, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    for (const TStrongObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
                    {
                        SGraphNodeInfo& Info = Out.Nodes.EmplaceGetRef();
                        Info.Id          = Node->GetNodeID();
                        Info.Type        = FString(*Node->GetClass()->GetName().ToString());
                        Info.DisplayName = FString(Node->GetNodeDisplayName());
                        Info.X           = Node->GetNodeX();
                        Info.Y           = Node->GetNodeY();
                        Info.Values      = EditableValues(Node->GetClass(), Node.Get());
                        CollectPins(Node.Get(), Info.Pins);
                    }

                    for (CClass* Class : NodeGraphOps::GetPlaceableNodeTypes(CParticleNodeGraph::StaticClass()))
                    {
                        TVector<FGraphNodeVariant> Variants;
                        CEdGraphNode* CDO = Class->GetDefaultObject<CEdGraphNode>();
                        CDO->GetVariants(Variants);
                        if (Variants.IsEmpty())
                        {
                            Out.NodeTypes.Emplace(CDO->GetNodeDisplayName());
                        }
                        for (const FGraphNodeVariant& Variant : Variants)
                        {
                            Out.NodeTypes.Add(Variant.DisplayName);
                        }
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} node(s), {} node type(s).", Out.Nodes.Num(), Out.NodeTypes.Num()));
                });
        }

        void RegisterParticleGraphAddNode(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddParticleGraphNodeParams, SAddParticleGraphNodeResult>(
                Owner, "particle.graph_add_node",
                "Add a node to a Graph Module and report its pins. Sinks such as Set Particle write once every sink has read its inputs, top to bottom.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddParticleGraphNodeParams& In, SAddParticleGraphNodeResult& Out)
                {
                    TStrongObjectPtr<CParticleSystem> Held;
                    CParticleSystem* System = nullptr;
                    CParticleNodeGraph* Graph = nullptr;
                    FString Error;
                    if (!ResolveGraph(In.System, In.Emitter, In.Stage, In.Index, EParticleAccess::Write, Held, System, Graph, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CClass* NodeClass = nullptr;
                    FName Variant;
                    if (!ResolveGraphNodeType(FStringView(In.NodeType), NodeClass, Variant))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("'{}' is not a particle graph node. particle.graph_describe lists them.", In.NodeType));
                    }

                    CEdGraphNode* Node = NodeGraphOps::AddNode(Graph, NodeClass, In.X, In.Y, Variant);
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error("The node could not be created.");
                    }

                    Out.Id = Node->GetNodeID();
                    CollectPins(Node, Out.Pins);
                    MarkSystemDirty(System, In.Emitter);
                    return Agent::FToolResult::Ok(Lumina::Format("Added {} as node {}. Compile to run it.", Node->GetNodeDisplayName(), Out.Id));
                });
        }

        void RegisterParticleGraphConnect(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SConnectParticleGraphParams, SParticleEditResult>(
                Owner, "particle.graph_connect",
                "Wire one node's output pin into another node's input pin in a Graph Module.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SConnectParticleGraphParams& In, SParticleEditResult& Out)
                {
                    TStrongObjectPtr<CParticleSystem> Held;
                    CParticleSystem* System = nullptr;
                    CParticleNodeGraph* Graph = nullptr;
                    FString Error;
                    if (!ResolveGraph(In.System, In.Emitter, In.Stage, In.Index, EParticleAccess::Write, Held, System, Graph, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CEdGraphNode* From = NodeGraphOps::FindNode(Graph, In.FromNode);
                    CEdGraphNode* To   = NodeGraphOps::FindNode(Graph, In.ToNode);
                    if (From == nullptr || To == nullptr)
                    {
                        return Agent::FToolResult::Error("Both nodes have to be in this graph.");
                    }

                    CEdNodeGraphPin* Output = NodeGraphOps::FindPin(From, FStringView(In.FromPin), ENodePinDirection::Output);
                    CEdNodeGraphPin* Input  = NodeGraphOps::FindPin(To, FStringView(In.ToPin), ENodePinDirection::Input);
                    if (Output == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No output '{}'. It has {}.", In.FromPin, NodeGraphOps::DescribePinNames(From, ENodePinDirection::Output)));
                    }
                    if (Input == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No input '{}'. It has {}.", In.ToPin, NodeGraphOps::DescribePinNames(To, ENodePinDirection::Input)));
                    }
                    if (!NodeGraphOps::ConnectPins(Graph, Output, Input, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    MarkSystemDirty(System, In.Emitter);
                    return Agent::FToolResult::Ok("Connected. Compile to run it.");
                });
        }

        void RegisterParticleGraphSetNodeProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetParticleGraphNodePropertyParams, SParticlePropertyResult>(
                Owner, "particle.graph_set_node_property",
                "Set one field on a Graph Module's node, such as a Parameter's Value.Constant or a grid node's Channel.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetParticleGraphNodePropertyParams& In, SParticlePropertyResult& Out)
                {
                    TStrongObjectPtr<CParticleSystem> Held;
                    CParticleSystem* System = nullptr;
                    CParticleNodeGraph* Graph = nullptr;
                    FString Error;
                    if (!ResolveGraph(In.System, In.Emitter, In.Stage, In.Index, EParticleAccess::Write, Held, System, Graph, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CEdGraphNode* Node = NodeGraphOps::FindNode(Graph, In.Node);
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No node {} is in this graph.", In.Node));
                    }

                    if (!ApplyPropertyJson(Node, In.Path, In.Value, false, Out, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.bNeedsCompile = !ParticleOps::RefreshModuleParams(System, In.Emitter);
                    MarkSystemDirty(System, In.Emitter);
                    return Agent::FToolResult::Ok(Lumina::Format("Node {} {} is now {}{}", In.Node, In.Path, Out.Current,
                        Out.bNeedsCompile ? ". Compile before it takes effect." : ", live."));
                });
        }

        void RegisterParticleSetArray(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetParticleArrayParams, SParticleEditResult>(
                Owner, "particle.set_array",
                "Hand an entity's particle system a named array of float4 values, as a script's SetArray does, from the next frame on.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetParticleArrayParams& In, SParticleEditResult& Out)
                {
                    SParticleSystemComponent* Component = nullptr;
                    FString Error;
                    if (!ResolveParticleComponent(In.Entity, Component, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Component->SetArray(FName(*In.Name), In.Values);
                    return Agent::FToolResult::Ok(Lumina::Format("{} now holds {} value(s).", In.Name, In.Values.Num()));
                });
        }

        void RegisterParticleSampleGrid(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSampleParticleGridParams, SSampleParticleGridResult>(
                Owner, "particle.sample_grid",
                "Read a grid channel at a world position from the CPU copy an emitter with Read Back Grid sends, as a script's SampleGrid does.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SSampleParticleGridParams& In, SSampleParticleGridResult& Out)
                {
                    SParticleSystemComponent* Component = nullptr;
                    FString Error;
                    if (!ResolveParticleComponent(In.Entity, Component, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    const auto Snapshot = Algo::FindIf(Component->GridSnapshots, [&](const FParticleGridSnapshot& Candidate) { return Candidate.EmitterIndex == In.Emitter; });
                    Out.bHasSnapshot = Snapshot != Component->GridSnapshots.end();
                    if (Out.bHasSnapshot)
                    {
                        for (const FName& Channel : Snapshot->ChannelNames)
                        {
                            Out.Channels.Emplace(*Channel.ToString());
                        }
                    }
                    Out.Value = Component->SampleGrid(In.Emitter, FName(*In.Channel), In.Position);
                    return Agent::FToolResult::Ok(Out.bHasSnapshot ? Lumina::Format("{} is {}.", In.Channel, Out.Value)
                                                                   : FString("No grid copy has come back for that emitter yet."));
                });
        }
    }

    using namespace MCPParticleToolsInternal;

    void RegisterParticleTools(FStringView Owner)
    {
        RegisterParticleListModuleTypes(Owner);
        RegisterParticleDescribe(Owner);
        RegisterParticleCreate(Owner);
        RegisterParticleCompile(Owner);
        RegisterParticleEmitter(Owner);
        RegisterParticleSetEmitterProperty(Owner);
        RegisterParticleModule(Owner);
        RegisterParticleSetModuleProperty(Owner);
        RegisterParticleUserParameter(Owner);
        RegisterParticleGraphDescribe(Owner);
        RegisterParticleGraphAddNode(Owner);
        RegisterParticleGraphConnect(Owner);
        RegisterParticleGraphSetNodeProperty(Owner);
        RegisterParticleSetArray(Owner);
        RegisterParticleSampleGrid(Owner);
    }
}
