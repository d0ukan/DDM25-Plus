// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using DDM32Plus.Core;
using DDM32Plus.Core.Releases;
using DDM32Plus.Core.Storage;
using DDM32Plus.Core.Update;
using DDM32Plus.Localization;

namespace DDM32Plus.Services;

/// <summary>
/// What the pages share: the install-folder paths, settings, the verified release list and the program update.
/// Network access only from <see cref="CheckAsync"/> (a click, or the start-up check the user can switch off) and
/// the downloads the user starts. Nothing here talks to the radio.
/// </summary>
public sealed class AppState
{
    public AppPaths Paths { get; }
    public Settings Settings { get; }
    public IReleaseNet Net { get; }
    public Catalog Catalog { get; private set; }
    public IReadOnlyList<PythonDataFolder> PythonData { get; }
    public string? LastCheckError { get; private set; }
    public bool Checking { get; private set; }

    public event Action? Changed;

    public AppState(AppPaths paths, IReleaseNet net)
    {
        Paths = paths.Ensure();
        Settings = new Settings(paths.SettingsFile);
        Net = net;
        Catalog = SafeCached();
        PythonData = paths.FindPythonData();
    }

    public static AppState Create()
    {
        var exeDir = Path.GetDirectoryName(Environment.ProcessPath) ?? AppContext.BaseDirectory;
        return new AppState(AppPaths.Detect(exeDir), new HttpReleaseNet());
    }

    private Catalog SafeCached()
    {
        try
        {
            return ReleaseClient.CachedCatalog(Paths.Releases);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return new Catalog { Source = "cache" };
        }
    }

    /// <summary>The disclaimer text's version: a changed text (either language) asks again.</summary>
    public static string DisclaimerVersion()
    {
        var keys = new[] { "Disc_Title", "Disc_Intro", "Disc_B1", "Disc_B2", "Disc_B3", "Disc_B4", "Disc_B5", "Disc_Outro" };
        var rm = new System.Resources.ResourceManager("DDM32Plus.Resources.Strings", typeof(AppState).Assembly);
        var text = new StringBuilder();
        foreach (var culture in new[] { "tr-TR", "en-GB" })
            foreach (var k in keys)
                text.Append(rm.GetString(k, new System.Globalization.CultureInfo(culture)));
        return Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text.ToString())))[..16];
    }

    public bool DisclaimerAccepted => Settings.DisclaimerAccepted(DisclaimerVersion());

    /// <summary>
    /// Self-update works only in the published single-file program (not a development build, which has its
    /// DDM32Plus.dll beside the .exe).
    /// </summary>
    public static bool CanSelfUpdate =>
        !File.Exists(Path.Combine(AppContext.BaseDirectory, AppInfo.ExeName + ".dll"));

    public async Task CheckAsync()
    {
        if (Checking)
            return;
        Checking = true;
        Changed?.Invoke();
        try
        {
            Catalog = await ReleaseClient.LoadCatalogAsync(Paths.Releases, Net).ConfigureAwait(true);
            Settings.Save("last_check_utc", Catalog.CheckedUtc);
            LastCheckError = null;
        }
        catch (Exception e) when (e is NetException or IOException or UnauthorizedAccessException)
        {
            LastCheckError = e.Message;
        }
        finally
        {
            Checking = false;
            Changed?.Invoke();
        }
    }

    public void NotifyChanged() => Changed?.Invoke();

    public List<Package> Packages()
    {
        try
        {
            return ReleaseClient.ListPackages(Paths.Releases);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return [];
        }
    }

    /// <summary>The firmware the radio runs as far as this program knows (settings installed_sha256), or null.</summary>
    public FirmwareVersion? Installed()
    {
        var sha = Settings.GetString("installed_sha256");
        return sha is null ? null : Catalog.ByApplication(sha);
    }

    public FirmwareVersion? Newest() => Catalog.Newest(Settings.IncludeBeta);

    public AppRelease? AppUpdate() => Catalog.NewerApp(AppInfo.Version, Settings.IncludeBeta);

    public static string LabelText(ReleaseLabel label) => Loc.T("Label_" + label);

    /// <summary>
    /// The program update: download (hash-checked against the signed manifest), stage, start the new program as the
    /// helper. The caller closes the window when this returns true.
    /// </summary>
    public async Task<bool> StartProgramUpdateAsync(AppRelease update, Action<long, long>? progress)
    {
        var zip = await ReleaseClient.DownloadAppAsync(update.Entry, Paths.Updates, Net, progress).ConfigureAwait(true);
        var staged = SelfUpdate.Stage(zip, Paths.Updates, update.Entry.Version);
        var appDir = Path.GetDirectoryName(Environment.ProcessPath) ?? Paths.Root;
        var (exe, args) = SelfUpdate.HelperCommand(staged, Environment.ProcessId, appDir,
            Path.Combine(Paths.Updates, "update-started.json"), Path.Combine(Paths.Updates, SelfUpdate.ResultFile));
        var psi = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = staged };
        foreach (var a in args)
            psi.ArgumentList.Add(a);
        return Process.Start(psi) is not null;
    }

    public static string ProgramVersion =>
        typeof(AppState).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
        ?? AppInfo.Version;
}
