// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json.Nodes;
using DDM32Plus.Core.Update;

namespace DDM32Plus.Core.Tests;

/// <summary>
/// The self-update swap and rollback on temporary folders with fake processes (nothing is started); the same cases as
/// pc/tests/test_selfupdate.py, plus: the user's data in the install folder (yedekler, settings.json) is never
/// touched.
/// </summary>
public sealed class SelfUpdateTests : IDisposable
{
    private readonly TempDir tmp = new();
    private readonly string app;
    private readonly string staging;
    private readonly string marker;

    public SelfUpdateTests()
    {
        app = MakeApp(tmp.Sub("Programs", "DDM32 Plus"), "old");
        staging = MakeApp(Path.Combine(app, "guncelleme", "app-9.9.9", "DDM32Plus"), "new");
        marker = Path.Combine(app, "guncelleme", "started.json");
        Directory.CreateDirectory(Path.Combine(app, "yedekler", "dm32-20261120"));
        File.WriteAllText(Path.Combine(app, "yedekler", "dm32-20261120", "backup-manifest.json"), "{}");
        File.WriteAllText(Path.Combine(app, "settings.json"), "{\"language\": \"tr\"}");
    }

    public void Dispose() => tmp.Dispose();

    private static string MakeApp(string folder, string tag)
    {
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, "DDM32Plus.exe"), $"exe {tag}");
        File.WriteAllText(Path.Combine(folder, "lib.dll"), $"lib {tag}");
        return folder;
    }

    private static string Read(params string[] p) => File.ReadAllText(Path.Combine(p));

    private void AssertUserDataUntouched()
    {
        Assert.Equal("{}", Read(app, "yedekler", "dm32-20261120", "backup-manifest.json"));
        Assert.Equal("{\"language\": \"tr\"}", Read(app, "settings.json"));
    }

    /// <summary>Fake start/stop: records what would run; the 'new program' writes its marker when told to.</summary>
    private sealed class Procs(bool newStarts = true, bool exited = true) : IUpdateProcesses
    {
        public List<(string Exe, List<string> Args, string Content)> Started { get; } = [];
        public List<object?> Stopped { get; } = [];

        public object? Start(string exe, IReadOnlyList<string> args)
        {
            Started.Add((exe, [.. args], File.ReadAllText(exe)));
            var i = args.ToList().IndexOf(SelfUpdate.CheckArg);
            if (i >= 0 && newStarts)
                SelfUpdate.WriteMarker(args[i + 1], "9.9.9");
            return Started.Count;
        }

        public bool WaitExit() => exited;
        public bool WaitMarker(string m) => File.Exists(m);
        public void Stop(object? handle) => Stopped.Add(handle);
    }

    private SelfUpdate.Outcome Swap(Procs p) => SelfUpdate.Swap(app, staging, p, marker);

    [Fact]
    public void OkKeepsOldForRollback()
    {
        var p = new Procs();
        Assert.Equal(SelfUpdate.Outcome.Ok, Swap(p));
        Assert.Equal("exe new", Read(app, "DDM32Plus.exe"));
        Assert.Equal("lib new", Read(app, "lib.dll"));
        Assert.Equal("exe old", Read(app, "guncelleme", "rollback", "DDM32Plus.exe"));
        Assert.Equal([SelfUpdate.CheckArg, marker], p.Started[^1].Args);
        Assert.Equal("exe new", p.Started[^1].Content);
        AssertUserDataUntouched();
    }

    [Fact]
    public void RollbackWhenNewDoesNotStart()
    {
        var p = new Procs(newStarts: false);
        Assert.Equal(SelfUpdate.Outcome.RolledBack, Swap(p));
        Assert.Equal("exe old", Read(app, "DDM32Plus.exe"));
        Assert.Equal("lib old", Read(app, "lib.dll"));
        Assert.Equal("exe new", Read(app, "guncelleme", "failed", "DDM32Plus.exe"));
        Assert.Equal([1], p.Stopped.Cast<int>());          // the new program was stopped
        Assert.Equal("exe old", p.Started[^1].Content);   // and the old one started again
        Assert.Empty(p.Started[^1].Args);
        AssertUserDataUntouched();
    }

    [Fact]
    public void NewFilesAreRemovedOnRollback()
    {
        File.WriteAllText(Path.Combine(staging, "extra.dll"), "new only");
        Assert.Equal(SelfUpdate.Outcome.RolledBack, Swap(new Procs(newStarts: false)));
        Assert.False(File.Exists(Path.Combine(app, "extra.dll")));
        Assert.Equal("exe old", Read(app, "DDM32Plus.exe"));
    }

    [Fact]
    public void AbortWhenOldProgramKeepsRunning()
    {
        var p = new Procs(exited: false);
        Assert.Equal(SelfUpdate.Outcome.Aborted, Swap(p));
        Assert.Equal("exe old", Read(app, "DDM32Plus.exe"));
        Assert.Empty(p.Started);
        Assert.False(Directory.Exists(Path.Combine(app, "guncelleme", "rollback")));
    }

    [Fact]
    public void CannotMoveFilesAside()
    {
        var p = new Procs();
        using (File.Open(Path.Combine(app, "lib.dll"), FileMode.Open, FileAccess.Read, FileShare.None))
            Assert.Equal(SelfUpdate.Outcome.Aborted, Swap(p));
        Assert.Equal("exe old", Read(app, "DDM32Plus.exe"));
        Assert.Equal("lib old", Read(app, "lib.dll"));
        Assert.Equal("exe old", p.Started[^1].Content);
        AssertUserDataUntouched();
    }

    [Fact]
    public void CopyFailurePutsOldBack()
    {
        var p = new Procs();
        using (File.Open(Path.Combine(staging, "lib.dll"), FileMode.Open, FileAccess.Write, FileShare.None))
            Assert.Equal(SelfUpdate.Outcome.RolledBack, Swap(p));
        Assert.Equal("exe old", Read(app, "DDM32Plus.exe"));
        Assert.Equal("lib old", Read(app, "lib.dll"));
        Assert.Equal("exe old", p.Started[^1].Content);
        AssertUserDataUntouched();
    }

    [Fact]
    public void PreviousRollbackCopyIsReplaced()
    {
        MakeApp(Path.Combine(app, "guncelleme", "rollback"), "older");
        Assert.Equal(SelfUpdate.Outcome.Ok, Swap(new Procs()));
        Assert.Equal("exe old", Read(app, "guncelleme", "rollback", "DDM32Plus.exe"));
    }

    [Fact]
    public void StagingWithoutExe()
    {
        File.Delete(Path.Combine(staging, "DDM32Plus.exe"));
        Assert.Equal(SelfUpdate.Outcome.Aborted, Swap(new Procs()));
        Assert.Equal("exe old", Read(app, "DDM32Plus.exe"));
    }

    [Theory]
    [InlineData("yedekler")]
    [InlineData("settings.json")]
    [InlineData("YEDEKLER")]
    public void StagingThatWouldTouchUserDataIsRefused(string name)
    {
        if (name.EndsWith(".json", StringComparison.Ordinal))
            File.WriteAllText(Path.Combine(staging, name), "{}");
        else
        {
            Directory.CreateDirectory(Path.Combine(staging, name, "x"));
            File.WriteAllText(Path.Combine(staging, name, "x", "backup-manifest.json"), "evil");
        }
        var p = new Procs();
        Assert.Equal(SelfUpdate.Outcome.Aborted, Swap(p));
        Assert.Empty(p.Started);
        Assert.Equal("exe old", Read(app, "DDM32Plus.exe"));
        AssertUserDataUntouched();
    }

    [Fact]
    public void HelperMainWritesResultShownOnce()
    {
        var p = new Procs();
        var result = Path.Combine(app, "guncelleme", "last-update.json");
        var code = SelfUpdate.HelperMain(["--pid", "123", "--app-dir", app, "--staging", staging, "--marker", marker,
            "--result", result], pid => { Assert.Equal(123, pid); return p; });
        Assert.Equal(0, code);
        Assert.Equal("ok", JsonNode.Parse(File.ReadAllText(result))!["result"]!.GetValue<string>());
        Assert.Equal("ok", SelfUpdate.ReadResult(Path.Combine(app, "guncelleme")));
        Assert.Null(SelfUpdate.ReadResult(Path.Combine(app, "guncelleme")));
        Assert.Equal(2, SelfUpdate.HelperMain(["--pid", "x"]));
    }

    [Fact]
    public void HelperCommandStartsTheStagedProgram()
    {
        var (exe, args) = SelfUpdate.HelperCommand(staging, 42, app, marker, "r.json");
        Assert.Equal(Path.Combine(staging, "DDM32Plus.exe"), exe);
        Assert.Equal([SelfUpdate.HelperArg, "--pid", "42", "--app-dir", app, "--staging", staging, "--marker", marker,
            "--result", "r.json"], args);
    }

    // --- staging the zip ------------------------------------------------------------------------------------------
    private string ZipOf(params string[] names)
    {
        var z = tmp.Sub($"app-{Guid.NewGuid():N}.zip");
        File.WriteAllBytes(z, TestData.Zip([.. names.Select(n => (n, "x"u8.ToArray()))]));
        return z;
    }

    [Fact]
    public void GoodZipStages()
    {
        var staged = SelfUpdate.Stage(ZipOf("DDM32Plus/DDM32Plus.exe", "DDM32Plus/README.txt"), tmp.Sub("updates"), "2.0.0");
        Assert.True(File.Exists(Path.Combine(staged, "DDM32Plus.exe")));
        Assert.Equal("app-2.0.0", Path.GetFileName(Path.GetDirectoryName(staged)));
    }

    [Theory]
    [InlineData("DDM32Plus/DDM32Plus.exe", "../evil.exe")]
    [InlineData("DDM32Plus/DDM32Plus.exe", "/abs.txt")]
    [InlineData("Other/DDM32Plus.exe")]
    [InlineData("DDM32Plus/../../x")]
    [InlineData("DDM32Plus/readme.txt")]
    [InlineData("DDM32Plus/C:evil")]
    [InlineData("DDM32Plus/DDM32Plus.exe", "DDM32Plus\\..\\evil.exe")]
    [InlineData("DDM32Plus/DDM32Plus.exe", "DDM32Plus/yedekler/x/backup-manifest.json")]
    [InlineData("DDM32Plus/DDM32Plus.exe", "DDM32Plus/settings.json")]
    public void UnsafeZipsAreRefused(params string[] names)
    {
        var updates = tmp.Sub("updates");
        Assert.Throws<UpdateException>(() => SelfUpdate.Stage(ZipOf(names), updates, "2.0.0"));
        Assert.False(Directory.Exists(Path.Combine(updates, "app-2.0.0")));
        Assert.False(File.Exists(tmp.Sub("evil.exe")));
    }

    [Fact]
    public void CleanupKeepsRunningCopyAndRollback()
    {
        var u = tmp.Sub("u");
        Directory.CreateDirectory(Path.Combine(u, "app-1.0.0", "DDM32Plus"));
        Directory.CreateDirectory(Path.Combine(u, "app-2.0.0", "DDM32Plus"));
        Directory.CreateDirectory(Path.Combine(u, "rollback"));
        File.WriteAllText(Path.Combine(u, "x.zip"), "z");
        SelfUpdate.Cleanup(u, keep: Path.Combine(u, "app-2.0.0", "DDM32Plus"));
        Assert.False(Directory.Exists(Path.Combine(u, "app-1.0.0")));
        Assert.True(Directory.Exists(Path.Combine(u, "app-2.0.0")));
        Assert.True(Directory.Exists(Path.Combine(u, "rollback")));
        Assert.False(File.Exists(Path.Combine(u, "x.zip")));
    }
}
