using System.Collections.Concurrent;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Execution;

public sealed class ActionHistoryData
{
    /// <summary>Output file to the command key that produced it.</summary>
    public Dictionary<string, string> CommandKeys { get; set; } = new();

    /// <summary>Output file to a fingerprint of every input seen when it was last produced successfully.</summary>
    public Dictionary<string, string> InputFingerprints { get; set; } = new();
}

/// <summary>Remembers the exact command that produced each output.</summary>
public sealed class ActionHistory
{
    private readonly string HistoryFile;

    private readonly ConcurrentDictionary<string, string> CommandKeys = new(StringComparer.OrdinalIgnoreCase);

    private readonly ConcurrentDictionary<string, string> InputFingerprints = new(StringComparer.OrdinalIgnoreCase);

    private bool bDirty;

    private ActionHistory(string HistoryFile)
    {
        this.HistoryFile = HistoryFile;
    }

    public static ActionHistory Load(string HistoryFile)
    {
        ActionHistory History = new(HistoryFile);
        ActionHistoryData? Data = JsonStore.Load<ActionHistoryData>(HistoryFile);

        if (Data is not null)
        {
            foreach ((string Output, string Key) in Data.CommandKeys)
            {
                History.CommandKeys[Output] = Key;
            }

            foreach ((string Output, string Fingerprint) in Data.InputFingerprints)
            {
                History.InputFingerprints[Output] = Fingerprint;
            }
        }

        return History;
    }

    public void Save()
    {
        if (!bDirty)
        {
            return;
        }

        ActionHistoryData Data = new();

        foreach ((string Output, string Key) in CommandKeys.OrderBy(P => P.Key, StringComparer.OrdinalIgnoreCase))
        {
            Data.CommandKeys[Output] = Key;
        }

        foreach ((string Output, string Fingerprint) in InputFingerprints.OrderBy(P => P.Key, StringComparer.OrdinalIgnoreCase))
        {
            Data.InputFingerprints[Output] = Fingerprint;
        }

        JsonStore.Save(HistoryFile, Data);
        bDirty = false;
    }

    /// <summary>True when the recorded command for this output differs from the one about to run.</summary>
    public bool HasCommandChanged(string OutputFile, string CommandKey)
    {
        return !CommandKeys.TryGetValue(OutputFile, out string? Recorded) || Recorded != CommandKey;
    }

    public void RecordCommand(string OutputFile, string CommandKey)
    {
        CommandKeys[OutputFile] = CommandKey;
        bDirty = true;
    }

    /// <summary>Records the exact set of inputs this output was last produced from.</summary>
    public void RecordInputFingerprint(string OutputFile, string Fingerprint)
    {
        InputFingerprints[OutputFile] = Fingerprint;
        bDirty = true;
    }

    /// <summary>True when this output has no recorded inputs, or they are not the ones on disk now.</summary>
    public bool HasInputsChanged(string OutputFile, string Fingerprint)
    {
        return !InputFingerprints.TryGetValue(OutputFile, out string? Recorded) || Recorded != Fingerprint;
    }

    public void Forget(string OutputFile)
    {
        bool bRemoved = CommandKeys.TryRemove(OutputFile, out _);
        bRemoved |= InputFingerprints.TryRemove(OutputFile, out _);

        if (bRemoved)
        {
            bDirty = true;
        }
    }
}
