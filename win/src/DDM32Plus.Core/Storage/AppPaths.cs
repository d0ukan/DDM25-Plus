// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;

namespace DDM32Plus.Core.Storage;

/// <summary>A data folder of the Python program found on this PC (read only: never moved or deleted).</summary>
public sealed record PythonDataFolder(string Folder, string Backups, int BackupFolders, int VerifiedBackups);

/// <summary>
/// Where the program keeps things: everything in its own install folder (the owner's decision, 2026-11-20).
/// The setup installs per user to %LOCALAPPDATA%\Programs\DDM32 Plus, so the folder is the user's own and writable.
///   settings.json   settings (language, disclaimer acceptance, update check)
///   backups\        the radio's backups and every install / restore log (private: calibration, channels).
///                   Program updates and the uninstaller never touch it.
///   releases\       downloaded firmware releases and the verified manifest cache
///   updates\        downloaded program updates, the self-update's staging and rollback copies
///   packs\          data pack files (user.csv and the packs built from it)
/// Up to 2.0.0-beta.2 these were yedekler, surumler, guncelleme and paketler: Ensure moves them.
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

    public string Packs => Path.Combine(Root, AppInfo.PacksFolder);

    /// <summary>The old backups folder (yedekler) when it is still there: a move that could not be done, or both
    /// names present. Its backups are still offered (quick update, restore, channels).</summary>
    public string? LegacyBackups =>
        Directory.Exists(Path.Combine(Root, AppInfo.LegacyBackupsFolder)) ? Path.Combine(Root, AppInfo.LegacyBackupsFolder) : null;

    /// <summary>This program's backup roots: backups, then the old yedekler if it is still there.</summary>
    public IReadOnlyList<string> OwnBackupRoots => LegacyBackups is { } l ? [Backups, l] : [Backups];

    /// <summary>Moves the old Turkish folders to the English names (a rename on the same disk; nothing is copied or
    /// deleted), then creates the folders. A folder whose new name already exists, or that cannot be moved (a file
    /// open in it), stays where it is; the next start tries again.</summary>
    public AppPaths Ensure()
    {
        MigrateLegacyFolders();
        foreach (var d in new[] { Backups, Releases, Updates })
            Directory.CreateDirectory(d);
        return this;
    }

    /// <summary>The (old, new) folders moved this time.</summary>
    public List<string> MigrateLegacyFolders()
    {
        var moved = new List<string>();
        foreach (var (oldName, newName) in AppInfo.LegacyFolders)
        {
            var from = Path.Combine(Root, oldName);
            var to = Path.Combine(Root, newName);
            if (!Directory.Exists(from) || Path.Exists(to))
                continue;
            try
            {
                Directory.Move(from, to);
                moved.Add(oldName);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
            }
        }
        return moved;
    }

    /// <summary>A path saved by an older version under ROOT\yedekler (and the like) mapped to the new folder when it
    /// was moved; any other path is returned as it is.</summary>
    public string MapLegacy(string path)
    {
        if (string.IsNullOrEmpty(path) || Path.Exists(path))
            return path;
        foreach (var (oldName, newName) in AppInfo.LegacyFolders)
        {
            var oldRoot = Path.Combine(Root, oldName) + Path.DirectorySeparatorChar;
            if (path.StartsWith(oldRoot, StringComparison.OrdinalIgnoreCase))
            {
                var mapped = Path.Combine(Root, newName, path[oldRoot.Length..]);
                return Path.Exists(mapped) ? mapped : path;
            }
        }
        return path;
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
                var backups = Path.Combine(folder, AppInfo.PythonBackupsFolder);
                var (all, verified) = CountBackups(backups);
                found.Add(new PythonDataFolder(folder, backups, all, verified));
            }
        }
        return found;
    }

    /// <summary>parent/name, or name-b, name-c ... the first that does not exist (the tools refuse an existing --out).</summary>
    public static string UniqueFolder(string parent, string name)
    {
        var first = Path.Combine(parent, name);
        if (!Path.Exists(first))
            return first;
        foreach (var c in "bcdefghijklmnopqrstuvwxyz")
        {
            var p = Path.Combine(parent, $"{name}-{c}");
            if (!Path.Exists(p))
                return p;
        }
        return Path.Combine(parent, $"{name}-{DateTimeOffset.UtcNow.ToUnixTimeSeconds()}");
    }

    public static string Today() => DateTime.Now.ToString("yyyyMMdd", System.Globalization.CultureInfo.InvariantCulture);

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

    /// <summary>The verified backup folders under the given roots that hold the file <paramref name="needs"/>, newest
    /// first (by the manifest's time); files are only listed and read.</summary>
    public static List<string> VerifiedBackups(IEnumerable<string> roots, string needs = "progmem-pass1.bin")
    {
        var found = new List<(string Dir, DateTime When)>();
        foreach (var root in roots)
        {
            if (!Directory.Exists(root))
                continue;
            try
            {
                foreach (var d in Directory.EnumerateDirectories(root))
                {
                    var m = Path.Combine(d, "backup-manifest.json");
                    if (File.Exists(m) && File.Exists(Path.Combine(d, needs)) && IsVerifiedBackup(d))
                        found.Add((d, File.GetLastWriteTimeUtc(m)));
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
            }
        }
        return [.. found.OrderByDescending(f => f.When).ThenByDescending(f => f.Dir, StringComparer.Ordinal).Select(f => f.Dir)];
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
