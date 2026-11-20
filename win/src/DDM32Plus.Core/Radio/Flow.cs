// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;

namespace DDM32Plus.Core.Radio;

public enum StepState { Idle, Running, Ok, Failed, Used, Stale }

/// <summary>How a radio step ended, read from what the tool left (pc/ddm32_app/parsers.py).</summary>
public sealed record StepResult(bool Passed, bool WritesStarted, bool Timeout, string Error)
{
    /// <summary>The radio was not switched on in time: the tool gave up waiting for its boot ROM.</summary>
    public static bool IsTimeout(string error) => error.Contains("did not start its serial boot ROM within", StringComparison.Ordinal);

    /// <summary>A finished backup tool run: verified when its manifest says so.</summary>
    public static StepResult Backup(string outDir, ToolError? error)
    {
        var verified = false;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(outDir, "backup-manifest.json")));
            verified = doc.RootElement.TryGetProperty("backup_verified", out var v) && v.ValueKind == JsonValueKind.True;
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
        {
        }
        var msg = error?.Record ?? "";
        return new StepResult(verified && error is null, false, IsTimeout(msg), msg);
    }

    /// <summary>A finished install or restore run: passed with its receipt saying restart_sent; writes_started from the
    /// receipt (no receipt: nothing was written).</summary>
    public static StepResult Install(string outDir, ToolError? error)
    {
        bool started = false, restarted = false;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(outDir, "install-receipt.json")));
            started = doc.RootElement.TryGetProperty("writes_started", out var s) && s.ValueKind == JsonValueKind.True;
            restarted = doc.RootElement.TryGetProperty("restart_sent", out var r) && r.ValueKind == JsonValueKind.True;
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
        {
        }
        var msg = error?.Record ?? "";
        return new StepResult(error is null && restarted, started, IsTimeout(msg), msg);
    }
}

/// <summary>A downloaded release the wizard may install (releases.Package as the wizard sees it).</summary>
public interface IInstallable
{
    string Sha256 { get; }
    bool Installable { get; }
}

/// <summary>
/// The install flow's rules (pc/ddm32_app/wizard.py): backup, detect, plan, install. Radio steps (backup, install)
/// start only from their own button and each needs the previous step to have succeeded; nothing here starts a step by
/// itself. Detect and plan read files only. A failed install is never retried: when no write had been sent the user
/// may plan and start again by hand; once writes had started the install stays locked until a new verified backup.
/// Message is a resource key (and its arguments) the UI shows.
/// </summary>
public sealed class Wizard
{
    public string? Port { get; private set; }
    public IInstallable? Package { get; private set; }
    public StepState Backup { get; private set; }
    public string? BackupDir { get; private set; }
    public bool BackupExisting { get; private set; }
    public StepState DetectState { get; private set; }
    public PyDict? Detection { get; private set; }
    public StepState Plan { get; private set; }
    public PyDict? PlanData { get; private set; }
    public (string Backup, string Sha, string Mode)? PlanKey { get; private set; }
    public StepState Install { get; private set; }
    public StepResult? InstallResult { get; private set; }
    public bool Locked { get; private set; }
    public string? Running { get; private set; }
    public (string Key, object?[] Args) Message { get; private set; } = ("", []);

    private static readonly Dictionary<string, string> Modes = new() { ["over_classic"] = "install_plus", ["from_stock"] = "install_plus_from_stock" };

    public void SetPort(string? port)
    {
        if (Running is "backup" or "install")
            throw new InvalidOperationException("port change while a radio step runs");
        Port = string.IsNullOrEmpty(port) ? null : port;
    }

    public void SetPackage(IInstallable? package)
    {
        if (Running is not null)
            throw new InvalidOperationException("package change while a step runs");
        if (!ReferenceEquals(package, Package))
        {
            Package = package;
            ResetPlan();
        }
    }

    private void ResetPlan()
    {
        (Plan, PlanData, PlanKey) = (StepState.Idle, null, null);
        if (Install != StepState.Running)
            Install = StepState.Idle;
    }

    /// <summary>install_plus / install_plus_from_stock for the detected radio, or null.</summary>
    public string? Mode
    {
        get
        {
            var d = Detection;
            if (DetectState != StepState.Ok || d is null || d["supported"] is not true)
                return null;
            return d["install_mode"] is string m && Modes.TryGetValue(m, out var mode) ? mode : null;
        }
    }

    public bool CanBackup() => Running is null && Port is not null;

    public bool CanDetect() => Running is null && Backup == StepState.Ok && !Locked;

    public bool CanPlan() => Running is null && Backup == StepState.Ok && DetectState == StepState.Ok && Mode is not null &&
                             !Locked && Install != StepState.Ok && Package is { Installable: true };

    public bool CanInstall() => Running is null && Plan == StepState.Ok && Port is not null && !Locked &&
                                Install == StepState.Idle && Package is { Installable: true } &&
                                PlanKey == (BackupDir!, Package.Sha256, Mode!);

    public void StartBackup(string backupDir)
    {
        if (!CanBackup())
            throw new InvalidOperationException("backup not allowed now");
        Running = "backup";
        (Backup, BackupDir, BackupExisting) = (StepState.Running, backupDir, false);
        (DetectState, Detection) = (StepState.Idle, null);
        ResetPlan();
        (Install, InstallResult) = (StepState.Idle, null);
        Message = ("Wiz_BackupRunning", []);
    }

    public void BackupFinished(StepResult result)
    {
        Running = null;
        if (result.Passed)
        {
            Backup = StepState.Ok;
            Locked = false;
            Message = ("Wiz_BackupVerified", []);
        }
        else
        {
            Backup = StepState.Failed;
            Message = result.Timeout ? ("Wiz_BackupTimeout", []) : ("Wiz_BackupFailed", [result.Error]);
        }
    }

    public void UseExistingBackup(string backupDir)
    {
        if (Running is not null)
            throw new InvalidOperationException("busy");
        (Backup, BackupDir, BackupExisting) = (StepState.Ok, backupDir, true);
        (DetectState, Detection) = (StepState.Idle, null);
        Locked = false;
        ResetPlan();
        (Install, InstallResult) = (StepState.Idle, null);
        Message = ("Wiz_ExistingBackup", []);
    }

    public void StartDetect()
    {
        if (!CanDetect())
            throw new InvalidOperationException("detect not allowed now");
        Running = "detect";
        (DetectState, Detection) = (StepState.Running, null);
        ResetPlan();
    }

    public void DetectFinished(PyDict? detection, string error = "")
    {
        Running = null;
        if (detection is not null)
        {
            (DetectState, Detection) = (StepState.Ok, detection);
            Message = detection["supported"] is true ? ("", []) : ("Wiz_Unsupported", [detection["reason"] as string ?? ""]);
        }
        else
        {
            (DetectState, Detection) = (StepState.Failed, null);
            Message = ("Wiz_DetectFailed", [error.Length > 0 ? error : "?"]);
        }
    }

    public void StartPlan()
    {
        if (!CanPlan())
            throw new InvalidOperationException("plan not allowed now");
        Running = "plan";
        (Plan, PlanData) = (StepState.Running, null);
        PlanKey = (BackupDir!, Package!.Sha256, Mode!);
        Message = ("Wiz_Planning", []);
    }

    public void PlanFinished(PyDict? plan, string error = "", bool safe = true)
    {
        Running = null;
        if (plan is not null && safe)
        {
            (Plan, PlanData) = (StepState.Ok, plan);
            Install = StepState.Idle;
            Message = ("Wiz_PlanReady", []);
        }
        else
        {
            (Plan, PlanData, PlanKey) = (StepState.Failed, plan, null);
            Message = ("Wiz_PlanFailed", [error.Length > 0 ? error : "unsafe plan"]);
        }
    }

    public void StartInstall()
    {
        if (!CanInstall())
            throw new InvalidOperationException("install not allowed now");
        Running = "install";
        Install = StepState.Running;
        Plan = StepState.Used;          // one plan, one install click: a new try needs a new plan
        Message = ("Wiz_Installing", []);
    }

    public void InstallFinished(StepResult result)
    {
        Running = null;
        InstallResult = result;
        if (result.Passed)
        {
            Install = StepState.Ok;
            Backup = StepState.Stale;   // the radio no longer matches the backup
            Message = ("Wiz_InstallPassed", []);
        }
        else if (result.WritesStarted)
        {
            Install = StepState.Failed;
            Locked = true;
            Message = ("Wiz_InstallStoppedAfterWrites", [result.Error]);
        }
        else
        {
            Install = StepState.Idle;
            Plan = StepState.Idle;
            Message = (result.Timeout ? "Wiz_InstallNotStartedTimeout" : "Wiz_InstallNotStarted", [result.Error]);
        }
    }
}

/// <summary>Geri dön (wizard.RestoreFlow): a backup of this radio, detect, plan, restore. The tool follows the
/// backup's layout: stock: install_firstboot --restore-stock; Classic layout: install_plus --restore.</summary>
public sealed class RestoreFlow
{
    public string? Port { get; set; }
    public string? Kind { get; private set; }
    public string? BackupDir { get; private set; }
    public StepState DetectState { get; private set; }
    public PyDict? Detection { get; private set; }
    public StepState Plan { get; private set; }
    public PyDict? PlanData { get; private set; }
    public StepState Restore { get; private set; }
    public StepResult? Result { get; private set; }
    public string? Running { get; private set; }
    public (string Key, object?[] Args) Message { get; private set; } = ("", []);

    public void Choose(string kind, string backupDir)
    {
        if (Running is not null)
            throw new InvalidOperationException("busy");
        (Kind, BackupDir) = (kind, backupDir);
        (DetectState, Detection, Plan, PlanData, Restore, Result) = (StepState.Idle, null, StepState.Idle, null, StepState.Idle, null);
        Message = ("", []);
    }

    /// <summary>(tool, extra args, plan mode) for the chosen backup, or null.</summary>
    public (string Tool, string[] Args, string Mode)? Tool
    {
        get
        {
            var d = Detection;
            if (DetectState != StepState.Ok || d is null)
                return null;
            if (d["layout"] as string == "stock")
                return ("install_firstboot", ["--restore-stock", "--use-saved-backup"], "restore_stock");
            if (d["layout"] as string == "classic" && Kind == "previous")
                return ("install_plus", ["--restore"], "restore");
            return null;
        }
    }

    public bool CanDetect() => Running is null && BackupDir is not null;

    public void StartDetect()
    {
        if (!CanDetect())
            throw new InvalidOperationException("detect not allowed now");
        (Running, DetectState) = ("detect", StepState.Running);
    }

    public void DetectFinished(PyDict? detection, string error = "")
    {
        Running = null;
        (DetectState, Detection) = detection is not null ? (StepState.Ok, detection) : (StepState.Failed, null);
        if (detection is null)
            Message = ("Res_DetectFailed", [error.Length > 0 ? error : "?"]);
        else if (Tool is null)
            Message = Kind == "stock" ? ("Res_NotStock", []) : ("Res_CannotRestore", [detection["reason"] as string ?? ""]);
    }

    public bool CanPlan() => Running is null && Tool is not null && Restore != StepState.Running;

    public void StartPlan()
    {
        if (!CanPlan())
            throw new InvalidOperationException("plan not allowed now");
        (Running, Plan, PlanData) = ("plan", StepState.Running, null);
    }

    public void PlanFinished(PyDict? plan, string error = "", bool safe = true)
    {
        Running = null;
        if (plan is not null && safe)
            (Plan, PlanData) = (StepState.Ok, plan);
        else
        {
            (Plan, PlanData) = (StepState.Failed, plan);
            Message = ("Wiz_PlanFailed", [error.Length > 0 ? error : "unsafe plan"]);
        }
    }

    public bool CanRestore() => Running is null && Plan == StepState.Ok && Port is not null && Restore != StepState.Running;

    public void StartRestore()
    {
        if (!CanRestore())
            throw new InvalidOperationException("restore not allowed now");
        (Running, Restore, Plan) = ("restore", StepState.Running, StepState.Used);
    }

    public void RestoreFinished(StepResult result)
    {
        Running = null;
        Result = result;
        Restore = result.Passed ? StepState.Ok : StepState.Failed;
        if (result.Passed)
            Message = ("Res_Passed", []);
        else if (result.WritesStarted)
        {
            Message = ("Res_StoppedAfterWrites", [result.Error]);
            Plan = StepState.Idle;
        }
        else
        {
            Message = (result.Timeout ? "Wiz_InstallNotStartedTimeout" : "Wiz_InstallNotStarted", [result.Error]);
            Plan = StepState.Idle;
        }
    }
}

/// <summary>The file-only steps the pages run (no port): detection and the write plans, the same as the tools'.</summary>
public static class FileSteps
{
    public static PyDict DetectBackup(string backup, Codec codec) =>
        Detect.Run(UnitImage.SavedImages(backup)["progmem-pass1.bin"], codec);

    /// <summary>The plan install_plus.py --plan-only prints for this backup and release.</summary>
    public static PyDict Plan(string mode, string backup, string plus, string? keyHex, Codec codec)
    {
        if (mode == "install_plus_from_stock")
        {
            var (o, t, _, prof) = InstallPlan.StockInputs(backup, plus, keyHex, codec);
            UnitImage.BackupJedec(backup);
            return InstallPlan.MakePlan(mode, o, t, prof.Tag, prof);
        }
        var (original, target, _, tag) = InstallPlan.InstallInputs(backup, plus, keyHex);
        UnitImage.BackupJedec(backup);
        return InstallPlan.MakePlan("install_plus", original, target, tag);
    }

    /// <summary>The plan of a restore (install_plus --restore or install_firstboot --restore-stock).</summary>
    public static PyDict RestorePlan(string mode, string backup)
    {
        if (mode == "restore")
        {
            var (b, t, _) = InstallPlan.RestoreInputs(backup);
            UnitImage.BackupJedec(backup);
            return InstallPlan.MakePlan("restore", b, t, null);
        }
        var (basis, target, _) = InstallPlan.RestoreInputs(backup, checkSlot: false);
        UnitImage.BackupJedec(backup);
        var todo = InstallPlan.Todo(basis, target);
        return new PyDict
        {
            ["mode"] = "restore_stock", ["changed_sectors"] = todo.Count,
            ["first"] = todo.Count > 0 ? PyJson.Hex(todo[0]) : null, ["last"] = todo.Count > 0 ? PyJson.Hex(todo[^1]) : null,
            ["target_application_sha256"] = Hash.Sha(target.AsSpan(InstallPlan.Start)),
            ["boot_area_unchanged"] = true, ["codec_slot_unchanged"] = false, ["codec_slot_rewritten_from_backup"] = true,
            ["data_chip_written"] = false,
        };
    }

    /// <summary>The plan is safe to offer: boot area kept, data chip not written (update_tab's plan check).</summary>
    public static bool Safe(PyDict plan) => plan["boot_area_unchanged"] is true && plan["data_chip_written"] is false;
}
