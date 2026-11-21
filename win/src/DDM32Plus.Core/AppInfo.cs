// SPDX-License-Identifier: GPL-3.0-or-later
namespace DDM32Plus.Core;

/// <summary>The program's name, version and the fixed folder names (one place for all of them).</summary>
public static class AppInfo
{
    public const string Name = "DDM32 Plus";
    public const string ExeName = "DDM32Plus";
    public const string ExeFile = ExeName + ".exe";

    /// <summary>The Windows (.NET) program's version; Directory.Build.props carries the same value.</summary>
    public const string Version = "2.0.0-beta.1";

    public const string Author = "d0ukan · TA2DOU";
    public const string RepoUrl = "https://github.com/d0ukan/DDM32-Plus";

    /// <summary>The Python program's data folder names under %LOCALAPPDATA% (read only, never moved or deleted).</summary>
    public const string PythonDataName = "DDM32Plus";
    public const string LegacyDataName = "DDM25Plus";

    // Folders and files inside the install folder
    public const string BackupsFolder = "yedekler";
    public const string ReleasesFolder = "surumler";
    public const string UpdatesFolder = "guncelleme";
    public const string PacksFolder = "paketler";
    public const string SettingsFile = "settings.json";

    /// <summary>Names an update must never replace: the user's data in the install folder.</summary>
    public static readonly IReadOnlyList<string> ReservedNames =
        [BackupsFolder, ReleasesFolder, UpdatesFolder, PacksFolder, SettingsFile, "unins000.exe", "unins000.dat"];
}
