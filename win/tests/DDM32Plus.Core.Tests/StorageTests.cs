// SPDX-License-Identifier: GPL-3.0-or-later
using DDM32Plus.Core.Storage;

namespace DDM32Plus.Core.Tests;

/// <summary>Install-folder paths, the Python program's data folders (read only) and settings.json.</summary>
public sealed class StorageTests : IDisposable
{
    private readonly TempDir tmp = new();

    public void Dispose() => tmp.Dispose();

    [Fact]
    public void EverythingLivesInTheInstallFolder()
    {
        var p = AppPaths.Detect(tmp.Sub("Programs", "DDM32 Plus"), _ => null).Ensure();
        Assert.Equal(tmp.Sub("Programs", "DDM32 Plus", "backups"), p.Backups);
        Assert.Equal(tmp.Sub("Programs", "DDM32 Plus", "settings.json"), p.SettingsFile);
        Assert.True(Directory.Exists(p.Backups) && Directory.Exists(p.Releases) && Directory.Exists(p.Updates));
        var over = AppPaths.Detect(tmp.Path, n => n == "DDM32_DATA" ? tmp.Sub("data") : null);
        Assert.Equal(tmp.Sub("data"), over.Root);
    }

    [Fact]
    public void OldTurkishFoldersAreMovedToTheEnglishNames()
    {
        var root = tmp.Sub("Programs", "DDM32 Plus");
        MakeBackup(Path.Combine(root, "yedekler", "dm32-20261121"), true);
        Directory.CreateDirectory(Path.Combine(root, "surumler", "1.0.0-beta.4"));
        Directory.CreateDirectory(Path.Combine(root, "paketler"));
        File.WriteAllText(Path.Combine(root, "paketler", "user.csv"), "x");
        var p = AppPaths.Detect(root, _ => null).Ensure();
        Assert.False(Directory.Exists(Path.Combine(root, "yedekler")));
        Assert.False(Directory.Exists(Path.Combine(root, "surumler")));
        Assert.True(AppPaths.IsVerifiedBackup(Path.Combine(p.Backups, "dm32-20261121")));
        Assert.True(Directory.Exists(Path.Combine(p.Releases, "1.0.0-beta.4")));
        Assert.Equal("x", File.ReadAllText(Path.Combine(p.Packs, "user.csv")));
        Assert.Null(p.LegacyBackups);
        Assert.Equal([p.Backups], p.OwnBackupRoots);
        // a path saved by the old version still finds the moved backup
        var saved = Path.Combine(root, "yedekler", "dm32-20261121");
        Assert.Equal(Path.Combine(p.Backups, "dm32-20261121"), p.MapLegacy(saved));
        Assert.Equal(@"E:\elsewhere\dm32", p.MapLegacy(@"E:\elsewhere\dm32"));
    }

    [Fact]
    public void BothNamesPresentKeepsTheOldFolderAndStillOffersItsBackups()
    {
        var root = tmp.Sub("Programs", "DDM32 Plus");
        MakeBackup(Path.Combine(root, "yedekler", "dm32-20261120"), true);
        MakeBackup(Path.Combine(root, "backups", "dm32-20261121"), true);
        var p = AppPaths.Detect(root, _ => null).Ensure();
        Assert.True(Directory.Exists(Path.Combine(root, "yedekler", "dm32-20261120")));
        Assert.Equal([p.Backups, Path.Combine(root, "yedekler")], p.OwnBackupRoots);
        Assert.Equal(2, AppPaths.VerifiedBackups(p.OwnBackupRoots, "backup-manifest.json").Count);
    }

    private void MakeBackup(string folder, bool verified)
    {
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, "backup-manifest.json"), verified ? "{\"backup_verified\": true}" : "{}");
    }

    [Fact]
    public void PythonDataFoldersAreFoundAndLeftAlone()
    {
        var local = tmp.Sub("Local");
        MakeBackup(Path.Combine(local, "DDM25Plus", "yedekler", "dm32-20261113"), true);
        MakeBackup(Path.Combine(local, "DDM25Plus", "yedekler", "dm32-20261114"), false);
        MakeBackup(Path.Combine(local, "DDM32Plus", "yedekler", "dm32-20261120"), true);
        Directory.CreateDirectory(Path.Combine(local, "DDM32Plus", "yedekler", "not-a-backup"));
        var before = Directory.GetFileSystemEntries(local, "*", SearchOption.AllDirectories).Order().ToArray();

        var p = new AppPaths(Path.Combine(local, "Programs", "DDM32 Plus"));
        var found = p.FindPythonData(n => n == "LOCALAPPDATA" ? local : null);
        Assert.Equal(2, found.Count);
        var d32 = found.Single(f => f.Folder.EndsWith("DDM32Plus", StringComparison.Ordinal));
        Assert.Equal((1, 1), (d32.BackupFolders, d32.VerifiedBackups));
        var d25 = found.Single(f => f.Folder.EndsWith("DDM25Plus", StringComparison.Ordinal));
        Assert.Equal((2, 1), (d25.BackupFolders, d25.VerifiedBackups));
        Assert.Equal(Path.Combine(local, "DDM25Plus", "yedekler"), d25.Backups);
        Assert.Equal(before, Directory.GetFileSystemEntries(local, "*", SearchOption.AllDirectories).Order().ToArray());

        // this program's own folder is never reported as the Python program's
        var own = new AppPaths(Path.Combine(local, "DDM32Plus"));
        Assert.Single(own.FindPythonData(n => n == "LOCALAPPDATA" ? local : null));
        Assert.Empty(p.FindPythonData(_ => null));
    }

    [Fact]
    public void SettingsRoundTripAndDamage()
    {
        var s = new Settings(tmp.Sub("settings.json"));
        Assert.Null(s.Language);
        Assert.True(s.AutoCheck);
        s.Language = "en";
        s.AutoCheck = false;
        Assert.Equal("en", s.Language);
        Assert.False(s.AutoCheck);
        Assert.False(File.Exists(tmp.Sub("settings.tmp")));
        Assert.False(s.DisclaimerAccepted("v1"));
        s.AcceptDisclaimer("v1", "2026-11-20T00:00:00Z");
        Assert.True(s.DisclaimerAccepted("v1"));
        Assert.False(s.DisclaimerAccepted("v2"));          // a changed text asks again
        s.WithdrawDisclaimer("v1", "2026-11-20T00:00:01Z");
        Assert.False(s.DisclaimerAccepted("v1"));
        File.WriteAllText(s.FilePath, "{ damaged");
        Assert.Null(s.Language);
        s.Language = "tr";
        Assert.Equal("tr", s.Language);
        s.Save("language", "xx");
        Assert.Null(s.Language);
    }

    [Fact]
    public void UnknownKeysAreKept()
    {
        var path = tmp.Sub("settings.json");
        File.WriteAllText(path, "{\"window_geometry\": \"abc\", \"port\": \"COM14\"}");
        var s = new Settings(path) { Language = "tr" };
        Assert.Equal("COM14", s.GetString("port"));
        Assert.Equal("abc", s.GetString("window_geometry"));
    }

    [Fact]
    public void VersionIsInOnePlace()
    {
        var info = typeof(AppInfo).Assembly
            .GetCustomAttributes(typeof(System.Reflection.AssemblyInformationalVersionAttribute), false)
            .Cast<System.Reflection.AssemblyInformationalVersionAttribute>().Single().InformationalVersion;
        Assert.Equal(AppInfo.Version, info);
        Assert.NotNull(Releases.ReleaseVersion.TryParse(AppInfo.Version));
    }
}
