#include "ParticleEmitterStack.h"
#include "ParticleStockModules.h"
#include "ParticleGridModules.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"

namespace Lumina
{
    void CParticleEmitterStack::CompileStacks(FParticleCompiler& Compiler)
    {
        int32 ModuleIndex = 0;

        for (const TStrongObjectPtr<CParticleModule>& Module : SpawnModules)
        {
            if (Module.IsValid() && Module->bEnabled)
            {
                Module->Generate(Compiler, ModuleIndex);
            }
            ++ModuleIndex;
        }

        for (const TStrongObjectPtr<CParticleModule>& Module : UpdateModules)
        {
            if (Module.IsValid() && Module->bEnabled)
            {
                Module->Generate(Compiler, ModuleIndex);
            }
            ++ModuleIndex;
        }

        // Turning a stage off turns off every module under it, up to the next stage.
        bool bStageEnabled = true;
        for (const TStrongObjectPtr<CParticleModule>& Module : GridModules)
        {
            if (const CParticleModule_SimulationStage* Stage = Cast<CParticleModule_SimulationStage>(Module.Get()))
            {
                bStageEnabled = Stage->bEnabled;
                if (bStageEnabled)
                {
                    Compiler.BeginGridStage(Stage->Iterations);
                }
            }
            else if (Module.IsValid() && Module->bEnabled && bStageEnabled)
            {
                Module->Generate(Compiler, ModuleIndex);
            }
            ++ModuleIndex;
        }

        for (const TStrongObjectPtr<CParticleModule>& Module : TargetModules)
        {
            if (Module.IsValid() && Module->bEnabled)
            {
                Module->Generate(Compiler, ModuleIndex);
            }
            ++ModuleIndex;
        }
    }

    TVector<TStrongObjectPtr<CParticleModule>>& CParticleEmitterStack::GetStack(EParticleModuleStage Stage)
    {
        switch (Stage)
        {
        case EParticleModuleStage::Spawn: return SpawnModules;
        case EParticleModuleStage::Grid:  return GridModules;
        case EParticleModuleStage::Target: return TargetModules;
        default:                          return UpdateModules;
        }
    }

    CParticleModule* CParticleEmitterStack::AddModule(CClass* ModuleClass)
    {
        if (ModuleClass == nullptr || !ModuleClass->IsChildOf(CParticleModule::StaticClass()))
        {
            return nullptr;
        }

        CParticleModule* Module = NewObject<CParticleModule>(ModuleClass, GetPackage());
        if (Module == nullptr)
        {
            return nullptr;
        }

        GetStack(Module->GetStage()).push_back(Module);
        return Module;
    }

    void CParticleEmitterStack::RemoveModule(CParticleModule* Module)
    {
        if (Module == nullptr)
        {
            return;
        }

        TVector<TStrongObjectPtr<CParticleModule>>& Stack = GetStack(Module->GetStage());
        for (auto It = Stack.begin(); It != Stack.end(); ++It)
        {
            if (It->Get() == Module)
            {
                Stack.erase(It);
                return;
            }
        }
    }

    void CParticleEmitterStack::MoveModule(CParticleModule* Module, int32 Direction)
    {
        if (Module == nullptr || Direction == 0)
        {
            return;
        }

        TVector<TStrongObjectPtr<CParticleModule>>& Stack = GetStack(Module->GetStage());
        for (int32 i = 0; i < (int32)Stack.size(); ++i)
        {
            if (Stack[i].Get() == Module)
            {
                const int32 Target = i + (Direction < 0 ? -1 : 1);
                if (Target >= 0 && Target < (int32)Stack.size())
                {
                    TStrongObjectPtr<CParticleModule> Tmp = Stack[i];
                    Stack[i] = Stack[Target];
                    Stack[Target] = Tmp;
                }
                return;
            }
        }
    }

    void CParticleEmitterStack::EnsureDefaultStack()
    {
        if (!SpawnModules.empty() || !UpdateModules.empty())
        {
            return;
        }

        AddModule(CParticleModule_SpawnLocation::StaticClass());
        AddModule(CParticleModule_InitialVelocity::StaticClass());
        AddModule(CParticleModule_InitialColor::StaticClass());
        AddModule(CParticleModule_InitialSize::StaticClass());
        AddModule(CParticleModule_Lifetime::StaticClass());

        AddModule(CParticleModule_GravityForce::StaticClass());
        AddModule(CParticleModule_ColorOverLife::StaticClass());
        AddModule(CParticleModule_Integrate::StaticClass());
    }
}
