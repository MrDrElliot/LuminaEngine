using System.Text;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>The Reflector's input over standard input, one "Keyword value" line per record, module records after their Module line; read by ReflectorInput.cpp.</summary>
public static class ReflectorInput
{
    public const int FormatVersion = 1;

    public static string Write(ReflectionInputDocument Document)
    {
        StringBuilder Out = new();
        Out.Append("LuminaReflector ").Append(FormatVersion).Append('\n');
        Record(Out, "Generation", Document.CodeGenerationVersion.ToString());
        Record(Out, "ReflectorFingerprint", Document.ReflectorFingerprint);
        Record(Out, "CoreFingerprint", Document.CoreReflectionFingerprint);
        Record(Out, "Workspace", Document.WorkspaceName);
        Record(Out, "WorkspacePath", Document.WorkspacePath);

        foreach (ReflectionGeneratorEntry Generator in Document.Generators)
        {
            Record(Out, "Generator", Generator.Library);
            Record(Out, "GeneratorFingerprint", Generator.Fingerprint);
            Record(Out, "GeneratorRules", Generator.RulesFile);
            Records(Out, "GeneratorOption", Generator.Options.Select(Option => $"{Option.Key}={Option.Value}"));
        }

        foreach (ReflectionProjectEntry Project in Document.Projects)
        {
            Out.Append('\n');
            Record(Out, "Module", Project.Name);
            Record(Out, "Path", Project.Path);
            OptionalRecord(Out, "GeneratedDir", Project.GeneratedDir);
            OptionalRecord(Out, "PrecompiledHeader", Project.PrecompiledHeader);
            OptionalRecord(Out, "CSharpBindingsDir", Project.CSharpBindingsDir);
            if (Project.RouteTypeBindings)
            {
                Out.Append("RouteTypeBindings\n");
            }
            if (Project.ReferenceOnly)
            {
                Out.Append("ReferenceOnly\n");
            }
            Records(Out, "Include", Project.IncludeDirs);
            Records(Out, "Define", Project.Definitions);
            Records(Out, "ForceInclude", Project.ForceIncludes);
            Records(Out, "Header", Project.Files);
        }

        Out.Append("End\n");
        return Out.ToString();
    }

    private static void Record(StringBuilder Out, string Keyword, string Value)
    {
        if (Value.Contains('\n') || Value.Contains('\r'))
        {
            throw new BuildException($"Reflector input {Keyword} '{Value}' contains a line break, which the line format cannot carry.");
        }
        Out.Append(Keyword).Append(' ').Append(Value).Append('\n');
    }

    private static void OptionalRecord(StringBuilder Out, string Keyword, string Value)
    {
        if (!string.IsNullOrEmpty(Value))
        {
            Record(Out, Keyword, Value);
        }
    }

    private static void Records(StringBuilder Out, string Keyword, IEnumerable<string> Values)
    {
        foreach (string Value in Values)
        {
            Record(Out, Keyword, Value);
        }
    }
}
