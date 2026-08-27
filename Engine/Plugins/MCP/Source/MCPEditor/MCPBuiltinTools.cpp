#include "MCPBuiltinTools.h"

#include "Agent/AgentToolRegistry.h"
#include "Core/Engine/Engine.h"

namespace Lumina::MCP
{
    void RegisterBuiltinTools(FStringView Owner)
    {
        Agent::FToolRegistry::Get().Register<SEngineStatusParams, SEngineStatusResult>(
            Owner,
            "engine.status",
            "Report which project the editor currently has open.",
            Agent::EToolEffect::ReadOnly,
            Agent::EToolThread::GameThread,
            [](const SEngineStatusParams&, SEngineStatusResult& Out)
            {
                if (GEngine == nullptr)
                {
                    return Agent::FToolResult::Error("The engine is not running.");
                }

                Out.bHasProject = GEngine->HasLoadedProject();

                const FStringView Name = GEngine->GetProjectName();
                const FStringView Path = GEngine->GetProjectPath();

                Out.ProjectName.assign(Name.data(), Name.size());
                Out.ProjectPath.assign(Path.data(), Path.size());

                return Out.bHasProject
                    ? Agent::FToolResult::Ok(Lumina::Format("Project '{}' is open at {}.", Out.ProjectName, Out.ProjectPath))
                    : Agent::FToolResult::Ok("No project is open.");
            });
    }
}
