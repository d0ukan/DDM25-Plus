// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.RegularExpressions;

namespace DDM32Plus.Core.Radio;

/// <summary>A Plug and Play device as Windows lists it (Win32_PnPEntity): no port is opened to list it.</summary>
public sealed record PnpDevice(string DeviceId, string Name, int ErrorCode);

public enum CableState { Ok, NoCable, NoDriver, Problem, NoPort }

/// <summary>What the cable check found: the state, the COM port of a working cable, the Windows problem code.</summary>
public sealed record CableStatus(CableState State, string? Port, int ErrorCode, IReadOnlyList<string> Ports);

/// <summary>
/// The DM-32UV programming cable is a WCH CH340 (USB VID_1A86&amp;PID_7523; the CH341 in serial mode is PID_5523).
/// Windows lists it even without its driver (problem code 28), so a missing driver is found without opening a port.
/// </summary>
public static partial class CableCheck
{
    private static readonly string[] Ids = ["VID_1A86&PID_7523", "VID_1A86&PID_5523"];

    [GeneratedRegex(@"\((COM\d+)\)")]
    private static partial Regex ComName();

    public static bool IsCh340(PnpDevice d) =>
        d.DeviceId.StartsWith("USB\\", StringComparison.OrdinalIgnoreCase) &&
        Ids.Any(id => d.DeviceId.Contains(id, StringComparison.OrdinalIgnoreCase));

    public static string? PortOf(PnpDevice d) => ComName().Match(d.Name) is { Success: true } m ? m.Groups[1].Value : null;

    public static CableStatus Evaluate(IEnumerable<PnpDevice> devices)
    {
        var cables = devices.Where(IsCh340).ToList();
        if (cables.Count == 0)
            return new CableStatus(CableState.NoCable, null, 0, []);
        var ports = cables.Where(d => d.ErrorCode == 0).Select(PortOf).OfType<string>()
            .OrderBy(p => int.Parse(p[3..], System.Globalization.CultureInfo.InvariantCulture)).ToList();
        if (ports.Count > 0)
            return new CableStatus(CableState.Ok, ports[0], 0, ports);
        // 28: no driver installed; 1 (not configured) and 18 (reinstall) mean the same to the user
        var missing = cables.FirstOrDefault(d => d.ErrorCode is 28 or 1 or 18);
        if (missing is not null)
            return new CableStatus(CableState.NoDriver, null, missing.ErrorCode, []);
        var problem = cables.FirstOrDefault(d => d.ErrorCode != 0);
        if (problem is not null)
            return new CableStatus(CableState.Problem, null, problem.ErrorCode, []);
        return new CableStatus(CableState.NoPort, null, 0, []);
    }
}
