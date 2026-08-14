using System.Text;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>Refuses to build a module graph whose edges contradict the layering the rules declare.</summary>
public static class ModuleLayerCheck
{
    public static void Verify(BuildTarget Target)
    {
        if (!Target.Rules.bEnforceModuleLayering)
        {
            Log.Verbose("Module layering checks are disabled for target '{0}'", Target.Name);
            return;
        }

        List<string> Violations = new();

        foreach (BuildModule Module in Target.Modules)
        {
            // Build-order edges say build this first, not link it; a module may wait on a Program.
            foreach (BuildModule Dependency in Module.AllDependencies.Distinct())
            {
                CheckVendoredIndependence(Module, Dependency, Violations);
            }
        }

        CheckForbiddenDependencies(Target, Violations);

        if (Violations.Count == 0)
        {
            return;
        }

        StringBuilder Message = new();
        Message.AppendLine($"Module layering violated in target '{Target.Name}' ({Violations.Count}):");

        foreach (string Violation in Violations)
        {
            Message.AppendLine("  " + Violation);
        }

        Message.Append(
            "Fix the dependency, or if the layering itself is wrong, change the rule that declares it. "
            + "A target that genuinely has to build anyway sets bEnforceModuleLayering to false in its own rules.");

        throw new BuildException(Message.ToString());
    }

    // Host type is deliberately unchecked: a Build.cs is evaluated per target type, so an editor-only
    // dependency simply does not exist in a Game resolution. ResolveModule rejects the real case.

    /// <summary>Vendored code must not depend on ours.</summary>
    private static void CheckVendoredIndependence(BuildModule Module, BuildModule Dependency, List<string> Violations)
    {
        if (!Module.Rules.bIsThirdParty || Dependency.Rules.bIsThirdParty)
        {
            return;
        }

        Violations.Add(
            $"{Module.Name} is third party and depends on {Dependency.Name}, which is ours. "
            + "Vendored code has to stand alone so it can be replaced by the next version of itself.");
    }

    /// <summary>Applies declared rules across the closure, so routing an edge through a module cannot evade it.</summary>
    private static void CheckForbiddenDependencies(BuildTarget Target, List<string> Violations)
    {
        foreach (ForbiddenDependency Rule in Target.Rules.ForbiddenDependencies)
        {
            BuildModule? Module = Target.Modules
                .FirstOrDefault(M => string.Equals(M.Name, Rule.ModuleName, StringComparison.OrdinalIgnoreCase));

            // A rule naming a module this target does not build is satisfied, not broken. Rules are
            // declared once for every target and each one builds a different subset.
            if (Module is null)
            {
                continue;
            }

            List<BuildModule>? Path = FindPath(Module, Rule.DependencyName);

            if (Path is null)
            {
                continue;
            }

            Violations.Add(
                $"{string.Join(" -> ", Path.Select(M => M.Name))}: {Rule.Reason}");
        }
    }

    /// <summary>Shortest dependency path from a module to a named one, or null when it cannot reach it.</summary>
    private static List<BuildModule>? FindPath(BuildModule From, string ToName)
    {
        Dictionary<BuildModule, BuildModule?> CameFrom = new() { [From] = null };
        Queue<BuildModule> Frontier = new();
        Frontier.Enqueue(From);

        while (Frontier.Count > 0)
        {
            BuildModule Current = Frontier.Dequeue();

            if (Current != From && string.Equals(Current.Name, ToName, StringComparison.OrdinalIgnoreCase))
            {
                List<BuildModule> Path = new();

                for (BuildModule? Step = Current; Step is not null; Step = CameFrom[Step])
                {
                    Path.Add(Step);
                }

                Path.Reverse();
                return Path;
            }

            foreach (BuildModule Next in Current.AllDependencies)
            {
                if (CameFrom.TryAdd(Next, Current))
                {
                    Frontier.Enqueue(Next);
                }
            }
        }

        return null;
    }
}
