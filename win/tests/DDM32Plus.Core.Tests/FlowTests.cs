// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using DDM32Plus.Core.Radio;

namespace DDM32Plus.Core.Tests;

/// <summary>The install and restore flows' rules (pc/tests/test_wizard.py), the pages' file-only steps against the
/// tools' own plans, and the CH340 cable check.</summary>
public sealed class FlowTests(RadioInputs inputs) : IClassFixture<RadioInputs>
{
    private sealed record Pkg(string Sha256 = "aaaa", bool Installable = true) : IInstallable;

    private static PyDict Detection(string layout, string? mode, bool supported, string reason = "") =>
        new() { ["layout"] = layout, ["install_mode"] = mode, ["supported"] = supported, ["reason"] = reason };

    private static readonly PyDict Classic = Detection("classic", "over_classic", true);
    private static readonly PyDict Stock = Detection("stock", "from_stock", true);
    private static readonly PyDict Unknown = Detection("unknown", null, false, "Unknown firmware: no codec slot");
    private static readonly PyDict PlanOk = new() { ["boot_area_unchanged"] = true, ["data_chip_written"] = false };
    private static readonly StepResult BackupOk = new(true, false, false, "");
    private static readonly StepResult InstallPass = new(true, true, false, "");
    private static readonly StepResult InstallTimeout = new(false, false, true, "DumpError: the radio did not start its serial boot ROM within 600 s");
    private static readonly StepResult InstallMidWrite = new(false, true, false, "OSError: cable");

    private static Wizard Ready(PyDict detection, IInstallable? pkg = null)
    {
        var w = new Wizard();
        w.SetPort("COM7");
        w.SetPackage(pkg ?? new Pkg());
        w.StartBackup("b1");
        w.BackupFinished(BackupOk);
        w.StartDetect();
        w.DetectFinished(detection);
        return w;
    }

    [Fact]
    public void OrderBackupDetectPlanInstall()
    {
        var w = new Wizard();
        Assert.False(w.CanBackup() || w.CanPlan() || w.CanInstall());
        w.SetPort("COM7");
        w.SetPackage(new Pkg());
        Assert.False(w.CanDetect());
        w.StartBackup("b1");
        Assert.False(w.CanBackup());
        w.BackupFinished(BackupOk);
        Assert.True(w.CanDetect());
        Assert.False(w.CanPlan());
        w.StartDetect();
        w.DetectFinished(Classic);
        Assert.Equal("install_plus", w.Mode);
        Assert.True(w.CanPlan());
        Assert.False(w.CanInstall());
        w.StartPlan();
        w.PlanFinished(PlanOk);
        Assert.True(w.CanInstall());
        w.StartInstall();
        Assert.False(w.CanInstall());
        w.InstallFinished(InstallPass);
        Assert.Equal(StepState.Ok, w.Install);
        Assert.Equal(StepState.Stale, w.Backup);
        Assert.False(w.CanPlan());
    }

    [Fact]
    public void StockModeAndPlanKey()
    {
        var pkg = new Pkg();
        var w = Ready(Stock, pkg);
        Assert.Equal("install_plus_from_stock", w.Mode);
        w.StartPlan();
        Assert.Equal(("b1", "aaaa", "install_plus_from_stock"), w.PlanKey);
        w.PlanFinished(PlanOk);
        Assert.True(w.CanInstall());
        w.SetPackage(new Pkg());                       // the same application again (a page refresh): the plan stays
        Assert.True(w.CanInstall());
        w.SetPackage(new Pkg("bbbb"));                 // another version: the plan is gone
        Assert.False(w.CanInstall());
    }

    [Fact]
    public void UnsupportedRadioBlocksPlan()
    {
        var w = Ready(Unknown);
        Assert.Null(w.Mode);
        Assert.False(w.CanPlan());
        Assert.Equal("Wiz_Unsupported", w.Message.Key);
        var w2 = new Wizard();
        w2.SetPort("COM7");
        w2.SetPackage(new Pkg());
        w2.StartBackup("b");
        w2.BackupFinished(BackupOk);
        w2.StartDetect();
        w2.DetectFinished(null, "boom");
        Assert.Equal(StepState.Failed, w2.DetectState);
        Assert.False(w2.CanPlan());
    }

    [Fact]
    public void StepsRefuseOutOfOrderAndNothingStartsByItself()
    {
        var w = new Wizard();
        Assert.Throws<InvalidOperationException>(w.StartDetect);
        Assert.Throws<InvalidOperationException>(w.StartPlan);
        Assert.Throws<InvalidOperationException>(w.StartInstall);
        Assert.Throws<InvalidOperationException>(() => w.StartBackup("b"));     // no port
        w = Ready(Classic);
        Assert.Null(w.Running);
        Assert.Equal(StepState.Idle, w.Plan);
        w.StartPlan();
        w.PlanFinished(PlanOk);
        Assert.Equal(StepState.Idle, w.Install);
    }

    [Fact]
    public void UnsafePlanAndUnsignedPackage()
    {
        var w = Ready(Classic);
        w.StartPlan();
        w.PlanFinished(PlanOk, safe: false);
        Assert.Equal(StepState.Failed, w.Plan);
        Assert.False(w.CanInstall());
        Assert.False(Ready(Classic, new Pkg(Installable: false)).CanPlan());
    }

    [Fact]
    public void OnePlanOneInstallAndNoAutomaticRetry()
    {
        var w = Ready(Classic);
        w.StartPlan();
        w.PlanFinished(PlanOk);
        w.StartInstall();
        w.InstallFinished(InstallTimeout);
        Assert.Equal("Wiz_InstallNotStartedTimeout", w.Message.Key);
        Assert.False(w.CanInstall());                  // a timeout needs a new plan, by hand
        Assert.True(w.CanPlan());
    }

    [Fact]
    public void FailedInstallAfterWritesIsLockedUntilANewBackup()
    {
        var w = Ready(Classic);
        w.StartPlan();
        w.PlanFinished(PlanOk);
        w.StartInstall();
        w.InstallFinished(InstallMidWrite);
        Assert.True(w.Locked);
        Assert.False(w.CanPlan() || w.CanInstall() || w.CanDetect());
        w.StartBackup("b2");
        w.BackupFinished(BackupOk);
        Assert.False(w.Locked);
    }

    [Fact]
    public void PortChangeRefusedWhileARadioStepRuns()
    {
        var w = new Wizard();
        w.SetPort("COM7");
        w.StartBackup("b");
        Assert.Throws<InvalidOperationException>(() => w.SetPort("COM8"));
    }

    // --- the faster paths (owner's decision 2026-11-20) -----------------------------------------------------------
    private static readonly PyDict Ddm32 = new() { ["layout"] = "classic", ["firmware"] = "ddm32", ["install_mode"] = "over_classic", ["supported"] = true, ["reason"] = "" };
    private static readonly PyDict Open25 = new() { ["layout"] = "classic", ["firmware"] = "classic", ["install_mode"] = "over_classic", ["supported"] = true, ["reason"] = "" };

    private static Wizard QuickReady(PyDict detection)
    {
        var w = new Wizard();
        w.SetPort("COM7");
        w.SetPackage(new Pkg());
        w.UseQuickBackup("old-backup");
        w.StartDetect();
        w.DetectFinished(detection);
        return w;
    }

    [Fact]
    public void QuickUpdateOnlyOverDdm32()
    {
        var w = QuickReady(Ddm32);
        Assert.Equal(BackupPath.Quick, w.Path);
        Assert.Equal("install_plus_quick", w.Mode);
        w.StartPlan();
        w.PlanFinished(PlanOk);
        Assert.True(w.CanInstall());
        var (tool, args) = w.InstallCommand("plus", "out");
        Assert.Equal("fast_install", tool);
        Assert.Equal(["--port", "COM7", "--quick", "--backup", "old-backup", "--plus", "plus", "--out", "out", "--wait", "600"], args);
        foreach (var d in new[] { Stock, Open25, Unknown })
        {
            var q = QuickReady(d);
            Assert.Null(q.Mode);
            Assert.False(q.CanPlan());
        }
        Assert.Equal("Wiz_QuickNeedsDdm32", QuickReady(Stock).Message.Key);
        // a new backup or a chosen backup is the normal path again
        w = QuickReady(Ddm32);
        w.UseExistingBackup("b");
        Assert.Equal(BackupPath.Normal, w.Path);
    }

    [Fact]
    public void InstallWithoutBackupNeedsTheRiskAccepted()
    {
        var w = new Wizard();
        w.SetPort("COM7");
        w.SetPackage(new Pkg());
        Assert.Throws<InvalidOperationException>(() => w.ChooseNoBackup(false));
        Assert.Equal(BackupPath.Normal, w.Path);
        Assert.False(w.CanInstall());
        w.ChooseNoBackup(true);
        Assert.Equal((BackupPath.None, true, StepState.Skipped), (w.Path, w.RiskAccepted, w.Backup));
        Assert.False(w.CanDetect());                                      // nothing to detect from: the tool reads live
        Assert.Equal("install_plus_no_backup", w.Mode);
        Assert.True(w.CanInstall());
        Assert.Equal(["--port", "COM7", "--no-backup", "--plus", "p", "--out", "o", "--wait", "600"], w.InstallCommand("p", "o").Args);
        w.SetPort(null);
        Assert.False(w.CanInstall());
        w.SetPort("COM7");
        w.StartInstall();
        w.InstallFinished(InstallMidWrite);
        Assert.True(w.Locked);
        Assert.Equal("Wiz_NoBackupStoppedAfterWrites", w.Message.Key);
        Assert.False(w.CanInstall());
        // a refusal before any write leaves it to the user, by hand
        w = new Wizard();
        w.SetPort("COM7");
        w.SetPackage(new Pkg());
        w.ChooseNoBackup(true);
        w.StartInstall();
        w.InstallFinished(new StepResult(false, false, false, "ValueError: first install from stock firmware needs a new backup"));
        Assert.False(w.Locked);
        Assert.Null(w.Running);
        Assert.True(w.CanInstall());
        // a normal backup afterwards turns the risk off again
        w.StartBackup("b");
        Assert.Equal((BackupPath.Normal, false), (w.Path, w.RiskAccepted));
    }

    [Fact]
    public void TheBackupStepKeepsItsOwnResult()
    {
        var w = new Wizard();
        w.SetPort("COM7");
        w.SetPackage(new Pkg());
        w.StartBackup(@"E:\DDM32-dev-backups\dm32-20261120");
        Assert.Equal("Wiz_BackupRunning", w.BackupMessage.Key);
        w.BackupFinished(BackupOk);
        Assert.Equal("Wiz_BackupDone", w.BackupMessage.Key);
        Assert.Equal(@"E:\DDM32-dev-backups\dm32-20261120", w.BackupMessage.Args[0]);
        w.StartDetect();
        w.DetectFinished(Classic);
        w.StartPlan();
        w.PlanFinished(PlanOk);
        Assert.Equal("Wiz_PlanReady", w.Message.Key);                 // the check step's
        Assert.Equal("Wiz_BackupDone", w.BackupMessage.Key);          // the backup step still says what the backup did
        w.StartBackup("b2");
        w.BackupFinished(new StepResult(false, false, true, "DumpError: the radio did not start its serial boot ROM within 120 s"));
        Assert.Equal("Wiz_BackupTimeout", w.BackupMessage.Key);
    }

    [Fact]
    public void TheQuickUpdateFindsBackupsInChosenFolders()
    {
        using var tmp = new TempDir();
        var chosen = tmp.Sub("DDM32-dev-backups");
        var yedekler = tmp.Sub("yedekler");
        Directory.CreateDirectory(yedekler);
        Assert.Null(BackupFinder.Last([yedekler, chosen], Codec.Dm32));
        // a stock backup in yedekler is not offered; the DDM32 Plus backup in the chosen folder is
        RadioFixtures.WriteBackup(Path.Combine(yedekler, "dm32-20261113"), inputs.Stock, inputs.Data);
        var taken = Path.Combine(chosen, "dm32-20261120");
        RadioFixtures.WriteBackup(taken, inputs.Program, inputs.Data);
        Directory.CreateDirectory(Path.Combine(chosen, "not-a-backup"));
        Assert.Equal(taken, BackupFinder.Last([yedekler, chosen, chosen, tmp.Sub("missing")], Codec.Dm32));
        Assert.Equal(2, BackupFinder.Candidates([yedekler, chosen]).Count);
        // the chosen folder is remembered in the settings
        var s = new Core.Storage.Settings(tmp.Sub("settings.json"));
        s.RememberBackupFolder(chosen);
        s.RememberBackupFolder(chosen + System.IO.Path.DirectorySeparatorChar);
        Assert.Equal([chosen], s.BackupFolders());
    }

    [Fact]
    public void NormalPathIsTheDefault()
    {
        var w = new Wizard();
        Assert.Equal(BackupPath.Normal, w.Path);
        Assert.False(w.RiskAccepted);
        var r = Ready(Classic);
        Assert.Equal("install_plus", r.InstallCommand("p", "o").Tool);
        Assert.Equal("--from-stock", Ready(Stock).InstallCommand("p", "o").Args[^1]);
    }

    private static RestoreFlow Flow(string kind, PyDict? detection)
    {
        var f = new RestoreFlow { Port = "COM7" };
        f.Choose(kind, "bk");
        f.StartDetect();
        f.DetectFinished(detection);
        return f;
    }

    [Fact]
    public void RestoreToolFollowsTheBackup()
    {
        Assert.Equal("install_firstboot", Flow("stock", Stock).Tool?.Tool);
        Assert.Equal(["--restore-stock", "--use-saved-backup"], Flow("previous", Stock).Tool!.Value.Args);
        Assert.Equal("restore", Flow("previous", Classic).Tool?.Mode);
        var f = Flow("stock", Classic);                 // stock restore only from a stock backup
        Assert.Null(f.Tool);
        Assert.Equal("Res_NotStock", f.Message.Key);
        Assert.Null(Flow("previous", Unknown).Tool);
        Assert.Null(Flow("previous", null).Tool);
    }

    [Fact]
    public void RestoreSteps()
    {
        var f = Flow("stock", Stock);
        Assert.False(f.CanRestore());
        f.StartPlan();
        f.PlanFinished(PlanOk);
        Assert.True(f.CanRestore());
        f.Port = null;
        Assert.False(f.CanRestore());
        f.Port = "COM7";
        f.StartRestore();
        Assert.False(f.CanRestore());
        f.RestoreFinished(InstallMidWrite);
        Assert.Equal(StepState.Failed, f.Restore);
        Assert.False(f.CanRestore());                  // a new plan first, by hand
        Assert.True(f.CanPlan());
        f.StartPlan();
        f.PlanFinished(PlanOk);
        f.StartRestore();
        f.RestoreFinished(InstallPass);
        Assert.Equal(StepState.Ok, f.Restore);
    }

    // --- the pages' file-only steps give the tools' own plans -----------------------------------------------------
    private static string PrintedPlan(string scenario)
    {
        var lines = RadioFixtures.Scenario(scenario).GetProperty("lines").EnumerateArray().Select(l => l.GetString()).ToList();
        var end = lines.IndexOf("}");
        return string.Join("\n", lines.Take(end + 1));
    }

    [Fact]
    public void FileStepsPrintTheToolsPlans()
    {
        Assert.Equal(PrintedPlan("install_plan_only"),
            PyJson.Dumps(FileSteps.Plan("install_plus", inputs.Backup, inputs.Plus, TestData.TestPublicHex, Codec.Dm32)));
        Assert.Equal(PrintedPlan("from_stock_plan_only"),
            PyJson.Dumps(FileSteps.Plan("install_plus_from_stock", inputs.StockBackup, inputs.Plus, TestData.TestPublicHex,
                RadioFixtures.SyntheticCodecTables())));
        Assert.Equal(PrintedPlan("restore_plan_only"), PyJson.Dumps(FileSteps.RestorePlan("restore", inputs.Backup)));
        Assert.Equal(PrintedPlan("restore_stock_plan_only"), PyJson.Dumps(FileSteps.RestorePlan("restore_stock", inputs.StockBackup)));
        var d = FileSteps.DetectBackup(inputs.Backup, Codec.Dm32);
        Assert.Equal("over_classic", d["install_mode"]);
        Assert.True(FileSteps.Safe(FileSteps.RestorePlan("restore", inputs.Backup)));
        // the release key: the test release does not pass the embedded one
        Assert.Throws<ValueError>(() => FileSteps.Plan("install_plus", inputs.Backup, inputs.Plus, null, Codec.Dm32));
    }

    [Fact]
    public void StepResultsFromWhatTheToolsLeft()
    {
        using var tmp = new TempDir();
        Assert.False(StepResult.Install(tmp.Path, new DumpError("x")).WritesStarted);
        File.WriteAllText(tmp.Sub("install-receipt.json"), "{\"writes_started\": true, \"restart_sent\": false}");
        var r = StepResult.Install(tmp.Path, new ValueError("Post-write verification failed; reset remains disabled"));
        Assert.True(r.WritesStarted);
        Assert.False(r.Passed);
        File.WriteAllText(tmp.Sub("install-receipt.json"), "{\"writes_started\": true, \"restart_sent\": true}");
        Assert.True(StepResult.Install(tmp.Path, null).Passed);
        Assert.True(StepResult.Backup(tmp.Path, new DumpError("the radio did not start its serial boot ROM within 120 s: x")).Timeout);
        File.WriteAllText(tmp.Sub("backup-manifest.json"), "{\"backup_verified\": true}");
        Assert.True(StepResult.Backup(tmp.Path, null).Passed);
    }

    // --- CH340 ---------------------------------------------------------------------------------------------------
    [Fact]
    public void CableCheckFindsAMissingDriverWithoutAPort()
    {
        var ok = new PnpDevice(@"USB\VID_1A86&PID_7523\5&2A1B&0&3", "USB-SERIAL CH340 (COM14)", 0);
        var noDriver = new PnpDevice(@"USB\VID_1A86&PID_7523\5&2A1B&0&4", "USB Serial", 28);
        var other = new PnpDevice(@"USB\VID_0403&PID_6001\A1", "USB Serial Port (COM3)", 0);
        var virtualPort = new PnpDevice(@"COM0COM\PORT\CNCA0", "com0com - serial port emulator (COM12)", 0);
        Assert.Equal(CableState.NoCable, CableCheck.Evaluate([other, virtualPort]).State);
        var s = CableCheck.Evaluate([other, noDriver]);
        Assert.Equal((CableState.NoDriver, 28), (s.State, s.ErrorCode));
        s = CableCheck.Evaluate([noDriver, ok, virtualPort]);
        Assert.Equal((CableState.Ok, "COM14"), (s.State, s.Port));
        Assert.Equal(CableState.Problem, CableCheck.Evaluate([ok with { ErrorCode = 10, Name = "USB-SERIAL CH340" }]).State);
        Assert.Equal(CableState.NoPort, CableCheck.Evaluate([ok with { Name = "USB-SERIAL CH340" }]).State);
        var two = CableCheck.Evaluate([ok with { Name = "USB-SERIAL CH340 (COM15)" }, ok]);
        Assert.Equal(["COM14", "COM15"], two.Ports);
    }
}
