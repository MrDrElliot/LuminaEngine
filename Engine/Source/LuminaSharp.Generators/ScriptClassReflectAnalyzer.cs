using System.Collections.Immutable;
using System.Linq;
using LuminaSharp.ScriptProperties;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace LuminaSharp.Generators;

// The engine registers only a class that carries [Reflect], so a forgotten one is flagged where it is written instead of in the editor log.
[Generator]
public sealed class ScriptClassReflectAnalyzer : IIncrementalGenerator
{
    private static readonly DiagnosticDescriptor MissingReflect = new(
        id: "LUM0112",
        title: "Script class needs [Reflect]",
        messageFormat: "'{0}' derives from the engine class '{1}' but has no [Reflect], so the engine never registers it. Add [Reflect], or make it abstract to keep it a plain C# base.",
        category: "LuminaSharp",
        defaultSeverity: DiagnosticSeverity.Warning,
        isEnabledByDefault: true);

    private sealed record Model(string Name, string NativeBase, SourceSpot Where);

    public void Initialize(IncrementalGeneratorInitializationContext Context)
    {
        IncrementalValueProvider<ImmutableArray<Model?>> Classes = Context.SyntaxProvider
            .CreateSyntaxProvider(
                static (Node, _) => Node is ClassDeclarationSyntax { BaseList: not null },
                static (Syntax, _) => Syntax.SemanticModel.GetDeclaredSymbol((ClassDeclarationSyntax)Syntax.Node) is INamedTypeSymbol { IsAbstract: false } Symbol
                    && NativeBaseOf(Symbol) is { } NativeBase && ScriptPropertyClassifier.FindAttribute(Symbol, ScriptPropertyTypeNames.ReflectAttribute) == null
                        ? new Model(Symbol.Name, NativeBase.Name, SourceSpot.Of(Symbol))
                        : null)
            .Where(static Class => Class != null)
            .Collect();

        Context.RegisterSourceOutput(Classes.Combine(Context.CompilationProvider), static (Output, Input) =>
        {
            foreach (Model Class in Input.Left.Distinct().Cast<Model>())
            {
                Output.ReportDiagnostic(Diagnostic.Create(MissingReflect, Class.Where.ToLocation(Input.Right), Class.Name, Class.NativeBase));
            }
        });
    }

    // The nearest generated wrapper above Symbol, mirroring TypeLibrary.NativeClassBaseOf at runtime.
    private static INamedTypeSymbol? NativeBaseOf(INamedTypeSymbol Symbol)
    {
        if (ScriptPropertyClassifier.FindAttribute(Symbol, ScriptPropertyTypeNames.NativeTypeAttribute) != null)
        {
            return null;
        }
        for (INamedTypeSymbol? Base = Symbol.BaseType; Base != null; Base = Base.BaseType)
        {
            if (ScriptPropertyClassifier.FindAttribute(Base, ScriptPropertyTypeNames.NativeTypeAttribute) != null)
            {
                return Base;
            }
        }
        return null;
    }
}
