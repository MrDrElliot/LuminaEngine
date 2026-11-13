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
}
