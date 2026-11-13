using LuminaBuildTool.Core;
using LuminaBuildTool.Execution;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class ActionHistoryTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("ActionHistoryTests").FullName;

    private readonly string HistoryFile;

    public ActionHistoryTests()
    {
        HistoryFile = Path.Combine(TempDirectory, "ActionHistory.json");
    }

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    private string WriteOutput(string Name)
    {
        string OutputPath = Path.Combine(TempDirectory, Name);
        File.WriteAllText(OutputPath, string.Empty);
        return OutputPath;
    }

    [Fact]
    public void HasCommandChanged_FalseAfterRecordCommandTrueAfterForget()
    {
        string Output = WriteOutput("Output.obj");
        ActionHistory History = ActionHistory.Load(HistoryFile);

        Assert.True(History.HasCommandChanged(Output, "CommandKey"));

        History.RecordCommand(Output, "CommandKey");
        Assert.False(History.HasCommandChanged(Output, "CommandKey"));
        Assert.True(History.HasCommandChanged(Output, "OtherKey"));

        History.Forget(Output);
        Assert.True(History.HasCommandChanged(Output, "CommandKey"));
    }

    [Fact]
    public void HasInputsChanged_FalseAfterRecordInputFingerprintTrueAfterForget()
    {
        string Output = WriteOutput("Output.obj");
        ActionHistory History = ActionHistory.Load(HistoryFile);

        Assert.True(History.HasInputsChanged(Output, "Fingerprint"));

        History.RecordInputFingerprint(Output, "Fingerprint");
        Assert.False(History.HasInputsChanged(Output, "Fingerprint"));
        Assert.True(History.HasInputsChanged(Output, "OtherFingerprint"));

        History.Forget(Output);
        Assert.True(History.HasInputsChanged(Output, "Fingerprint"));
    }

    [Fact]
    public void SaveLoad_RoundTripsCommandKeyAndInputFingerprint()
    {
        string Output = WriteOutput("Output.obj");
        ActionHistory History = ActionHistory.Load(HistoryFile);
        History.RecordCommand(Output, "CommandKey");
        History.RecordInputFingerprint(Output, "Fingerprint");
        History.Save();

        ActionHistory Reloaded = ActionHistory.Load(HistoryFile);
        Assert.False(Reloaded.HasCommandChanged(Output, "CommandKey"));
        Assert.False(Reloaded.HasInputsChanged(Output, "Fingerprint"));
    }

    [Fact]
    public void Load_DropsEntryForMissingOutputAndMarksDirtyForNextSave()
    {
        string KeptOutput = WriteOutput("Kept.obj");
        string MissingOutput = Path.Combine(TempDirectory, "Missing.obj");

        ActionHistory Original = ActionHistory.Load(HistoryFile);
        Original.RecordCommand(KeptOutput, "CommandKey");
        Original.RecordCommand(MissingOutput, "CommandKey");
        Original.Save();

        File.Delete(MissingOutput);

        ActionHistory Reloaded = ActionHistory.Load(HistoryFile);
        Assert.True(Reloaded.HasCommandChanged(MissingOutput, "CommandKey"));
        Assert.False(Reloaded.HasCommandChanged(KeptOutput, "CommandKey"));

        Reloaded.Save();

        ActionHistoryData? Saved = JsonStore.Load<ActionHistoryData>(HistoryFile);
        Assert.NotNull(Saved);
        Assert.DoesNotContain(MissingOutput, Saved.CommandKeys.Keys);
        Assert.Contains(KeptOutput, Saved.CommandKeys.Keys);
    }

    [Fact]
    public void Load_MergesSeveralFilesAndSaveRoutesEachEntryBackByResolver()
    {
        string EngineFile = Path.Combine(TempDirectory, "Engine.json");
        string ProjectFile = Path.Combine(TempDirectory, "Project.json");

        string EngineOutput = WriteOutput("Engine.obj");
        string ProjectOutput = WriteOutput("Project.obj");

        // An entry already on the engine side, as if an earlier engine-only build recorded it.
        ActionHistory Seed = ActionHistory.Load(EngineFile);
        Seed.RecordCommand(EngineOutput, "EngineCommand");
        Seed.Save();

        string ResolveFile(string Output) => string.Equals(Output, EngineOutput, StringComparison.OrdinalIgnoreCase)
            ? EngineFile
            : ProjectFile;

        ActionHistory Merged = ActionHistory.Load(new[] { ProjectFile, EngineFile }, ResolveFile);

        // The engine-only record is visible without a project build having ever run, so it is not treated as outdated.
        Assert.False(Merged.HasCommandChanged(EngineOutput, "EngineCommand"));

        Merged.RecordCommand(ProjectOutput, "ProjectCommand");
        Merged.Save();

        ActionHistoryData? SavedEngine = JsonStore.Load<ActionHistoryData>(EngineFile);
        ActionHistoryData? SavedProject = JsonStore.Load<ActionHistoryData>(ProjectFile);

        Assert.NotNull(SavedEngine);
        Assert.NotNull(SavedProject);
        Assert.Contains(EngineOutput, SavedEngine.CommandKeys.Keys);
        Assert.DoesNotContain(ProjectOutput, SavedEngine.CommandKeys.Keys);
        Assert.Contains(ProjectOutput, SavedProject.CommandKeys.Keys);
        Assert.DoesNotContain(EngineOutput, SavedProject.CommandKeys.Keys);
    }
}
