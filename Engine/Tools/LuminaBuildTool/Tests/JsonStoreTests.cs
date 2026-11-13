using LuminaBuildTool.Core;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class JsonStoreTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("JsonStoreTests").FullName;

    private sealed record Record
    {
        public string Name { get; init; } = "";
        public int Count { get; init; }
    }

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    [Fact]
    public void SaveThenLoad_RoundTripsRecord()
    {
        string FilePath = Path.Combine(TempDirectory, "Record.json");
        Record Original = new() { Name = "Widget", Count = 7 };

        JsonStore.Save(FilePath, Original);
        Record? Loaded = JsonStore.Load<Record>(FilePath);

        Assert.Equal(Original, Loaded);
    }

    [Fact]
    public void Load_ReturnsNullForMissingFile()
    {
        string FilePath = Path.Combine(TempDirectory, "Missing.json");

        Assert.Null(JsonStore.Load<Record>(FilePath));
    }

    [Fact]
    public void Load_ReturnsNullForCorruptJson()
    {
        string FilePath = Path.Combine(TempDirectory, "Corrupt.json");
        File.WriteAllText(FilePath, "{ \"Name\": \"Widget\", \"Count\": ");

        Assert.Null(JsonStore.Load<Record>(FilePath));
    }

    [Fact]
    public void Save_CreatesMissingDestinationDirectory()
    {
        string FilePath = Path.Combine(TempDirectory, "Nested", "Deeper", "Record.json");

        JsonStore.Save(FilePath, new Record { Name = "Widget", Count = 1 });

        Assert.True(File.Exists(FilePath));
    }

    [Fact]
    public void Save_WritesNoByteOrderMark()
    {
        string FilePath = Path.Combine(TempDirectory, "NoBom.json");

        JsonStore.Save(FilePath, new Record { Name = "Widget", Count = 1 });

        byte[] Bytes = File.ReadAllBytes(FilePath);
        Assert.False(Bytes.Length >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF);
    }
}
