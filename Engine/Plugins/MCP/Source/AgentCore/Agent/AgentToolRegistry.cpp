#include "AgentCorePCH.h"
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
        if (Name.IsEmpty())
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

        Tool.Owner       = FString(Owner.GetData(), Owner.Len());
        Tool.Name        = FString(Name.GetData(), Name.Len());
        Tool.Description = FString(Description.GetData(), Description.Len());

        const FString Key = Tool.Name;

        FWriteScopeLock Lock(ToolsMutex);

        // A silent replacement would leave two owners believing they own the name.
        if (!Tools.TryAdd(Key, Move(Tool)))
        {
            LOG_WARN("[AgentTools] '{}' is already registered and was not replaced.", Name);
            return false;
        }
        return true;
    }

    bool FToolRegistry::Unregister(FStringView Name)
    {
        const FString Key(Name.GetData(), Name.Len());

        FWriteScopeLock Lock(ToolsMutex);
        return Tools.Remove(Key) > 0;
    }

    int32 FToolRegistry::UnregisterOwner(FStringView Owner)
    {
        const FString Match(Owner.GetData(), Owner.Len());

        FWriteScopeLock Lock(ToolsMutex);

        int32 Removed = 0;
        for (auto It = Tools.begin(); It != Tools.end(); )
        {
            if (It->Value.Owner == Match)
            {
                It = Tools.Remove(It);
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
        const FString Key(Name.GetData(), Name.Len());

        FReadScopeLock Lock(ToolsMutex);
        return Tools.Contains(Key);
    }

    int32 FToolRegistry::GetToolCount() const
    {
        FReadScopeLock Lock(ToolsMutex);
        return Tools.Num();
    }

    bool FToolRegistry::TryGetTool(FStringView Name, FTool& OutTool) const
    {
        const FString Key(Name.GetData(), Name.Len());

        FReadScopeLock Lock(ToolsMutex);

        const auto Found = Tools.Find(Key);
        if (Found == nullptr)
        {
            return false;
        }

        OutTool = (*Found);
        return true;
    }

    void FToolRegistry::ForEachTool(const TFunction<void(const FTool&)>& Functor) const
    {
        if (!Functor)
        {
            return;
        }

        FReadScopeLock Lock(ToolsMutex);
        for (const auto& [Name, Tool] : Tools)
        {
            Functor(Tool);
        }
    }
}
