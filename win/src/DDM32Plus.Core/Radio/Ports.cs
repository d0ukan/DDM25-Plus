// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;

namespace DDM32Plus.Core.Radio;

/// <summary>What the monitor protocol needs from a serial port (pyserial's subset the Python tools use).</summary>
public interface IRadioPort
{
    /// <summary>Read timeout in seconds (only set, as the Python tools do).</summary>
    double Timeout { get; set; }

    int BaudRate { get; set; }

    /// <summary>Sends the bytes; returns how many went out (or null when the port does not say).</summary>
    int? Write(byte[] data);

    /// <summary>What arrived within the timeout, at most n bytes; empty when nothing came.</summary>
    byte[] Read(int n);

    void ResetInputBuffer();

    void Flush();
}

/// <summary>The serial port itself: opened and closed by the tool (raw.open(), raw.close()).</summary>
public interface IRawSerial : IRadioPort
{
    void Open();

    void Close();
}

/// <summary>Opens the raw port for a tool; the only way a tool reaches a serial port (see RadioAccess).</summary>
public delegate IRawSerial SerialFactory(string portName);

/// <summary>The time the tools wait by: real for a radio, virtual for the byte-for-byte tests.</summary>
public interface IClock
{
    /// <summary>time.monotonic()</summary>
    double Monotonic();

    /// <summary>time.sleep()</summary>
    void Sleep(double seconds);

    /// <summary>time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())</summary>
    string UtcStamp();
}

public sealed class SystemClock : IClock
{
    public static readonly SystemClock Instance = new();
    private static readonly System.Diagnostics.Stopwatch Watch = System.Diagnostics.Stopwatch.StartNew();

    public double Monotonic() => Watch.Elapsed.TotalSeconds;

    public void Sleep(double seconds)
    {
        if (seconds > 0)
            Thread.Sleep(TimeSpan.FromSeconds(seconds));
    }

    public string UtcStamp() => DateTime.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);
}

/// <summary>
/// The virtual clock of the byte-for-byte tests (the same as make_radio_fixtures.py): every reading advances 1 ms,
/// a sleep advances by its length, and every UTC stamp is the same.
/// </summary>
public sealed class VirtualClock(string stamp = "2026-11-20T00:00:00Z") : IClock
{
    public double T { get; private set; } = 1000.0;

    public double Monotonic()
    {
        T += 0.001;
        return T;
    }

    public void Sleep(double seconds) => T += Math.Max(0.0, seconds);

    public string UtcStamp() => stamp;
}

/// <summary>
/// The switch in front of every serial port: a tool gets a port only through <see cref="Factory"/>, and that
/// refuses while radio access is off. It is off in every build unless the program is compiled with the developer
/// flag (dotnet build -p:RadioAccess=true, which defines DDM32_RADIO_ACCESS); nothing at run time can turn it on.
/// The first real run needs the owner's approval for that step (AGENTS.md, hard safety rules). Even with radio access
/// the tools that write (install, restore) get no port: <see cref="WriteFactory"/> stays off.
/// </summary>
public static class RadioAccess
{
#if DDM32_RADIO_ACCESS
    public const bool Enabled = true;
#else
    public const bool Enabled = false;
#endif

    public sealed class DisabledException() : InvalidOperationException(
        "Radio access is switched off in this build: no serial port is opened (RadioAccess.Enabled = false)");

#if DDM32_RADIO_WRITES
    public const bool WritesEnabled = Enabled;
#else
    public const bool WritesEnabled = false;
#endif

    /// <summary>The port factory for the tools that write (install, restore): off even in the developer build with
    /// radio access, which runs only the read-only backup. A later, separately approved flag turns writes on.</summary>
    public static SerialFactory WriteFactory =>
#pragma warning disable CS0162
        WritesEnabled ? name => new SerialRadioPort(name) : _ => throw new DisabledException();
#pragma warning restore CS0162

    /// <summary>The serial port factory for the tools: a real port only when radio access is compiled in.</summary>
    public static SerialFactory Factory =>
#pragma warning disable CS0162 // one branch is unreachable, by the build flag
        Enabled ? name => new SerialRadioPort(name) : _ => throw new DisabledException();
#pragma warning restore CS0162
}
