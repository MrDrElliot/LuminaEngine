#include "MCPServer.h"

#include "Agent/AgentGameThread.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Agent/AgentToolSchema.h"
#include "Containers/Algorithm.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Log/Log.h"
#include "MCPAnimGraphTools.h"
#include "MCPAssetTools.h"
#include "MCPBuiltinTools.h"
#include "MCPDataTableTools.h"
#include "MCPEditorSessionTools.h"
#include "MCPMaterialTools.h"
#include "MCPPackageTools.h"
#include "MCPParticleTools.h"
#include "MCPPrefabTools.h"
#include "MCPProfilerTools.h"
#include "MCPRuntimeTools.h"
#include "MCPSceneTools.h"
#include "MCPSequenceTools.h"
#include "MCPStressTools.h"
#include "MCPTerrainTools.h"
#include "Platform/Time/PlatformTime.h"

namespace Lumina::MCP
{
    namespace MCPServerInternal
    {
        // Bump when the protocol revision this speaks changes, and recheck tools against the new one.
        constexpr const char* GProtocolVersion = "2025-06-18";

        constexpr const char* GServerName    = "Lumina";
        constexpr const char* GServerVersion = "0.1.0";

        constexpr const char* GOwner = "MCPEditor";

        constexpr FStringView GSessionHeader = "Mcp-Session-Id";

        // What a model reads. A tool that ran and failed says so here rather than as a transport error.
        FJsonValue MakeContent(const Agent::FToolResult& Outcome)
        {
            FJsonValue Block = FJsonValue::MakeObject();
            Block.Set("type", "text");
            Block.Set("text", Outcome.Text);

            FJsonValue Content = FJsonValue::MakeArray();
            Content.Add(Move(Block));
            return Content;
        }

        // A RawJson string field takes any JSON value, re-encoded as text so a model need not double-quote it.
        void EncodeRawJsonArguments(CStruct* ParamsType, FJsonValue& Arguments)
        {
            if (ParamsType == nullptr || !Arguments.IsObject())
            {
                return;
            }

            ParamsType->ForEachProperty<FProperty>([&Arguments](FProperty* Property)
            {
                if (Property == nullptr || Property->GetType() != EPropertyTypeFlags::String
                    || !Property->HasEditorFlag(EPropertyEditorFlags::RawJson))
                {
                    return;
                }

                FJsonValue* Found = Arguments.Find(FStringView(Property->GetPropertyName().ToString()));
                if (Found == nullptr)
                {
                    return;
                }

                if (!Found->IsString())
                {
                    *Found = FJsonValue(Found->ToString());
                    return;
                }

                // A string that is not itself JSON is a plain string, so it is quoted rather than refused.
                FJsonValue Parsed;
                if (!FJsonValue::Parse(Found->AsString(), Parsed))
                {
                    *Found = FJsonValue(Found->ToString());
                }
            });
        }
    }

    using namespace MCPServerInternal;

    FServer::FServer()
    {
        RegisterProtocolMethods();
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

        RegisterBuiltinTools(GOwner);
        RegisterSceneTools(GOwner);
        RegisterAssetTools(GOwner);
        RegisterMaterialTools(GOwner);
        RegisterParticleTools(GOwner);
        RegisterAnimGraphTools(GOwner);
        RegisterDataTableTools(GOwner);
        RegisterPrefabTools(GOwner);
        RegisterTerrainTools(GOwner);
        RegisterEditorSessionTools(GOwner);
        RegisterProfilerTools(GOwner);
        RegisterRuntimeTools(GOwner);
        RegisterStressTools(GOwner);
        RegisterSequenceTools(GOwner);
        RegisterPackageTools(GOwner);

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
        KeptLoaded.Reset();

        UnregisterEditorSessionTools();
        Agent::FToolRegistry::Get().UnregisterOwner(GOwner);
    }

    void FServer::KeepLoaded(CObject* Object)
    {
        if (Object != nullptr && !Algo::AnyOf(KeptLoaded, [Object](const TStrongObjectPtr<CObject>& Kept) { return Kept.Get() == Object; }))
        {
            KeptLoaded.Emplace(Object);
        }
    }

    void FServer::RegisterProtocolMethods()
    {
        Dispatcher.RegisterMethod(GOwner, "initialize", [](const JsonRpc::FRequest&)
        {
            FJsonValue Result = FJsonValue::MakeObject();
            Result.Set("protocolVersion", GProtocolVersion);

            // Only tools are offered so far, so no resource or prompt capability is advertised.
            Result.Set("capabilities", FJsonValue::MakeObject({ { "tools", FJsonValue::MakeObject() } }));
            Result.Set("serverInfo", FJsonValue::MakeObject({ { "name", GServerName }, { "version", GServerVersion } }));

            return JsonRpc::FResponse::Success(Result);
        });

        Dispatcher.RegisterMethod(GOwner, "notifications/initialized", [](const JsonRpc::FRequest&)
        {
            return JsonRpc::FResponse::Success(FJsonValue::MakeObject());
        });

        Dispatcher.RegisterMethod(GOwner, "ping", [](const JsonRpc::FRequest&)
        {
            return JsonRpc::FResponse::Success(FJsonValue::MakeObject());
        });

        Dispatcher.RegisterMethod(GOwner, "tools/list", [](const JsonRpc::FRequest&)
        {
            FJsonValue Tools = FJsonValue::MakeArray();

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

                FJsonValue Entry = FJsonValue::MakeObject();
                Entry.Set("name", Tool.Name);
                Entry.Set("description", Tool.Description);
                Entry.Set("inputSchema", Input.Schema);

                if (Tool.ResultType != nullptr)
                {
                    if (const Agent::FSchemaResult Output = Agent::GenerateSchema(Tool.ResultType); Output.IsValid())
                    {
                        Entry.Set("outputSchema", Output.Schema);
                    }
                }

                Tools.Add(Move(Entry));
            });

            FJsonValue Result = FJsonValue::MakeObject();
            Result.Set("tools", Move(Tools));
            return JsonRpc::FResponse::Success(Result);
        });

        Dispatcher.RegisterMethod(GOwner, "tools/call", [](const JsonRpc::FRequest& Request)
        {
            const FJsonValue& Name = Request.Params["name"];
            if (!Name.IsString())
            {
                return JsonRpc::FResponse::Failure(JsonRpc::EErrorCode::InvalidParams,
                    "A tool call needs a string name.");
            }

            const FString ToolName(Name.AsString());

            Agent::FTool Tool;
            if (!Agent::FToolRegistry::Get().TryGetTool(FStringView(ToolName), Tool))
            {
                // An unknown tool is a protocol error, unlike a tool that ran and reported failure.
                return JsonRpc::FResponse::Failure(JsonRpc::EErrorCode::MethodNotFound,
                    "No tool is registered under that name.");
            }

            const FJsonValue& Arguments = Request.Params["arguments"];
            FJsonValue Given = Arguments.IsObject() ? Arguments : FJsonValue::MakeObject();

            Agent::FStructInstance Params(Tool.ParamsType);
            Agent::FStructInstance Result(Tool.ResultType);

            if (!Params.IsValid())
            {
                return JsonRpc::FResponse::Failure(JsonRpc::EErrorCode::InternalError,
                    "The tool's parameters could not be constructed.");
            }

            EncodeRawJsonArguments(Params.GetType(), Given);

            if (const Agent::FMarshalResult Read = Agent::ReadStruct(Given, Params.GetType(), Params.Get());
                !Read.IsValid())
            {
                return JsonRpc::FResponse::Failure(JsonRpc::EErrorCode::InvalidParams,
                    FStringView(Read.Error));
            }

            Agent::FToolResult Outcome;

            // A tool that throws reports the failure instead of ending the editor.
            const auto Invoke = [&Outcome, &Tool, &Params, &Result]()
            {
                try
                {
                    Outcome = Tool.Invoke(Params.Get(), Result.Get());
                }
                catch (const std::exception& Exception)
                {
                    Outcome = Agent::FToolResult::Error(FString("The tool failed: ") + Exception.what());
                }
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

            FJsonValue Payload = FJsonValue::MakeObject();
            Payload.Set("content", MakeContent(Outcome));
            Payload.Set("isError", Outcome.bIsError);

            if (!Outcome.bIsError && Result.IsValid())
            {
                FJsonValue Structured;
                if (Agent::WriteStruct(Result.GetType(), Result.Get(), Structured).IsValid())
                {
                    Payload.Set("structuredContent", Move(Structured));
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

        const FStringView Session = Request.FindHeader(GSessionHeader);

        // The session only labels the client in the log, so ending one has nothing else to release.
        if (Request.Method == "DELETE")
        {
            Log.RecordDisconnect(Session);
            return Http::FResponse::Empty(204, "No Content");
        }

        if (Request.Method != "POST")
        {
            return Http::FResponse::Empty(405, "Method Not Allowed");
        }

        const FStringView ContentType = Request.FindHeader("Content-Type");
        if (!ContentType.IsEmpty() && ContentType.Find("application/json") == Constants::kIndexNone)
        {
            return Http::FResponse::Empty(415, "Unsupported Media Type");
        }

        FReply Reply = HandleMessage(FStringView(Request.Body), Session);

        // A notification produces nothing to send, which over HTTP is an accepted empty response.
        if (Reply.Body.IsEmpty())
        {
            return Http::FResponse::Empty(202, "Accepted");
        }

        Http::FResponse Response = Http::FResponse::Json(Move(Reply.Body));
        if (!Reply.OpenedSession.IsEmpty())
        {
            Response.ExtraHeaders.Add({ FString(GSessionHeader), Move(Reply.OpenedSession) });
        }
        return Response;
    }

    FServer::FReply FServer::HandleMessage(FStringView Message, FStringView Session)
    {
        const PlatformTime::FStopwatch Stopwatch;

        FReply Reply;
        Reply.Body = Dispatcher.HandleMessage(Message);
        Reply.OpenedSession = Log.Record(Session, Message, FStringView(Reply.Body), Stopwatch.ElapsedMilliseconds());
        return Reply;
    }
}
