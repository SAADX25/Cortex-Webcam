using System.Collections;
using System.IO;
using System.Runtime.InteropServices;

namespace CortexStudio;
internal readonly record struct NetworkAccessState(bool Verified, bool Allowed, string Profile);
internal static class NetworkAccess
{
    private static bool Includes(string ports, int port)
    {
        if (ports is "*" or "Any" or "") return true;
        foreach (var item in ports.Split(','))
        {
            var range = item.Trim().Split('-');
            if (range.Length == 1 && int.TryParse(range[0], out var single) && single == port) return true;
            if (range.Length == 2 && int.TryParse(range[0], out var lower) && int.TryParse(range[1], out var upper) && port >= lower && port <= upper) return true;
        }
        return false;
    }
    internal static NetworkAccessState Read(int videoPort, int controlPort)
    {
        object? raw = null;
        try
        {
            var type = Type.GetTypeFromProgID("HNetCfg.FwPolicy2");
            if (type is null) return new(false, false, "Unavailable");
            raw = Activator.CreateInstance(type);
            dynamic policy = raw!;
            int active = policy.CurrentProfileTypes;
            var profile = (active & 4) != 0 ? "Public network" : (active & 2) != 0 ? "Private network" : "Domain network";
            var program = Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, "CortexStudio.exe");
            bool video = false, control = false, blocked = false;
            foreach (dynamic rule in (IEnumerable)policy.Rules)
            {
                try
                {
                    string path = rule.ApplicationName ?? "";
                    if (!string.Equals(Environment.ExpandEnvironmentVariables(path), program, StringComparison.OrdinalIgnoreCase) || !(bool)rule.Enabled || (int)rule.Direction != 1 || ((int)rule.Profiles & active) == 0) continue;
                    int protocol = rule.Protocol;
                    if (protocol != 17 && protocol != 256) continue;
                    string ports = protocol == 256 ? "*" : rule.LocalPorts ?? "*";
                    if ((int)rule.Action == 0 && (Includes(ports, videoPort) || Includes(ports, controlPort))) blocked = true;
                    if ((int)rule.Action == 1) { video |= Includes(ports, videoPort); control |= Includes(ports, controlPort); }
                }
                finally { if (Marshal.IsComObject(rule)) Marshal.ReleaseComObject(rule); }
            }
            return new(true, video && control && !blocked, profile);
        }
        catch { return new(false, false, "Network access not verified"); }
        finally { if (raw is not null && Marshal.IsComObject(raw)) Marshal.ReleaseComObject(raw); }
    }
}
