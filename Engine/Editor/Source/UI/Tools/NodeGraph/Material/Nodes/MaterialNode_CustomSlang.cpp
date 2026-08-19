#include "MaterialNode_CustomSlang.h"

#include "imgui.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialInput.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace
    {
        // Every alias here is declared natively for the pixel stage and re-declared by the vertex preamble,
        // so an unconnected input resolves in either stage.
        FString DefaultExpressionFor(ECustomSlangInputDefault Default)
        {
            switch (Default)
            {
                case ECustomSlangInputDefault::UV0:           return "float2(UV0)";
                case ECustomSlangInputDefault::WorldPosition: return "WorldPosition";
                case ECustomSlangInputDefault::WorldNormal:   return "WorldNormal";
                case ECustomSlangInputDefault::VertexColor:   return "VertexColor";
                case ECustomSlangInputDefault::Zero:
                default:                                      return "0.0";
            }
        }

        bool IsIdentifierStart(char C) { return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || C == '_'; }
        bool IsIdentifierBody(char C)  { return IsIdentifierStart(C) || (C >= '0' && C <= '9'); }

        bool IsValidIdentifier(const char* Str)
        {
            if (Str == nullptr || Str[0] == '\0' || !IsIdentifierStart(Str[0]))
            {
                return false;
            }
            for (const char* P = Str + 1; *P != '\0'; ++P)
            {
                if (!IsIdentifierBody(*P))
                {
                    return false;
                }
            }
            return true;
        }

        // Names that would produce a redeclaration or a parse error where we emit them. Not the full
        // Slang keyword set -- just the ones a material author plausibly types as a pin name.
        bool IsReservedWord(const FString& Name)
        {
            static const char* Reserved[] =
            {
                "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue",
                "return", "discard", "struct", "class", "interface", "enum", "typedef", "namespace",
                "const", "static", "uniform", "in", "out", "inout", "void", "true", "false", "this",
                "float", "float2", "float3", "float4", "float2x2", "float3x3", "float4x4",
                "half", "half2", "half3", "half4", "double", "int", "int2", "int3", "int4",
                "uint", "uint2", "uint3", "uint4", "bool", "bool2", "bool3", "bool4",
                "matrix", "vector", "cbuffer", "tbuffer", "sampler", "SamplerState", "SamplerComparisonState",
                "Texture1D", "Texture2D", "Texture3D", "TextureCube", "Texture2DArray",
                "RWTexture2D", "StructuredBuffer", "RWStructuredBuffer", "ByteAddressBuffer",
            };
            for (const char* Word : Reserved)
            {
                if (Name == Word)
                {
                    return true;
                }
            }
            return false;
        }

        // Brace/paren balance over the body, skipping comments and literals. Load-bearing: an unbalanced
        // closing brace ends our generated scope early and cascades errors far from the node that caused it.
        struct FBalanceResult
        {
            bool  bBalanced = true;
            const char* Problem = nullptr;
        };

        FBalanceResult CheckBalance(const FString& Code)
        {
            int32 Braces = 0;
            int32 Parens = 0;
            int32 Brackets = 0;
            bool  bInBlockComment = false;

            const size_t Len = Code.size();
            for (size_t i = 0; i < Len; ++i)
            {
                const char C = Code[i];
                const char N = (i + 1 < Len) ? Code[i + 1] : '\0';

                if (bInBlockComment)
                {
                    if (C == '*' && N == '/') { bInBlockComment = false; ++i; }
                    continue;
                }

                if (C == '/' && N == '/')
                {
                    while (i < Len && Code[i] != '\n') { ++i; }
                    continue;
                }
                if (C == '/' && N == '*')
                {
                    bInBlockComment = true;
                    ++i;
                    continue;
                }
                if (C == '"' || C == '\'')
                {
                    const char Quote = C;
                    ++i;
                    while (i < Len && Code[i] != Quote)
                    {
                        if (Code[i] == '\\') { ++i; }
                        ++i;
                    }
                    continue;
                }

                switch (C)
                {
                    case '{': ++Braces;   break;
                    case '}': --Braces;   break;
                    case '(': ++Parens;   break;
                    case ')': --Parens;   break;
                    case '[': ++Brackets; break;
                    case ']': --Brackets; break;
                    default: break;
                }

                if (Braces < 0)
                {
                    return FBalanceResult{ false, "Unmatched '}' -- it would close the generated block early and break the rest of the shader." };
                }
                if (Parens < 0)
                {
                    return FBalanceResult{ false, "Unmatched ')'." };
                }
                if (Brackets < 0)
                {
                    return FBalanceResult{ false, "Unmatched ']'." };
                }
            }

            if (bInBlockComment)
            {
                return FBalanceResult{ false, "Unterminated block comment ('/*' with no '*/')." };
            }
            if (Braces != 0)
            {
                return FBalanceResult{ false, "Unbalanced braces -- a '{' is never closed." };
            }
            if (Parens != 0)
            {
                return FBalanceResult{ false, "Unbalanced parentheses." };
            }
            if (Brackets != 0)
            {
                return FBalanceResult{ false, "Unbalanced brackets." };
            }
            return FBalanceResult{};
        }

        // Preprocessor directives escape our scoping (and #include can pull arbitrary files into the
        // generated shader), so they are rejected outright.
        bool FindPreprocessorDirective(const FString& Code, FString& OutLine)
        {
            const size_t Len = Code.size();
            bool bLineStart = true;
            for (size_t i = 0; i < Len; ++i)
            {
                const char C = Code[i];
                if (C == '\n')
                {
                    bLineStart = true;
                    continue;
                }
                if (C == ' ' || C == '\t' || C == '\r')
                {
                    continue;
                }
                if (bLineStart && C == '#')
                {
                    size_t End = i;
                    while (End < Len && Code[End] != '\n') { ++End; }
                    OutLine = Code.substr(i, End - i);
                    return true;
                }
                bLineStart = false;
            }
            return false;
        }
    }

    void CMaterialExpression_CustomSlang::BuildNode()
    {
        // Starter signature so a freshly placed node compiles and shows something. Guarded by a serialized
        // flag so it runs once at creation and never resurrects pins the user deleted.
        if (!bDefaultsSeeded)
        {
            bDefaultsSeeded = true;

            if (Inputs.empty())
            {
                FCustomSlangPin& UV = Inputs.emplace_back();
                UV.Name    = "UV";
                UV.Type    = EMaterialValueType::Float2;
                UV.Default = ECustomSlangInputDefault::UV0;
            }
            if (Outputs.empty())
            {
                FCustomSlangPin& Out = Outputs.emplace_back();
                Out.Name = "Out";
                Out.Type = EMaterialValueType::Float3;
            }
        }

        RebuildPins();
    }

    uint64 CMaterialExpression_CustomSlang::ComputeSignatureHash() const
    {
        size_t Seed = 0;
        auto Mix = [&Seed](const TVector<FCustomSlangPin>& Pins, uint64 Salt)
        {
            Hash::HashCombine(Seed, Salt);
            for (const FCustomSlangPin& Pin : Pins)
            {
                Hash::HashCombine(Seed, Pin.Name.GetID());
                Hash::HashCombine(Seed, (uint64)Pin.Type);
            }
        };
        Mix(Inputs, 0x1ull);
        Mix(Outputs, 0x2ull);
        return (uint64)Seed;
    }

    void CMaterialExpression_CustomSlang::RebuildPins()
    {
        // Keep wires whose pin name survives the rebuild (same approach as the material-function call
        // node); pin IDs are name-hashed so they stay stable across save/load.
        struct FConnSnapshot { FString Name; bool bInput; TVector<CEdNodeGraphPin*> Remotes; };
        TVector<FConnSnapshot> Snapshots;

        auto SnapshotPins = [&](const TVector<TObjectPtr<CEdNodeGraphPin>>& Pins, bool bInput)
        {
            for (const TObjectPtr<CEdNodeGraphPin>& Pin : Pins)
            {
                if (!Pin.IsValid() || !Pin->HasConnection())
                {
                    continue;
                }
                FConnSnapshot Snap;
                Snap.Name    = Pin->GetPinName();
                Snap.bInput  = bInput;
                Snap.Remotes = Pin->GetConnections();
                Snapshots.push_back(Move(Snap));
            }
        };
        SnapshotPins(GetInputPins(), true);
        SnapshotPins(GetOutputPins(), false);

        auto ClearDirection = [&](ENodePinDirection Direction)
        {
            for (const TObjectPtr<CEdNodeGraphPin>& Pin : NodePins[(uint32)Direction])
            {
                TVector<CEdNodeGraphPin*> Remotes = Pin->GetConnections();
                for (CEdNodeGraphPin* Remote : Remotes)
                {
                    Remote->RemoveConnection(Pin.Get());
                }
                Pin->ClearConnections();
            }
            NodePins[(uint32)Direction].clear();
        };
        ClearDirection(ENodePinDirection::Input);
        ClearDirection(ENodePinDirection::Output);

        CustomInputPins.clear();
        CustomOutputPins.clear();

        for (const FCustomSlangPin& Decl : Inputs)
        {
            const EMaterialInputType T = ToMaterialInputType(Decl.Type);
            CMaterialInput* Pin = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), Decl.Name.c_str(), ENodePinDirection::Input));
            Pin->SetPinName(Decl.Name.c_str());
            Pin->SetInputType(T);
            Pin->SetComponentMask(FullMaskForType(T));
            CustomInputPins.push_back(Pin);
        }

        for (const FCustomSlangPin& Decl : Outputs)
        {
            const EMaterialInputType T = ToMaterialInputType(Decl.Type);
            CMaterialOutput* Pin = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), Decl.Name.c_str(), ENodePinDirection::Output));
            Pin->SetPinName(Decl.Name.c_str());
            Pin->SetInputType(T);
            Pin->SetComponentMask(FullMaskForType(T));
            CustomOutputPins.push_back(Pin);
        }

        // Restore surviving wires by (direction, name). Names are not guaranteed unique, so a pin takes one
        // snapshot only -- otherwise two pins sharing a name pile both wire sets onto the first.
        THashSet<CEdNodeGraphPin*> Restored;
        for (const FConnSnapshot& Snap : Snapshots)
        {
            const TVector<TObjectPtr<CEdNodeGraphPin>>& Pins = Snap.bInput ? GetInputPins() : GetOutputPins();
            for (const TObjectPtr<CEdNodeGraphPin>& Pin : Pins)
            {
                if (!Pin.IsValid() || Pin->GetPinName() != Snap.Name || Restored.count(Pin.Get()) != 0)
                {
                    continue;
                }
                Restored.insert(Pin.Get());
                for (CEdNodeGraphPin* Remote : Snap.Remotes)
                {
                    if (Remote != nullptr)
                    {
                        Pin->AddConnection(Remote);
                        Remote->AddConnection(Pin.Get());
                    }
                }
                break;
            }
        }

        SignatureHash = ComputeSignatureHash();
        bPinsBuilt = true;
    }

    void CMaterialExpression_CustomSlang::DrawNodeTitleBar()
    {
        // Signature edits land through the details panel, so pick them up on the next draw.
        const uint64 Current = ComputeSignatureHash();
        if (!bPinsBuilt || Current != SignatureHash)
        {
            RebuildPins();
        }
        Super::DrawNodeTitleBar();
    }

    void CMaterialExpression_CustomSlang::DrawNodeBody()
    {
        ImGui::TextUnformatted(Title.c_str());

        // Compact status line: enough to see at a glance whether this node is wired and healthy,
        // without trying to host a code editor on the canvas.
        int32 Lines = Code.empty() ? 0 : 1;
        for (char C : Code)
        {
            Lines += (C == '\n') ? 1 : 0;
        }

        TVector<FString> Problems;
        const bool bValid = Validate(Problems);

        ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.64f, 1.0f), "%d in / %d out - %d line%s",
                           (int32)Inputs.size(), (int32)Outputs.size(), Lines, Lines == 1 ? "" : "s");

        if (!bValid)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "%d problem%s",
                               (int32)Problems.size(), Problems.size() == 1 ? "" : "s");
            if (ImGui::IsItemHovered() && !Problems.empty())
            {
                ImGui::SetTooltip("%s", Problems[0].c_str());
            }
        }
    }

    bool CMaterialExpression_CustomSlang::Validate(TVector<FString>& OutProblems) const
    {
        // Declared names: valid identifiers, not keywords, unique across BOTH directions (an input
        // sharing a name with an output would make the copy-back ambiguous).
        THashSet<FString> Seen;
        auto CheckNames = [&](const TVector<FCustomSlangPin>& Pins, const char* Direction)
        {
            for (const FCustomSlangPin& Pin : Pins)
            {
                const FString Name = Pin.Name.ToString();
                if (!IsValidIdentifier(Name.c_str()))
                {
                    OutProblems.push_back(FString(Direction) + " name '" + Name + "' is not a valid Slang identifier (letters, digits, underscore; cannot start with a digit).");
                    continue;
                }
                if (IsReservedWord(Name))
                {
                    OutProblems.push_back(FString(Direction) + " name '" + Name + "' is a reserved Slang keyword.");
                    continue;
                }
                if (!Seen.insert(Name).second)
                {
                    OutProblems.push_back(FString("Duplicate pin name '") + Name + "' -- input and output names must all be distinct.");
                }
            }
        };
        CheckNames(Inputs, "Input");
        CheckNames(Outputs, "Output");

        if (Outputs.empty())
        {
            OutProblems.push_back("No outputs declared -- add at least one output for this node to produce a value.");
        }

        FString Directive;
        if (FindPreprocessorDirective(Code, Directive))
        {
            OutProblems.push_back(FString("Preprocessor directives are not allowed ('") + Directive + "'). They escape this node's scope.");
        }

        const FBalanceResult Balance = CheckBalance(Code);
        if (!Balance.bBalanced)
        {
            OutProblems.push_back(Balance.Problem);
        }

        return OutProblems.empty();
    }

    void CMaterialExpression_CustomSlang::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        // Compose with any enclosing material-function inline prefix so nested use never collides.
        const FString Prefix = Compiler.GetCurrentInlinePrefix() + "CS" + Format("{}", GetNodeID()) + "_";

        auto EmitError = [&](const FString& Description)
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Custom Slang";
            NodeError.Description = Description;
            NodeError.Node        = this;
            Compiler.AddError(NodeError);
        };

        // Canonical outputs first, always emitted and always initialized: downstream nodes read these,
        // so even a rejected node leaves a compilable shader instead of dangling references.
        TVector<FString> OutVars;
        OutVars.reserve(CustomOutputPins.size());
        for (size_t i = 0; i < CustomOutputPins.size(); ++i)
        {
            CMaterialOutput* Pin = CustomOutputPins[i];
            const EMaterialInputType T = Pin->InputType;
            const FString Var = Prefix + "out" + Format("{}", i);

            Compiler.AddRaw(FMaterialCompiler::GetHLSLTypeName(T) + " " + Var + " = " + ZeroLiteral(T) + ";\n");
            Pin->ResolvedVar = Var;
            OutVars.push_back(Var);
        }

        TVector<FString> Problems;
        if (!Validate(Problems))
        {
            for (const FString& Problem : Problems)
            {
                EmitError(Problem);
            }
            return;
        }

        if (Code.empty())
        {
            EmitError("Node has no code; its outputs stay at zero.");
            return;
        }

        // Resolve upstream expressions BEFORE opening the block so nothing upstream lands inside our scope.
        TVector<FString> ArgExprs;
        ArgExprs.reserve(CustomInputPins.size());
        for (size_t i = 0; i < CustomInputPins.size(); ++i)
        {
            // An unconnected pin falls back to its declared default expression rather than zero, so
            // the common "just give me UV" case needs nothing wired up.
            // The stage aliases are all float-typed, so none of them is a sensible fallback for a texture
            // handle -- casting UV0 to a uint would silently name some unrelated texture. An unconnected
            // handle input falls back to the same neutral index the TextureHandle node's error path uses.
            const bool bHandle = i < Inputs.size()
                              && ToMaterialInputType(Inputs[i].Type) == EMaterialInputType::TextureHandle;

            const FString Fallback = bHandle
                ? ZeroLiteral(EMaterialInputType::TextureHandle)
                : (i < Inputs.size() ? DefaultExpressionFor(Inputs[i].Default) : FString("0.0"));

            const FMaterialCompiler::FInputValue Value = Compiler.GetTypedInputValue(CustomInputPins[i], Fallback);
            ArgExprs.push_back(Value.Value + GetSwizzleForMask(Value.Mask));
        }

        Compiler.AddRaw(FString("// ---- custom slang: ") + Title.c_str() + " ----\n");
        Compiler.AddRaw("{\n");

        // Inputs as const locals under the author's own names. The cast coerces whatever width was wired in
        // to the declared width, so the body always sees exactly the type it declared.
        for (size_t i = 0; i < CustomInputPins.size() && i < Inputs.size(); ++i)
        {
            const EMaterialInputType T = ToMaterialInputType(Inputs[i].Type);
            const FString TypeName = FMaterialCompiler::GetHLSLTypeName(T);
            Compiler.AddRaw("const " + TypeName + " " + Inputs[i].Name.ToString() + " = (" + TypeName + ")(" + ArgExprs[i] + ");\n");
        }

        // Outputs as zero-initialized locals the body assigns to.
        for (const FCustomSlangPin& Out : Outputs)
        {
            const EMaterialInputType T = ToMaterialInputType(Out.Type);
            Compiler.AddRaw(FMaterialCompiler::GetHLSLTypeName(T) + " " + Out.Name.ToString() + " = " + ZeroLiteral(T) + ";\n");
        }

        // The body gets its own nested scope so the author's locals can't collide with the
        // input/output declarations above.
        Compiler.AddRaw("{\n");
        Compiler.AddRaw(Code);
        Compiler.AddRaw("\n}\n");

        // Copy back into the canonical outputs, then close the block.
        for (size_t i = 0; i < OutVars.size() && i < Outputs.size(); ++i)
        {
            Compiler.AddRaw(OutVars[i] + " = " + Outputs[i].Name.ToString() + ";\n");
        }

        Compiler.AddRaw("}\n");
    }
}
