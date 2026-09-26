using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Text;
using System.Threading;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;
using LuminaSharp.ScriptProperties;

namespace LuminaSharp.Generators;

// A typed entry point per [ScriptFunction], so dispatch neither reflects nor boxes an argument per call.
[Generator]
public sealed class ScriptFunctionInvokerGenerator : IIncrementalGenerator
{
    private const string ScriptFunctionAttribute = "LuminaSharp.ScriptFunctionAttribute";

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        IncrementalValuesProvider<Model?> Models = context.SyntaxProvider.ForAttributeWithMetadataName(
            ScriptFunctionAttribute,
            predicate: static (Node, _) => Node is MethodDeclarationSyntax,
            transform: static (Context, Token) => BuildModel(Context, Token));

        context.RegisterSourceOutput(Models.Collect(), static (Output, All) => Emit(Output, All));
    }

    private sealed record Model(string OwnerFullyQualified, string OwnerHint, string Function, string Body);

    // Null for anything FrameMarshal would not bind as plain blittable slots, which keeps the reflection path.
    private static Model? BuildModel(GeneratorAttributeSyntaxContext Context, CancellationToken Token)
    {
        if (Context.TargetSymbol is not IMethodSymbol Method || Method.IsStatic || Method.IsGenericMethod)
        {
            return null;
        }

        INamedTypeSymbol Owner = Method.ContainingType;
        SymbolDisplayFormat Qualified = SymbolDisplayFormat.FullyQualifiedFormat;
        string OwnerName = Owner.ToDisplayString(Qualified);

        var Reads = new List<string>();
        var WriteBacks = new List<string>();
        var CallArguments = new List<string>();

        for (int Index = 0; Index < Method.Parameters.Length; ++Index)
        {
            IParameterSymbol Parameter = Method.Parameters[Index];
            if (!TryFrameType(Parameter.Type, Qualified, out string TypeName, out bool bIsBool))
            {
                return null;
            }

            bool bMarshalled = IsMarshalledType(Parameter.Type);

            string Local = "__a" + Index;
            string Slot = "(void*)(__frame + __offsets[" + Index + "])";
            string Address = "__frame + __offsets[" + Index + "]";

            if (Parameter.RefKind == RefKind.Out)
            {
                Reads.Add(TypeName + " " + Local + " = default" + (bMarshalled ? "!" : string.Empty) + ";");
            }
            else if (bMarshalled)
            {
                Reads.Add(TypeName + " " + Local + " = global::LuminaSharp.ElementMarshal.Read<" + TypeName + ">("
                    + Address + ");");
            }
            else if (bIsBool)
            {
                Reads.Add("bool " + Local + " = global::System.Runtime.CompilerServices.Unsafe.ReadUnaligned<byte>("
                    + Slot + ") != 0;");
            }
            else
            {
                Reads.Add(TypeName + " " + Local + " = global::System.Runtime.CompilerServices.Unsafe.ReadUnaligned<"
                    + TypeName + ">(" + Slot + ");");
            }

            if (Parameter.RefKind is RefKind.Out or RefKind.Ref)
            {
                WriteBacks.Add(bMarshalled
                    ? "global::LuminaSharp.ElementMarshal.Write<" + TypeName + ">(" + Address + ", " + Local + ");"
                    : WriteStatement(Slot, Local, TypeName, bIsBool));
            }

            CallArguments.Add(RefPrefix(Parameter.RefKind) + Local);
        }

        string Arguments = string.Join(", ", CallArguments);
        string Invocation = "__target." + Method.Name + "(" + Arguments + ");";
        string ReturnWrite = string.Empty;
        if (!Method.ReturnsVoid)
        {
            if (!TryFrameType(Method.ReturnType, Qualified, out string ReturnType, out bool bReturnIsBool))
            {
                return null;
            }

            string ReturnSlot = "(void*)(__frame + __offsets[" + Method.Parameters.Length + "])";
            string ReturnAddress = "__frame + __offsets[" + Method.Parameters.Length + "]";
            Invocation = ReturnType + " __result = __target." + Method.Name + "(" + Arguments + ");";
            ReturnWrite = IsMarshalledType(Method.ReturnType)
                ? "global::LuminaSharp.ElementMarshal.Write<" + ReturnType + ">(" + ReturnAddress + ", __result);"
                : WriteStatement(ReturnSlot, "__result", ReturnType, bReturnIsBool);
        }

        var Builder = new StringBuilder();
        Builder.Append("        private static void Invoke_").Append(Method.Name)
               .Append("(nint __instance, nint __frame, int* __offsets)\n        {\n            try\n            {\n");
        Builder.Append("                var __target = (").Append(OwnerName).Append(")global::System.Runtime.InteropServices.GCHandle.FromIntPtr(__instance).Target!;\n");
        foreach (string Read in Reads)
        {
            Builder.Append("                ").Append(Read).Append('\n');
        }
        Builder.Append("                ").Append(Invocation).Append('\n');
        foreach (string Write in WriteBacks)
        {
            Builder.Append("                ").Append(Write).Append('\n');
        }
        if (ReturnWrite.Length > 0)
        {
            Builder.Append("                ").Append(ReturnWrite).Append('\n');
        }
        Builder.Append("            }\n            catch (global::System.Exception __e)\n            {\n");
        Builder.Append("                global::LuminaSharp.NativeBindings.LogException(__e);\n");
        Builder.Append("            }\n        }\n");

        string Hint = Owner.ToDisplayString().Replace('.', '_').Replace('<', '_').Replace('>', '_');
        return new Model(OwnerName, Hint, Method.Name, Builder.ToString());
    }

    private static string RefPrefix(RefKind Kind)
    {
        return Kind switch
        {
            RefKind.Out => "out ",
            RefKind.Ref => "ref ",
            RefKind.In => "in ",
            _ => string.Empty,
        };
    }

    private static string WriteStatement(string Slot, string Value, string TypeName, bool bIsBool)
    {
        return bIsBool
            ? "global::System.Runtime.CompilerServices.Unsafe.WriteUnaligned<byte>(" + Slot + ", " + Value
                + " ? (byte)1 : (byte)0);"
            : "global::System.Runtime.CompilerServices.Unsafe.WriteUnaligned<" + TypeName + ">(" + Slot + ", "
                + Value + ");";
    }

    // The shared classifier decides the kind, and this narrows it to the slots a frame binds without a token.
    private static bool TryFrameType(ITypeSymbol Type, SymbolDisplayFormat Qualified, out string Name, out bool bIsBool)
    {
        Name = string.Empty;
        bIsBool = Type.SpecialType == SpecialType.System_Boolean;

        if (Type is IPointerTypeSymbol || IsStringHandle(Type))
        {
            return false;
        }

        switch (ScriptPropertyClassifier.Classify(Type).Access)
        {
            case EScriptAccess.Blittable:
            case EScriptAccess.Enum:
            case EScriptAccess.String:
            case EScriptAccess.Object:
                Name = bIsBool ? "bool"
                    : Type.SpecialType == SpecialType.System_String ? "string"
                    : Type.ToDisplayString(Qualified);
                return true;

            default:
                return false;
        }
    }

    // A blittable slot folds back to the same unaligned read, so every kind can go through one call.
    private static bool IsMarshalledType(ITypeSymbol Type)
    {
        if (IsStringHandle(Type))
        {
            return false;
        }

        EScriptAccess Access = ScriptPropertyClassifier.Classify(Type).Access;
        return Access is EScriptAccess.String or EScriptAccess.Object;
    }

    // A frame binds a managed string but not the FString handle, which the property classifier calls one kind.
    private static bool IsStringHandle(ITypeSymbol Type)
    {
        return Type.ToDisplayString() == ScriptPropertyTypeNames.FString;
    }

    private static void Emit(SourceProductionContext Output, ImmutableArray<Model?> All)
    {
        IEnumerable<Model> Models = All.Where(M => M is not null).Select(M => M!);

        foreach (IGrouping<string, Model> Group in Models.GroupBy(M => M.OwnerFullyQualified))
        {
            // An overloaded name is refused at describe time, so emitting for one would not compile anyway.
            List<Model> Entries = Group
                .GroupBy(M => M.Function)
                .Where(G => G.Count() == 1)
                .Select(G => G.First())
                .ToList();
            if (Entries.Count == 0)
            {
                continue;
            }

            Model First = Entries[0];
            var Builder = new StringBuilder();
            Builder.Append("// <auto-generated/>\n#nullable enable\n");
            Builder.Append("namespace LuminaSharp.Generated\n{\n");
            Builder.Append("    internal static unsafe class ScriptInvokers_").Append(First.OwnerHint).Append('\n');
            Builder.Append("    {\n");

            foreach (Model Entry in Entries)
            {
                Builder.Append(Entry.Body);
            }

            Builder.Append("        [global::System.Runtime.CompilerServices.ModuleInitializer]\n");
            Builder.Append("        internal static void Register()\n        {\n");
            foreach (Model Entry in Entries)
            {
                Builder.Append("            global::LuminaSharp.ScriptInvokerRegistry.Register(typeof(")
                       .Append(Entry.OwnerFullyQualified).Append("), \"").Append(Entry.Function)
                       .Append("\", (nint)(delegate* managed<nint, nint, int*, void>)&Invoke_")
                       .Append(Entry.Function).Append(");\n");
            }
            Builder.Append("        }\n    }\n}\n");

            Output.AddSource("ScriptInvokers_" + First.OwnerHint + ".g.cs",
                SourceText.From(Builder.ToString(), Encoding.UTF8));
        }
    }
}
