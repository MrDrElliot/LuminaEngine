using LuminaBuildTool.Core;
using LuminaBuildTool.Execution;
using LuminaBuildTool.Graph;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class ActionExecutorTests
{
    private sealed class RecordingOperation(string Name, List<string> Events, Action? Body) : BuildOperation
    {
        public override string GetIdentity() => Name;

        public override void Execute()
        {
            lock (Events)
            {
                Events.Add("+" + Name);
            }

            Body?.Invoke();

            lock (Events)
            {
                Events.Add("-" + Name);
            }
        }
    }

    private readonly List<string> Events = new();

    private readonly string MissingFile = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + ".json");

    private BuildAction Fake(string Name, bool bCanExecuteInParallel = true, Action? Body = null, params BuildAction[] DependsOn)
    {
        BuildAction Created = new(ActionType.Copy, "Test")
        {
            StatusText = Name,
            bCanExecuteInParallel = bCanExecuteInParallel,
            Operation = new RecordingOperation(Name, Events, Body),
        };

        Created.OrderDependencies.AddRange(DependsOn);
        return Created;
    }

    private static void Fail() => throw new IOException("fake failure");

    private Task<ExecutionResult> Execute(ActionGraph Graph, int MaxParallelism, bool bStopOnFirstError, params BuildAction[] Outdated)
    {
        ActionExecutor Executor = new(Graph, ActionHistory.Load(MissingFile), DependencyCache.Load(MissingFile), MaxParallelism, bStopOnFirstError);
        return Executor.ExecuteAsync(Outdated, CancellationToken.None);
    }

    [Fact]
    public async Task FailedDependencyBlocksDiamondJoin()
    {
        BuildAction Root = Fake("A");
        BuildAction Failing = Fake("B", Body: Fail, DependsOn: [Root]);
        BuildAction Sibling = Fake("C", DependsOn: [Root]);
        BuildAction Join = Fake("D", DependsOn: [Failing, Sibling]);
        BuildAction[] Actions = [Root, Failing, Sibling, Join];

        ExecutionResult Result = await Execute(ActionGraphTests.BuildGraph(Actions), 4, bStopOnFirstError: false, Actions);

        Assert.Equal(2, Result.ActionsExecuted);
        Assert.Equal(1, Result.ActionsFailed);
        Assert.Equal(1, Result.ActionsBlocked);
        Assert.DoesNotContain("+D", Events);
    }

    [Fact]
    public async Task ExclusiveActionDrainsParallelSlotsAndRunsAlone()
    {
        BuildAction Running = Fake("P1", Body: () => Thread.Sleep(50));
        BuildAction Exclusive = Fake("X", bCanExecuteInParallel: false);
        BuildAction[] Actions = [Running, Exclusive, Fake("P2"), Fake("P3")];

        ExecutionResult Result = await Execute(ActionGraphTests.BuildGraph(Actions), 2, bStopOnFirstError: false, Actions);

        Assert.Equal(4, Result.ActionsExecuted);
        Assert.Equal(new[] { "+P1", "-P1", "+X", "-X" }, Events.Take(4));
        Assert.Equal(new[] { "+P2", "+P3", "-P2", "-P3" }, Events.Skip(4).Order(StringComparer.Ordinal));
    }

    [Fact]
    public async Task StopOnFirstErrorSchedulesNothingAfterFailure()
    {
        BuildAction[] Actions = [Fake("Failing", Body: Fail), Fake("A"), Fake("B")];

        ExecutionResult Result = await Execute(ActionGraphTests.BuildGraph(Actions), 1, bStopOnFirstError: true, Actions);

        Assert.Equal(1, Result.ActionsFailed);
        Assert.Equal(0, Result.ActionsExecuted);
        Assert.Equal(new[] { "+Failing" }, Events);
    }

    [Fact]
    public async Task MutuallyBlockingOutdatedActionsThrow()
    {
        BuildAction First = Fake("First");
        BuildAction Second = Fake("Second");
        ActionGraph Graph = ActionGraphTests.BuildGraph(First, Second);

        // ActionGraph.Build rejects cycles, so the edges are added after wiring.
        ((List<BuildAction>)Graph.GetDependencies(First)).Add(Second);
        ((List<BuildAction>)Graph.GetDependencies(Second)).Add(First);

        await Assert.ThrowsAsync<BuildException>(() => Execute(Graph, 2, bStopOnFirstError: false, First, Second));
        Assert.Empty(Events);
    }
}
