#include "EditorPCH.h"
#include "Agent/AgentToolRegistry.h"

#include "Agent/AgentToolSchema.h"

#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "Memory/Memory.h"

namespace Lumina::Agent
{
    FStructInstance::FStructInstance(CStruct* InType)
        : Type(InType)
    {
        if (Type == nullptr)
        {
            return;
        }

        const uint32 Size = Type->GetSize();
        if (Size == 0)
        {
            return;
        }

        Storage = Memory::Malloc(Size, Type->GetAlignment());
        if (Storage != nullptr)
        {
            Type->InitializeStruct(Storage);
        }
    }

    FStructInstance::~FStructInstance()
    {
        if (Storage == nullptr)
        {
            return;
        }

        Type->DestroyStruct(Storage);
        Memory::Free(Storage);

        Storage = nullptr;
    }

    FToolRegistry& FToolRegistry::Get()
    {
        static FToolRegistry Registry;
        return Registry;
    }

    bool FToolRegistry::RegisterInternal(FStringView Owner, FStringView Name, FStringView Description, FTool&& Tool)
    {
        if (Name.empty())
        {
            LOG_WARN("[AgentTools] Refused a tool with no name.");
            return false;
        }

        if (Tool.ParamsType == nullptr)
        {
            LOG_WARN("[AgentTools] Refused '{}' because its parameter type is not reflected.", Name);
            return false;
        }

        if (!Tool.Invoke)
        {
            LOG_WARN("[AgentTools] Refused '{}' because its handler is null.", Name);
            return false;
        }

        // Refused here rather than at call time, so a tool can never advertise a shape nothing can parse.
        if (const FSchemaResult Schema = GenerateSchema(Tool.ParamsType); !Schema.IsValid())
        {
            LOG_WARN("[AgentTools] Refused '{}' because its parameters cannot be described. {}",
                Name, Schema.Error);
            return false;
        }

        if (Tool.ResultType != nullptr)
        {
            if (const FSchemaResult Schema = GenerateSchema(Tool.ResultType); !Schema.IsValid())
            {
                LOG_WARN("[AgentTools] Refused '{}' because its result cannot be described. {}",
                    Name, Schema.Error);
                return false;
            }
        }

        Tool.Owner       = FString(Owner.data(), Owner.size());
        Tool.Name        = FString(Name.data(), Name.size());
        Tool.Description = FString(Description.data(), Description.size());

        const FString Key = Tool.Name;

        FWriteScopeLock Lock(ToolsMutex);

        // A silent replacement would leave two owners believing they own the name.
        if (Tools.find(Key) != Tools.end())
        {
            LOG_WARN("[AgentTools] '{}' is already registered and was not replaced.", Name);
            return false;
        }

        Tools[Key] = Move(Tool);
        return true;
    }

    bool FToolRegistry::Unregister(FStringView Name)
    {
        const FString Key(Name.data(), Name.size());

        FWriteScopeLock Lock(ToolsMutex);

        const auto Found = Tools.find(Key);
        if (Found == Tools.end())
        {
            return false;
        }

        Tools.erase(Found);
        return true;
    }

    int32 FToolRegistry::UnregisterOwner(FStringView Owner)
    {
        const FString Match(Owner.data(), Owner.size());

        FWriteScopeLock Lock(ToolsMutex);

        int32 Removed = 0;
        for (auto It = Tools.begin(); It != Tools.end(); )
        {
            if (It->second.Owner == Match)
            {
                It = Tools.erase(It);
                ++Removed;
            }
            else
            {
                ++It;
            }
        }

        return Removed;
    }

    bool FToolRegistry::Contains(FStringView Name) const
    {
        const FString Key(Name.data(), Name.size());

        FReadScopeLock Lock(ToolsMutex);
        return Tools.find(Key) != Tools.end();
    }

    int32 FToolRegistry::GetToolCount() const
    {
        FReadScopeLock Lock(ToolsMutex);
        return static_cast<int32>(Tools.size());
    }

    bool FToolRegistry::TryGetTool(FStringView Name, FTool& OutTool) const
    {
        const FString Key(Name.data(), Name.size());

        FReadScopeLock Lock(ToolsMutex);

        const auto Found = Tools.find(Key);
        if (Found == Tools.end())
        {
            return false;
        }

        OutTool = Found->second;
        return true;
    }

    void FToolRegistry::ForEachTool(const TFunction<void(const FTool&)>& Functor) const
    {
        if (!Functor)
        {
            return;
        }

        FReadScopeLock Lock(ToolsMutex);
        for (const auto& Pair : Tools)
        {
            Functor(Pair.second);
        }
    }
}
