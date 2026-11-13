using System.Text.Json;
using LuminaBuildTool.Core;
using LuminaBuildTool.Execution;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Toolchain;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class ActionGraphTests : IDisposable
{
    private sealed class FakeToolchain(List<BuildAction> Actions) : IToolchain
    {
        public string Name => "Fake";

        public string Description => "Fake";

        public string VersionKey => "Fake";

        public string ProjectToolsetName => "Fake";

        public List<BuildAction> CreateCompileActions(BuildTarget Target, BuildModule Module) => Actions;

        public BuildAction? CreateLinkAction(BuildTarget Target, BuildModule Module, IReadOnlyList<BuildAction> CompileActions) => null;
    }

    private readonly string TempDirectory = Directory.CreateTempSubdirectory("ActionGraphTests").FullName;

    private readonly ActionHistory History;

    private readonly DependencyCache Dependencies;

    public ActionGraphTests()
    {
        History = ActionHistory.Load(Path.Combine(TempDirectory, "History.json"));
        Dependencies = DependencyCache.Load(Path.Combine(TempDirectory, "Dependencies.json"));
    }

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    private FileItem WriteFile(string Name, string Contents)
    {
        FileItem Item = FileItem.Get(Path.Combine(TempDirectory, Name));
        File.WriteAllText(Item.Location, Contents);
        Item.Invalidate();
        return Item;
    }

    private BuildAction Compile(string Source, string Header)
    {
        BuildAction Action = new(ActionType.Compile, "Test") { StatusText = Source, Arguments = "/c " + Source };
        Action.PrerequisiteItems.Add(WriteFile(Source, "source"));
        Action.ProducedItems.Add(WriteFile(Path.ChangeExtension(Source, ".obj"), "object"));

        string Includes = JsonSerializer.Serialize(new { Data = new { Includes = new[] { WriteFile(Header, "header").Location } } });
        Dependencies.RecordFromCompilerOutput(
            Action.ProducedItems[0].Location, WriteFile(Source + ".json", Includes).Location, DependencyListFormat.MsvcSourceDependencies);

        return Action;
    }

    private BuildAction Link(params BuildAction[] Objects)
    {
        BuildAction Action = new(ActionType.Link, "Test") { StatusText = "Test.dll" };
        Action.PrerequisiteItems.AddRange(Objects.Select(Object => Object.ProducedItems[0]));
        Action.ProducedItems.Add(WriteFile("Test.dll", "binary"));
        return Action;
    }

    internal static ActionGraph BuildGraph(params BuildAction[] Actions)
    {
        TestTarget Fixture = new("ActionGraphTest");
        Fixture.AddModule("Test");
        return ActionGraph.Build(Fixture.Target, new FakeToolchain(Actions.ToList()));
    }

    private ActionGraph BuildUpToDate(params BuildAction[] Actions)
    {
        ActionGraph Graph = BuildGraph(Actions);

        foreach (BuildAction Action in Actions)
        {
            History.RecordCommand(Action.HistoryKey, Action.GetCommandKey());
            History.RecordInputFingerprint(Action.HistoryKey, ActionGraph.ComputeInputFingerprint(Action, Dependencies));
        }

        Assert.Empty(Graph.DetermineOutdatedActions(History, Dependencies));
        return Graph;
    }

    [Fact]
    public void MissingOutputMarksActionAndDependentsOutdated()
    {
        BuildAction First = Compile("First.cpp", "First.h");
        BuildAction Second = Compile("Second.cpp", "Second.h");
        BuildAction Linked = Link(First, Second);
        ActionGraph Graph = BuildUpToDate(First, Second, Linked);

        File.Delete(First.ProducedItems[0].Location);
        First.ProducedItems[0].Invalidate();

        Assert.True(ActionGraph.IsOutdated(First, History, Dependencies, out string Reason));
        Assert.Equal("'First.obj' does not exist", Reason);
        Assert.Equal(new[] { First, Linked }, Graph.DetermineOutdatedActions(History, Dependencies));
    }

    [Fact]
    public void ChangedCommandKeyPropagatesToDependents()
    {
        BuildAction First = Compile("First.cpp", "First.h");
        BuildAction Second = Compile("Second.cpp", "Second.h");
        BuildAction Linked = Link(First, Second);
        ActionGraph Graph = BuildUpToDate(First, Second, Linked);

        First.Arguments += " /O2";

        Assert.True(ActionGraph.IsOutdated(First, History, Dependencies, out string Reason));
        Assert.Equal("command line changed", Reason);
        Assert.False(ActionGraph.IsOutdated(Linked, History, Dependencies, out _));
        Assert.Equal(new[] { First, Linked }, Graph.DetermineOutdatedActions(History, Dependencies));
    }

    [Fact]
    public void HeaderEditRebuildsOnlyItsIncluders()
    {
        BuildAction First = Compile("First.cpp", "First.h");
        BuildAction Second = Compile("Second.cpp", "Second.h");
        BuildAction Linked = Link(First, Second);
        ActionGraph Graph = BuildUpToDate(First, Second, Linked);

        WriteFile("First.h", "header after an edit");

        Assert.True(ActionGraph.IsOutdated(First, History, Dependencies, out _));
        Assert.False(ActionGraph.IsOutdated(Second, History, Dependencies, out _));
        Assert.False(ActionGraph.IsOutdated(Linked, History, Dependencies, out _));
        Assert.Equal(new[] { First, Linked }, Graph.DetermineOutdatedActions(History, Dependencies));
    }

    [Fact]
    public void OrderDependencyCycleThrowsWithCyclePath()
    {
        BuildAction Lead = Compile("Lead.cpp", "Lead.h");
        BuildAction First = Compile("First.cpp", "First.h");
        BuildAction Second = Compile("Second.cpp", "Second.h");
        Lead.OrderDependencies.Add(First);
        First.OrderDependencies.Add(Second);
        Second.OrderDependencies.Add(First);

        BuildException Error = Assert.Throws<BuildException>(() => BuildGraph(Lead, First, Second));

        Assert.Equal("Cycle in the action graph: First.cpp -> Second.cpp -> First.cpp", Error.Message);
    }
}
