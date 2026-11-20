// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;

namespace DDM32Plus.Core.Storage;

/// <summary>A data folder of the Python program found on this PC (read only: never moved or deleted).</summary>
public sealed record PythonDataFolder(string Folder, string Backups, int BackupFolders, int VerifiedBackups);

/// <summary>
/// Where the program keeps things: everything in its own install folder (the owner's decision, 2026-11-20).
/// The setup installs per user to %LOCALAPPDATA%\Programs\DDM32 Plus, so the folder is the user's own and writable.
///   settings.json   settings (language, disclaimer acceptance, update check)
///   yedekler\       the radio's backups and every install / restore log (private: calibration, channels).
///                   Program updates and the uninstaller never touch it.
///   surumler\       downloaded firmware releases and the verified manifest cache
///   guncelleme\     downloaded program updates, the self-update's staging and rollback copies
/// DDM32_DATA overrides the folder (tests, screenshots).
/// </summary>
public sealed class AppPaths
{
    public string Root { get; }
    public string Backups => Path.Combine(Root, AppInfo.BackupsFolder);
    public string Releases => Path.Combine(Root, AppInfo.ReleasesFolder);
    public string Updates => Path.Combine(Root, AppInfo.UpdatesFolder);
    public string SettingsFile => Path.Combine(Root, AppInfo.SettingsFile);

    public AppPaths(string root) => Root = Path.GetFullPath(root);

    /// <summary>The install folder (the .exe's folder) unless DDM32_DATA is set.</summary>
    public static AppPaths Detect(string exeDir, Func<string, string?>? env = null)
    {
        env ??= Environment.GetEnvironmentVariable;
        var over = env("DDM32_DATA");
        return new AppPaths(string.IsNullOrWhiteSpace(over) ? exeDir : over);
    }

    public AppPaths Ensure()
    {
        foreach (var d in new[] { Backups, Releases, Updates })
            Directory.CreateDirectory(d);
        return this;
    }

    /// <summary>
    /// The Python program's data folders (%LOCALAPPDATA%\DDM32Plus, and DDM25Plus from before the rename) with how
    /// many backups they hold. Read only; this program's own folder is never reported.
    /// </summary>
    public IReadOnlyList<PythonDataFolder> FindPythonData(Func<string, string?>? env = null)
    {
        env ??= Environment.GetEnvironmentVariable;
        var bases = new[] { env("LOCALAPPDATA"), env("APPDATA") }.Where(b => !string.IsNullOrWhiteSpace(b)).Distinct();
        var found = new List<PythonDataFolder>();
        foreach (var b in bases)
        {
            foreach (var name in new[] { AppInfo.PythonDataName, AppInfo.LegacyDataName })
            {
                var folder = Path.GetFullPath(Path.Combine(b!, name));
                if (!Directory.Exists(folder) || SamePath(folder, Root) || found.Any(f => SamePath(f.Folder, folder)))
                    continue;
                var backups = Path.Combine(folder, AppInfo.BackupsFolder);
                var (all, verified) = CountBackups(backups);
                found.Add(new PythonDataFolder(folder, backups, all, verified));
            }
        }
        return found;
    }

    /// <summary>(backup folders, verified ones): folders with a backup-manifest.json, verified when it says so.</summary>
    public static (int All, int Verified) CountBackups(string backups)
    {
        if (!Directory.Exists(backups))
            return (0, 0);
        int all = 0, verified = 0;
        foreach (var d in Directory.EnumerateDirectories(backups))
        {
            var m = Path.Combine(d, "backup-manifest.json");
            if (!File.Exists(m))
                continue;
            all++;
            if (IsVerifiedBackup(d))
                verified++;
        }
        return (all, verified);
    }

    /// <summary>backup-manifest.json says backup_verified: true (file read only).</summary>
    public static bool IsVerifiedBackup(string folder)
    {
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "backup-manifest.json")));
            return doc.RootElement.ValueKind == JsonValueKind.Object &&
                   doc.RootElement.TryGetProperty("backup_verified", out var v) && v.ValueKind == JsonValueKind.True;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            return false;
        }
    }

    private static bool SamePath(string a, string b) =>
        string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(a)),
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(b)), StringComparison.OrdinalIgnoreCase);
}
