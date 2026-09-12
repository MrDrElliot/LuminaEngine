#pragma once

#include "Containers/Span.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "ScriptExports.h"

namespace Lumina::Scripting
{
    /** What a managed type becomes on the native side. */
    enum class EManagedTypeKind : uint8
    {
        None = 0,
        /** A C# subclass of a REFLECT(Scriptable) native class, minted as a CScriptClass. */
        ScriptableClass,
        /** A C# [ScriptStruct] / [DataTableRow] shape, minted as a CScriptStruct. */
        DataStruct,
    };

    /**
     * One managed type as discovered for a generation, with everything needed to build it already in hand.
     *
     * The schema in particular is fetched ONCE here. It used to be gathered twice per type per reload -- once
     * to decide whether the class needed rebuilding and again to do the rebuild -- which is a full marshalled
     * round trip across the .NET boundary paid twice for the same bytes.
     */
    struct FManagedTypeDefinition
    {
        EManagedTypeKind    Kind = EManagedTypeKind::None;
        FName               TypeName;

        /** Empty when the type declares no [Property] members; bHasSchema says which. */
        FScriptExportSchema Schema;
        bool                bHasSchema = false;

        //~ ScriptableClass payload.
        FString             NativeBaseName;
        uint64              OverrideFlags = 0;
        uint8               UpdatePhase = 0;
    };

    /**
     * One stage of rebuilding the engine's view of the managed world after a script load.
     *
     * A stage used to be a call in a hand-written sequence inside the .NET host, with its ordering constraint
     * recorded in a comment beside the call ("Ordered after the class minting, since both read the generation
     * that just loaded"). Declaring stages makes the order a property of the set rather than of one caller,
     * and lets the sequence be exercised without a .NET runtime behind it.
     */
    class IManagedTypeCompiler
    {
    public:

        virtual ~IManagedTypeCompiler() = default;

        /** Names the stage in logs and in the ordering test. */
        virtual const char* GetName() const = 0;

        /** Rebuilds this stage from the generation that just loaded. Definitions covers every kind; a stage
         *  takes the ones it owns and ignores the rest. */
        virtual void Compile(TSpan<const FManagedTypeDefinition> Definitions) = 0;
    };

    /** Installs the engine's own stages, in the order they must run. Idempotent. */
    RUNTIME_API void RegisterBuiltInManagedTypeStages();

    /**
     * The ordered set of stages. Registration order IS execution order, which is the ordering contract that
     * used to live only in comments.
     */
    class RUNTIME_API FManagedTypeRegistry
    {
    public:

        static FManagedTypeRegistry& Get();

        /** Appends a stage. Not owned: a stage is a process-lifetime singleton. */
        void Register(IManagedTypeCompiler* Compiler);

        /** Runs every stage, in order, over one generation's definitions. */
        void CompileAll(TSpan<const FManagedTypeDefinition> Definitions);

        NODISCARD TSpan<IManagedTypeCompiler* const> GetStages() const { return Stages; }

        /** Test seam: drops every registered stage. */
        void ResetForTesting() { Stages.clear(); }

    private:

        TVector<IManagedTypeCompiler*> Stages;
    };
}
