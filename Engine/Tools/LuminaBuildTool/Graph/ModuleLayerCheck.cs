using System.Text;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>
/// Refuses to build a module graph whose edges contradict the layering the rules declare.
/// </summary>
/// <remarks>
/// Most of this is checked against declarations that already exist rather than a new set to
/// maintain. A module states which target types it belongs in; a third-party module states that it
/// is vendored. Both are statements about layering that nothing was reading as one, which meant an
/// editor-only module could be pulled into the runtime and the build would say so only much later,
/// in whichever configuration first went looking for it.
///
/// Every violation is reported, not just the first. An architectural drift is usually several edges
/// that arrived together, and fixing them one build at a time is the slowest way to find that out.
/// </remarks>
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
            // Build-order edges are excluded throughout: naming one says this has to be built
            // first, not that anything links or includes it. A module is entitled to wait for a
            // Program it could never depend on, which is exactly how a code generator is used.
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

    // Host type is deliberately not checked here. It reads like the obvious rule, that a dependency
    // must exist everywhere its dependent does, and it is unsound: a Build.cs is evaluated per
    // target type, so a module can name an editor dependency inside a Target.bWithEditor check and
    // the edge simply does not exist in a Game resolution. Lumina does exactly that. What remains
    // once the conditional edges are excluded is the case where a dependency is missing from the
    // target being resolved right now, and ResolveModule already rejects that with a better message.

    /// <summary>
    /// Vendored code must not depend on ours.
    /// </summary>
    /// <remarks>
    /// A third-party module is a copy of someone else's source that we expect to replace wholesale
    /// on the next update. An edge back into an engine module makes that update a merge, and the
    /// engine's own layering then runs through code we do not own.
    /// </remarks>
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

    /// <summary>
    /// Applies the target's own declared rules across the whole closure, so an edge routed through
    /// an intermediate module is still the edge the rule forbids.
    /// </summary>
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

    /// <summary>
    /// Shortest dependency path from a module to a named one, or null when it cannot reach it.
    /// </summary>
    /// <remarks>
    /// Breadth first, so the reported path is the shortest one. A violation is acted on by deleting
    /// an edge, and the shortest path is the one with the fewest candidates to consider.
    /// </remarks>
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
