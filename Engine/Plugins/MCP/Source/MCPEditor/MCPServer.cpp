#include "MCPServer.h"

#include "Agent/AgentGameThread.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Agent/AgentToolSchema.h"
#include "Log/Log.h"
#include "MCPAssetTools.h"
#include "MCPMaterialTools.h"
#include "MCPBuiltinTools.h"
#include "MCPSceneTools.h"

namespace Lumina::MCP
{
    namespace
    {
        // Bump when the protocol revision this speaks changes, and recheck tools against the new one.
        constexpr const char* GProtocolVersion = "2025-06-18";

        constexpr const char* GServerName    = "Lumina";
        constexpr const char* GServerVersion = "0.1.0";

        constexpr const char* GOwner = "MCPEditor";

        std::string ToStandard(FStringView Text)
        {
            return std::string(Text.data(), Text.size());
        }

        // What a model reads. A tool that ran and failed says so here rather than as a transport error.
        nlohmann::json MakeContent(const Agent::FToolResult& Outcome)
        {
            nlohmann::json Block = nlohmann::json::object();
            Block["type"] = "text";
            Block["text"] = ToStandard(FStringView(Outcome.Text));

            nlohmann::json Content = nlohmann::json::array();
            Content.push_back(Move(Block));
            return Content;
        }
    }

    FServer::~FServer()
    {
        Stop();
    }

    bool FServer::Start(const FServerSettings& InSettings)
    {
        if (IsRunning())
        {
            LOG_WARN("[MCP] The server is already listening on port {}.", GetBoundPort());
            return false;
        }

        Settings = InSettings;

        RegisterProtocolMethods();
        RegisterBuiltinTools(GOwner);
        RegisterSceneTools(GOwner);
        RegisterAssetTools(GOwner);
        RegisterMaterialTools(GOwner);

        Http::FServerParams Params;
        Params.Port = Settings.Port;

        // Loopback only, since anything that reaches this endpoint can drive the editor.
        Params.bLoopbackOnly = true;

        const bool bStarted = Transport.Start(Params, [this](const Http::FRequest& Request)
        {
            return Route(Request);
        });

        if (!bStarted)
        {
            LOG_ERROR("[MCP] Failed to listen on port {}.", Settings.Port);
            Dispatcher.UnregisterOwner(GOwner);
            return false;
        }

        LOG_INFO("[MCP] Listening on loopback port {} at endpoint {}.", GetBoundPort(), Settings.Endpoint);
        return true;
    }

    void FServer::Stop()
    {
        if (Transport.IsRunning())
        {
            LOG_INFO("[MCP] Shutting down.");
        }

        Transport.Stop();

        Dispatcher.UnregisterOwner(GOwner);
        Agent::FToolRegistry::Get().UnregisterOwner(GOwner);
    }

    void FServer::RegisterProtocolMethods()
    {
        Dispatcher.RegisterMethod(GOwner, "initialize", [](const JsonRpc::FRequest&)
        {
            nlohmann::json Result = nlohmann::json::object();
            Result["protocolVersion"] = GProtocolVersion;

            // Only tools are offered so far, so no resource or prompt capability is advertised.
            Result["capabilities"] = nlohmann::json::object();
            Result["capabilities"]["tools"] = nlohmann::json::object();

            Result["serverInfo"] = nlohmann::json::object();
            Result["serverInfo"]["name"]    = GServerName;
            Result["serverInfo"]["version"] = GServerVersion;

            return JsonRpc::FResponse::Success(Result);
        });

        Dispatcher.RegisterMethod(GOwner, "notifications/initialized", [](const JsonRpc::FRequest&)
        {
            return JsonRpc::FResponse::Success(nlohmann::json::object());
        });

        Dispatcher.RegisterMethod(GOwner, "ping", [](const JsonRpc::FRequest&)
        {
            return JsonRpc::FResponse::Success(nlohmann::json::object());
        });

        Dispatcher.RegisterMethod(GOwner, "tools/list", [](const JsonRpc::FRequest&)
        {
            nlohmann::json Tools = nlohmann::json::array();

            Agent::FToolRegistry::Get().ForEachTool([&Tools](const Agent::FTool& Tool)
            {
                const Agent::FSchemaResult Input = Agent::GenerateSchema(Tool.ParamsType);
                if (!Input.IsValid())
                {
                    // Registration already refused this shape, so reaching here means something changed.
                    LOG_WARN("[MCP] Skipping '{}' because its parameters no longer describe. {}",
                        Tool.Name, Input.Error);
                    return;
                }

                nlohmann::json Entry = nlohmann::json::object();
                Entry["name"]        = ToStandard(FStringView(Tool.Name));
                Entry["description"] = ToStandard(FStringView(Tool.Description));
                Entry["inputSchema"] = Input.Schema;

                if (Tool.ResultType != nullptr)
                {
                    if (const Agent::FSchemaResult Output = Agent::GenerateSchema(Tool.ResultType); Output.IsValid())
                    {
                        Entry["outputSchema"] = Output.Schema;
                    }
                }

                Tools.push_back(Move(Entry));
            });

            nlohmann::json Result = nlohmann::json::object();
            Result["tools"] = Move(Tools);
            return JsonRpc::FResponse::Success(Result);
        });

        Dispatcher.RegisterMethod(GOwner, "tools/call", [](const JsonRpc::FRequest& Request)
        {
            const auto Name = Request.Params.find("name");
            if (Name == Request.Params.end() || !Name->is_string())
            {
                return JsonRpc::FResponse::Failure(JsonRpc::EErrorCode::InvalidParams,
                    "A tool call needs a string name.");
            }

            const FString ToolName(Name->get_ref<const std::string&>().c_str());

            Agent::FTool Tool;
            if (!Agent::FToolRegistry::Get().TryGetTool(FStringView(ToolName), Tool))
            {
                // An unknown tool is a protocol error, unlike a tool that ran and reported failure.
                return JsonRpc::FResponse::Failure(JsonRpc::EErrorCode::MethodNotFound,
                    "No tool is registered under that name.");
            }

            const auto Arguments = Request.Params.find("arguments");
            const nlohmann::json Given = Arguments != Request.Params.end() && Arguments->is_object()
                ? *Arguments
                : nlohmann::json::object();

            Agent::FStructInstance Params(Tool.ParamsType);
            Agent::FStructInstance Result(Tool.ResultType);

            if (!Params.IsValid())
            {
                return JsonRpc::FResponse::Failure(JsonRpc::EErrorCode::InternalError,
                    "The tool's parameters could not be constructed.");
            }

            if (const Agent::FMarshalResult Read = Agent::ReadStruct(Given, Params.GetType(), Params.Get());
                !Read.IsValid())
            {
                return JsonRpc::FResponse::Failure(JsonRpc::EErrorCode::InvalidParams,
                    FStringView(Read.Error));
            }

            Agent::FToolResult Outcome;

            const auto Invoke = [&Outcome, &Tool, &Params, &Result]()
            {
                Outcome = Tool.Invoke(Params.Get(), Result.Get());
            };

            if (Tool.Thread == Agent::EToolThread::GameThread)
            {
                const Agent::EGameThreadResult Hop = Agent::FGameThreadGate::Run(
                    Invoke, Agent::FGameThreadGate::GetDefaultTimeoutMilliseconds());

                if (Hop == Agent::EGameThreadResult::TimedOut)
                {
                    Outcome = Agent::FToolResult::Error("The editor did not run this in time. It may be busy.");
                }
            }
            else
            {
                Invoke();
            }

            nlohmann::json Payload = nlohmann::json::object();
            Payload["content"] = MakeContent(Outcome);
            Payload["isError"] = Outcome.bIsError;

            if (!Outcome.bIsError && Result.IsValid())
            {
                nlohmann::json Structured;
                if (Agent::WriteStruct(Result.GetType(), Result.Get(), Structured).IsValid())
                {
                    Payload["structuredContent"] = Move(Structured);
                }
            }

            return JsonRpc::FResponse::Success(Payload);
        });
    }

    Http::FResponse FServer::Route(const Http::FRequest& Request)
    {
        if (Request.Target != Settings.Endpoint)
        {
            return Http::FResponse::Empty(404, "Not Found");
        }

        // A stream is what a GET would open, and nothing here pushes messages the client did not ask for.
        if (Request.Method == "GET")
        {
            return Http::FResponse::Empty(405, "Method Not Allowed");
        }

        // Ending a session costs nothing to honor, since no per-session state is kept yet.
        if (Request.Method == "DELETE")
        {
            return Http::FResponse::Empty(204, "No Content");
        }

        if (Request.Method != "POST")
        {
            return Http::FResponse::Empty(405, "Method Not Allowed");
        }

        const FStringView ContentType = Request.FindHeader("Content-Type");
        if (!ContentType.empty() && ContentType.find("application/json") == FStringView::npos)
        {
            return Http::FResponse::Empty(415, "Unsupported Media Type");
        }

        const FString Reply = Dispatcher.HandleMessage(FStringView(Request.Body));

        // A notification produces nothing to send, which over HTTP is an accepted empty response.
        if (Reply.empty())
        {
            return Http::FResponse::Empty(202, "Accepted");
        }

        return Http::FResponse::Json(Reply);
    }
}
