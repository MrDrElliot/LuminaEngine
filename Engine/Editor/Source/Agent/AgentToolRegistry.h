#pragma once

#include "Agent/AgentTool.h"
#include "Containers/HashTable.h"
#include "Core/Threading/Sync.h"

namespace Lumina::Agent
{
    // Every tool an external agent can call. Editor only, and it knows nothing about MCP or any transport.
    class EDITOR_API FToolRegistry
    {
    public:

        static FToolRegistry& Get();

        // Owner names the module or plugin, so unloading one drops everything it added.
        template<typename TParams, typename TResult>
        bool Register(FStringView Owner, FStringView Name, FStringView Description,
            EToolEffect Effect, EToolThread Thread,
            TFunction<FToolResult(const TParams&, TResult&)> Handler)
        {
            FTool Tool;
            Tool.ParamsType = TParams::StaticStruct();
            Tool.ResultType = TResult::StaticStruct();
            Tool.Effect     = Effect;
            Tool.Thread     = Thread;

            // Wrapping a null handler would hide it behind a lambda that is itself never null.
            if (Handler)
            {
                Tool.Invoke = [Handler = Move(Handler)](const void* Params, void* Result)
                {
                    return Handler(*static_cast<const TParams*>(Params), *static_cast<TResult*>(Result));
                };
            }

            return RegisterInternal(Owner, Name, Description, Move(Tool));
        }

        // For a tool whose answer is the text alone, with nothing structured to hand back.
        template<typename TParams>
        bool Register(FStringView Owner, FStringView Name, FStringView Description,
            EToolEffect Effect, EToolThread Thread,
            TFunction<FToolResult(const TParams&)> Handler)
        {
            FTool Tool;
            Tool.ParamsType = TParams::StaticStruct();
            Tool.Effect     = Effect;
            Tool.Thread     = Thread;

            if (Handler)
            {
                Tool.Invoke = [Handler = Move(Handler)](const void* Params, void*)
                {
                    return Handler(*static_cast<const TParams*>(Params));
                };
            }

            return RegisterInternal(Owner, Name, Description, Move(Tool));
        }

        bool Unregister(FStringView Name);

        // Returns how many went away, which is what plugin unload and script reload both need.
        int32 UnregisterOwner(FStringView Owner);

        NODISCARD bool Contains(FStringView Name) const;
        NODISCARD int32 GetToolCount() const;

        // Copied out rather than held, so a tool may register or unregister while another one runs.
        NODISCARD bool TryGetTool(FStringView Name, FTool& OutTool) const;

        // Holds a read lock, so the functor must not register or unregister anything.
        void ForEachTool(const TFunction<void(const FTool&)>& Functor) const;

    private:

        bool RegisterInternal(FStringView Owner, FStringView Name, FStringView Description, FTool&& Tool);

        THashMap<FString, FTool> Tools;

        mutable FSharedMutex ToolsMutex;
    };
}
