#pragma once

#include "Agent/AgentAssetResolve.h"
#include "Containers/String.h"
#include "Core/Http/HttpMessage.h"
#include "Core/Http/HttpServer.h"
#include "Core/JsonRpc/JsonRpc.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "MCPLog.h"

namespace Lumina::MCP
{
    struct FServerSettings
    {
        uint16 Port = 8787;

        // Every Model Context Protocol message arrives by POST to this path.
        FString Endpoint = "/mcp";
    };

    // Carries Model Context Protocol traffic over loopback HTTP and hands it to a JSON-RPC dispatcher.
    class MCPEDITOR_API FServer
    {
    public:

        FServer();
        ~FServer();

        FServer(const FServer&) = delete;
        FServer& operator=(const FServer&) = delete;

        bool Start(const FServerSettings& InSettings);
        void Stop();

        NODISCARD bool IsRunning() const { return Transport.IsRunning(); }
        NODISCARD uint16 GetBoundPort() const { return Transport.GetBoundPort(); }

        // Where a plugin or subsystem registers the methods it wants to answer.
        NODISCARD JsonRpc::FDispatcher& GetDispatcher() { return Dispatcher; }

        NODISCARD const FLog& GetLog() const { return Log; }
        NODISCARD FLog& GetLog() { return Log; }

        struct FReply
        {
            FString Body;

            // Set when this message opened a session, which the client sends back on every later request.
            FString OpenedSession;
        };

        // The dispatch boundary every message crosses, so each one is logged whichever method or tool it names.
        NODISCARD FReply HandleMessage(FStringView Message, FStringView Session);

        // Holds an asset for the session, as an open editor tab would, when a tool's own short-lived ref is its only one.
        void KeepLoaded(CObject* Object);

    private:

        void RegisterProtocolMethods();

        Http::FResponse Route(const Http::FRequest& Request);

        Http::FServer        Transport;
        JsonRpc::FDispatcher Dispatcher;
        FLog                 Log;

        FServerSettings Settings;

        TVector<TStrongObjectPtr<CObject>> KeptLoaded;
    };

    // Keeps the asset through the running server, since an unheld one is freed when any short-lived ref drops, such as a compile job's.
    MCPEDITOR_API void KeepAssetLoaded(CObject* Asset);

    MCPEDITOR_API void MarkAssetDirty(const CObject* Asset);

    // Rejects the traversal and separator tricks that would take a write outside the mounted roots.
    NODISCARD MCPEDITOR_API bool IsWellFormedAssetPath(const FString& Path, FString& OutError);

    // Checks where a new asset would go and gives the .lasset path, failing when the folder is outside the content roots or the file exists.
    NODISCARD MCPEDITOR_API bool MakeNewAssetPath(const FString& Folder, const FString& Name, FFixedString& OutPath, FString& OutError);

    // Loads an asset a tool may edit and keeps it for the session, so the edit is not freed before a save.
    template<typename T>
    NODISCARD bool ResolveEditableAsset(FStringView Reference, T*& OutAsset, FString& OutError)
    {
        if (!Agent::ResolveAsset<T>(Reference, OutAsset, OutError))
        {
            return false;
        }

        KeepAssetLoaded(OutAsset);
        return true;
    }
}
