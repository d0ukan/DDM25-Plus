// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.Globalization;
using System.IO.Compression;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace DDM32Plus.Core.Update;

/// <summary>A program zip that may not be used, or a failed staging step.</summary>
public sealed class UpdateException(string message) : Exception(message);

/// <summary>What the helper needs from the operating system; the tests pass fakes (nothing is started).</summary>
public interface IUpdateProcesses
{
    /// <summary>Starts exe with args; returns a handle for Stop.</summary>
    object? Start(string exe, IReadOnlyList<string> args);

    /// <summary>True once the old program has exited.</summary>
    bool WaitExit();

    /// <summary>True when the new program wrote the marker in time.</summary>
    bool WaitMarker(string marker);

    void Stop(object? handle);
}

/// <summary>
/// Self-update of the installed program (pc/ddm32_app/selfupdate.py, changed for the install-folder layout):
///
/// 1. The program downloads the new program zip named in a signed manifest ("app_net") and checks its size and
///    SHA-256 (ReleaseClient.DownloadAppAsync), then extracts it safely into guncelleme\app-VERSION\DDM32Plus\
///    (Stage). Nothing in the install folder changes yet.
/// 2. It starts the NEW program from that staging folder as the helper
///       DDM32Plus.exe --self-update --pid OLD --app-dir INSTALL --staging STAGING --marker FILE --result FILE
///    and closes itself.
/// 3. The helper (Swap) waits for the old program to exit and replaces ONLY the files the new program brings: each
///    one that exists is moved to guncelleme\rollback\ first. The user's data (yedekler, settings.json, surumler,
///    guncelleme) is never moved, copied over or deleted: a zip that would touch those names is refused. Then it
///    starts the new program with --update-check FILE; the new program writes the marker once its window is up.
/// 4. No marker in time: the helper stops the new program, moves its files to guncelleme\failed\, puts the old files
///    back and starts the old program. An error before the swap leaves everything as it was.
/// The result goes to guncelleme\last-update.json; the next start shows it once.
/// </summary>
public static class SelfUpdate
{
    public const string HelperArg = "--self-update";
    public const string CheckArg = "--update-check";
    public const string ResultFile = "last-update.json";
    public const string RollbackFolder = "rollback";
    public const string FailedFolder = "failed";
    public static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(90);
    public static readonly TimeSpan ExitTimeout = TimeSpan.FromSeconds(60);

    public enum Outcome { Ok, RolledBack, Aborted }

    public static string ResultName(Outcome o) => o switch
    {
        Outcome.Ok => "ok",
        Outcome.RolledBack => "rolled_back",
        _ => "aborted",
    };

    private static bool IsReserved(string topName) =>
        AppInfo.ReservedNames.Any(r => string.Equals(r, topName, StringComparison.OrdinalIgnoreCase));

    /// <summary>
    /// Extract a program zip whose members all sit under DDM32Plus/ (no absolute paths, no '..', no backslash, no
    /// drive or stream colon, none of the user's data names); returns the extracted DDM32Plus folder.
    /// </summary>
    public static string SafeExtract(string zipPath, string dest)
    {
        using (var z = ZipFile.OpenRead(zipPath))
        {
            foreach (var e in z.Entries)
            {
                var name = e.FullName;
                // PurePosixPath(name).parts: empty and '.' parts drop out
                var parts = name.Split('/').Where(p => p.Length > 0 && p != ".").ToArray();
                if (parts.Length == 0 || parts[0] != AppInfo.ExeName || name.StartsWith('/') || name.Contains('\\') ||
                    parts.Any(p => p == ".." || p.Contains(':')) ||
                    (parts.Length > 1 && IsReserved(parts[1])))
                    throw new UpdateException($"Unsafe or unexpected path in the program zip: '{name}'");
            }
            Directory.CreateDirectory(dest);
            var root = Path.GetFullPath(dest) + Path.DirectorySeparatorChar;
            foreach (var e in z.Entries)
            {
                var target = Path.GetFullPath(Path.Combine(dest, e.FullName));
                if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                    throw new UpdateException($"Unsafe or unexpected path in the program zip: '{e.FullName}'");
                if (e.FullName.EndsWith('/'))
                {
                    Directory.CreateDirectory(target);
                    continue;
                }
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                e.ExtractToFile(target, overwrite: true);
            }
        }
        var app = Path.Combine(dest, AppInfo.ExeName);
        if (!File.Exists(Path.Combine(app, AppInfo.ExeFile)))
            throw new UpdateException($"{AppInfo.ExeFile} missing in the program zip");
        return app;
    }

    /// <summary>The new program, extracted into updatesDir\app-VERSION\DDM32Plus.</summary>
    public static string Stage(string zipPath, string updatesDir, string version)
    {
        var target = Path.Combine(updatesDir, $"app-{version}");
        if (Directory.Exists(target))
            Directory.Delete(target, recursive: true);
        try
        {
            return SafeExtract(zipPath, target);
        }
        catch
        {
            TryDelete(target);
            throw;
        }
    }

    public static (string Exe, string[] Args) HelperCommand(string stagedApp, int pid, string appDir, string marker,
        string result) =>
        (Path.Combine(stagedApp, AppInfo.ExeFile),
         [HelperArg, "--pid", pid.ToString(CultureInfo.InvariantCulture), "--app-dir", appDir, "--staging", stagedApp,
          "--marker", marker, "--result", result]);

    public static void WriteMarker(string path, string version)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        File.WriteAllText(path, new JsonObject
        {
            ["started"] = true, ["version"] = version, ["pid"] = Environment.ProcessId,
        }.ToJsonString());
    }

    /// <summary>Relative paths of every file in the staged program.</summary>
    private static List<string> StagedFiles(string staging) =>
        [.. Directory.EnumerateFiles(staging, "*", SearchOption.AllDirectories)
            .Select(f => Path.GetRelativePath(staging, f)).Order(StringComparer.OrdinalIgnoreCase)];

    /// <summary>The helper's work: Ok, RolledBack or Aborted. Only the staged program's own files change.</summary>
    public static Outcome Swap(string appDir, string staging, IUpdateProcesses procs, string marker, Action<string>? log = null)
    {
        log ??= _ => { };
        var exe = Path.Combine(appDir, AppInfo.ExeFile);
        var updates = Path.Combine(appDir, AppInfo.UpdatesFolder);
        var rollback = Path.Combine(updates, RollbackFolder);
        var failed = Path.Combine(updates, FailedFolder);
        if (!File.Exists(Path.Combine(staging, AppInfo.ExeFile)))
        {
            log("aborted: the staged program has no .exe");
            return Outcome.Aborted;
        }
        var files = StagedFiles(staging);
        if (files.Any(f => IsReserved(f.Split(Path.DirectorySeparatorChar)[0])))
        {
            log("aborted: the staged program would replace user data; nothing changed");
            return Outcome.Aborted;
        }
        if (!procs.WaitExit())
        {
            log("aborted: the old program did not exit; nothing changed");
            return Outcome.Aborted;
        }
        if (File.Exists(marker))
            File.Delete(marker);

        // 1. Move the files the update replaces aside (rollback copy)
        var moved = new List<string>();
        try
        {
            TryDelete(rollback);
            foreach (var rel in files)
            {
                var cur = Path.Combine(appDir, rel);
                if (!File.Exists(cur))
                    continue;
                var keep = Path.Combine(rollback, rel);
                Directory.CreateDirectory(Path.GetDirectoryName(keep)!);
                File.Move(cur, keep);
                moved.Add(rel);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            log($"aborted: cannot move the program files aside ({e.Message}); nothing changed");
            MoveBack(appDir, rollback, moved);
            procs.Start(exe, []);
            return Outcome.Aborted;
        }

        // 2. Copy the new files in
        var copied = new List<string>();
        try
        {
            foreach (var rel in files)
            {
                var dst = Path.Combine(appDir, rel);
                Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
                File.Copy(Path.Combine(staging, rel), dst, overwrite: false);
                copied.Add(rel);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            log($"copy failed ({e.Message}); putting the old program back");
            foreach (var rel in copied)
                TryDeleteFile(Path.Combine(appDir, rel));
            MoveBack(appDir, rollback, moved);
            procs.Start(exe, []);
            return Outcome.RolledBack;
        }

        // 3. Start the new program and wait for its marker
        var handle = procs.Start(exe, [CheckArg, marker]);
        if (procs.WaitMarker(marker))
        {
            log("ok: the new program started");
            return Outcome.Ok;
        }
        log("the new program did not start in time; rolling back");
        procs.Stop(handle);
        TryDelete(failed);
        foreach (var rel in copied)
        {
            var cur = Path.Combine(appDir, rel);
            try
            {
                var bad = Path.Combine(failed, rel);
                Directory.CreateDirectory(Path.GetDirectoryName(bad)!);
                File.Move(cur, bad);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                TryDeleteFile(cur);
            }
        }
        MoveBack(appDir, rollback, moved);
        procs.Start(exe, []);
        return Outcome.RolledBack;
    }

    private static void MoveBack(string appDir, string rollback, List<string> moved)
    {
        foreach (var rel in moved)
        {
            var dst = Path.Combine(appDir, rel);
            TryDeleteFile(dst);
            File.Move(Path.Combine(rollback, rel), dst);
        }
    }

    /// <summary>The helper's entry: DDM32Plus.exe --self-update ARGS. Exit code 0 when the new program runs.</summary>
    public static int HelperMain(IReadOnlyList<string> argv, Func<int, IUpdateProcesses>? processes = null)
    {
        var a = ParseArgs(argv);
        if (a is null)
            return 2;
        var lines = new List<string>();
        void Log(string text) => lines.Add($"{DateTime.Now:HH:mm:ss} {text}");
        var procs = (processes ?? (pid => new RealProcesses(pid)))(a.Value.Pid);
        var result = Swap(a.Value.AppDir, a.Value.Staging, procs, a.Value.Marker, Log);
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(a.Value.Result))!);
            var arr = new JsonArray();
            foreach (var l in lines)
                arr.Add(l);
            File.WriteAllText(a.Value.Result, new JsonObject
            {
                ["result"] = ResultName(result), ["log"] = arr,
                ["finished"] = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture),
            }.ToJsonString());
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
        return result == Outcome.Ok ? 0 : 1;
    }

    private static (int Pid, string AppDir, string Staging, string Marker, string Result)? ParseArgs(IReadOnlyList<string> argv)
    {
        var map = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var i = 0; i + 1 < argv.Count; i += 2)
        {
            if (!argv[i].StartsWith("--", StringComparison.Ordinal))
                return null;
            map[argv[i]] = argv[i + 1];
        }
        if (argv.Count % 2 != 0)
            return null;
        string[] need = ["--pid", "--app-dir", "--staging", "--marker", "--result"];
        if (need.Any(n => !map.ContainsKey(n)) || !int.TryParse(map["--pid"], NumberStyles.None, CultureInfo.InvariantCulture, out var pid))
            return null;
        return (pid, map["--app-dir"], map["--staging"], map["--marker"], map["--result"]);
    }

    /// <summary>The last helper result, once (the file is removed after reading).</summary>
    public static string? ReadResult(string updatesDir)
    {
        var p = Path.Combine(updatesDir, ResultFile);
        try
        {
            var node = JsonNode.Parse(File.ReadAllText(p));
            File.Delete(p);
            return node?["result"]?.GetValue<string>();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException)
        {
            return null;
        }
    }

    /// <summary>Old staging folders and zips (not the one running now). Rollback copies are kept.</summary>
    public static void Cleanup(string updatesDir, string? keep = null)
    {
        if (!Directory.Exists(updatesDir))
            return;
        var keepFull = keep is null ? null : Path.GetFullPath(keep);
        foreach (var d in Directory.GetDirectories(updatesDir, "app-*"))
        {
            var full = Path.GetFullPath(d) + Path.DirectorySeparatorChar;
            if (keepFull is not null && (keepFull + Path.DirectorySeparatorChar).StartsWith(full, StringComparison.OrdinalIgnoreCase))
                continue;
            TryDelete(d);
        }
        foreach (var f in Directory.GetFiles(updatesDir))
            if (f.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".partial", StringComparison.OrdinalIgnoreCase))
                TryDeleteFile(f);
    }

    private static void TryDelete(string dir)
    {
        try
        {
            if (Directory.Exists(dir))
                Directory.Delete(dir, recursive: true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }

    private static void TryDeleteFile(string file)
    {
        try
        {
            if (File.Exists(file))
                File.Delete(file);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }

    /// <summary>The real process functions (Windows).</summary>
    private sealed class RealProcesses(int oldPid) : IUpdateProcesses
    {
        public object? Start(string exe, IReadOnlyList<string> args)
        {
            var psi = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! };
            foreach (var a in args)
                psi.ArgumentList.Add(a);
            return Process.Start(psi);
        }

        public bool WaitExit()
        {
            try
            {
                using var p = Process.GetProcessById(oldPid);
                return p.WaitForExit(ExitTimeout);
            }
            catch (ArgumentException)
            {
                return true; // already gone
            }
            catch (InvalidOperationException)
            {
                return true;
            }
        }

        public bool WaitMarker(string marker)
        {
            var end = DateTime.UtcNow + StartTimeout;
            while (DateTime.UtcNow < end)
            {
                if (File.Exists(marker))
                    return true;
                Thread.Sleep(500);
            }
            return false;
        }

        public void Stop(object? handle)
        {
            try
            {
                (handle as Process)?.Kill(entireProcessTree: true);
            }
            catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception)
            {
            }
            Thread.Sleep(1000);
        }
    }
}
