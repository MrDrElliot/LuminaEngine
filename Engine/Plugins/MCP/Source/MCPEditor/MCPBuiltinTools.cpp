#include "MCPBuiltinTools.h"

#include "Agent/AgentToolRegistry.h"
#include "Core/Engine/Engine.h"

namespace Lumina::MCP
{
    namespace MCPBuiltinToolsInternal
    {
        Agent::FToolResult ReportEngineStatus(const SEngineStatusParams&, SEngineStatusResult& Out)
        {
            if (Global::GEngine == nullptr)
            {
                return Agent::FToolResult::Error("The engine is not running.");
            }

            Out.bHasProject = Global::GEngine->HasLoadedProject();

            const FStringView Name = Global::GEngine->GetProjectName();
            const FStringView Path = Global::GEngine->GetProjectPath();

            Out.ProjectName = FStringView(Name.GetData(), Name.Len());
            Out.ProjectPath = FStringView(Path.GetData(), Path.Len());

            return Out.bHasProject
                ? Agent::FToolResult::Ok(Lumina::Format("Project '{}' is open at {}.", Out.ProjectName, Out.ProjectPath))
                : Agent::FToolResult::Ok("No project is open.");
        }

        void RegisterEngineStatus(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SEngineStatusParams, SEngineStatusResult>(
                Owner, "engine.status",
                "Report which project the editor currently has open.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread, &ReportEngineStatus);
        }
    }

    using namespace MCPBuiltinToolsInternal;

    void RegisterBuiltinTools(FStringView Owner)
    {
        RegisterEngineStatus(Owner);
    }
}
