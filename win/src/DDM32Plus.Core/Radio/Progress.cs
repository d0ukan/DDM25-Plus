// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text.RegularExpressions;

namespace DDM32Plus.Core.Radio;

/// <summary>
/// The install and restore tools' own progress lines turned into what the page shows: a phase (a resource key that
/// takes the percent) and the bar's percent. Pre-write checks ("Read current firmware: 40%", "Check settings chip:
/// 50%") are one phase, comparing the radio with the backup (program 0-50 %, settings chip 50-100 %); the install
/// without backup reads the program once instead. Then the writes ("...checked: 25% (1/4)") and the readback
/// ("Read complete installed program chip", "Recheck untouched settings chip", "Verify ... application").
/// </summary>
public sealed partial class ProgressTracker
{
    [GeneratedRegex(@"^(.+?): (\d{1,3})%(?: \((\d+)/(\d+)\))?$")]
    private static partial Regex Line();

    private string? phase;

    /// <summary>(resource key, percent of the bar) for a progress line, else null.</summary>
    public (string Key, int Percent)? Feed(string line)
    {
        var m = Line().Match(line.Trim());
        if (!m.Success)
            return null;
        var label = m.Groups[1].Value;
        var p = Math.Clamp(int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture), 0, 100);
        (string Key, int Percent)? r = label switch
        {
            "Read current firmware" => ("Prog_Compare", p / 2),
            "Read current firmware (once, kept in memory)" => ("Prog_Read", p / 2),
            // the settings chip before the writes: the second half of the phase it follows
            "Check settings chip" => (phase == "Prog_Read" ? "Prog_Read" : "Prog_Compare", 50 + p / 2),
            "Written and sector-readback checked" or "Firmware written and sector-readback checked" => ("Prog_Write", p),
            "Read complete installed program chip" => ("Prog_Verify", p / 2),
            "Recheck untouched settings chip" => ("Prog_Verify", 50 + p / 2),
            "Verify newly installed application" or "Verify restored application" => ("Prog_Verify", p),
            _ => null,
        };
        if (r is { } x)
            phase = x.Key;
        return r;
    }
}
