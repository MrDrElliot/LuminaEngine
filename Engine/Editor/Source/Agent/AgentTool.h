#pragma once

#include "Containers/Function.h"
#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Core/Object/Class.h"
#include "Core/Templates/LuminaTemplate.h"

namespace Lumina::Agent
{
    enum class EToolEffect : uint8
    {
        // Reads editor state and changes nothing, so it is safe to run without a transaction.
        ReadOnly,

        // Changes editor state, so the caller wraps it in a transaction and a read-only mode can refuse it.
        Mutating,
    };

    enum class EToolThread : uint8
    {
        // Touches the world, the renderer or CObjects, so it has to run where those are safe.
        GameThread,

        // Reaches nothing the game thread owns, so it can answer straight from the calling thread.
        Any,
    };

    struct FToolResult
    {
        // What a language model reads. A failure explains itself here rather than in a transport error.
        FString Text;

        bool bIsError = false;

        NODISCARD static FToolResult Ok(FString InText)
        {
            FToolResult Result;
            Result.Text = Move(InText);
            return Result;
        }

        NODISCARD static FToolResult Error(FString InText)
        {
            FToolResult Result;
            Result.Text     = Move(InText);
            Result.bIsError = true;
            return Result;
        }
    };

    // Type erased, so one table can hold tools whose parameter types have nothing in common.
    using FToolInvoke = TFunction<FToolResult(const void* Params, void* Result)>;

    struct FTool
    {
        FString Owner;
        FString Name;
        FString Description;

        CStruct* ParamsType = nullptr;

        // Null when the tool answers with text alone and has no structured payload.
        CStruct* ResultType = nullptr;

        EToolEffect Effect = EToolEffect::ReadOnly;
        EToolThread Thread = EToolThread::GameThread;

        FToolInvoke Invoke;
    };

    // Holds one reflected struct in aligned storage, so no caller has to remember to destroy it.
    class EDITOR_API FStructInstance
    {
    public:

        explicit FStructInstance(CStruct* InType);
        ~FStructInstance();

        FStructInstance(const FStructInstance&) = delete;
        FStructInstance& operator=(const FStructInstance&) = delete;

        NODISCARD void* Get() const { return Storage; }
        NODISCARD CStruct* GetType() const { return Type; }
        NODISCARD bool IsValid() const { return Storage != nullptr; }

    private:

        CStruct* Type    = nullptr;
        void*    Storage = nullptr;
    };
}
