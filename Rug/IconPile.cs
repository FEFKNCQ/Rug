using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace Rug;

internal static class IconPile
{
    private const uint LVM_GETITEMCOUNT = 0x1004;
    private const uint LVM_GETITEMPOSITION = 0x1010;
    private const uint LVM_SETITEMPOSITION = 0x100F;
    private const int GWL_STYLE = -16;
    private const long LVS_AUTOARRANGE = 0x0100;
    private const uint SMTO_ABORTIFHUNG = 0x0002;
    private const uint PROCESS_VM_OPERATION = 0x0008;
    private const uint PROCESS_VM_READ = 0x0010;
    private const uint PROCESS_VM_WRITE = 0x0020;
    private const uint PROCESS_QUERY_INFORMATION = 0x0400;
    private const uint MEM_COMMIT = 0x1000;
    private const uint MEM_RESERVE = 0x2000;
    private const uint MEM_RELEASE = 0x8000;
    private const uint PAGE_READWRITE = 0x04;

    [StructLayout(LayoutKind.Sequential)] private struct POINT { public int x, y; }

    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [StructLayout(LayoutKind.Sequential)] private struct RECT { public int left, top, right, bottom; }
    [DllImport("user32.dll")] private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string? cls, string? title);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern IntPtr GetWindowLongPtr(IntPtr hWnd, int index);
    [DllImport("user32.dll")] private static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out IntPtr result);
    [DllImport("kernel32.dll")] private static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll")] private static extern bool VirtualFreeEx(IntPtr process, IntPtr address, UIntPtr size, uint type);
    [DllImport("kernel32.dll")] private static extern bool ReadProcessMemory(IntPtr process, IntPtr address, out POINT buffer, UIntPtr size, out UIntPtr read);

    public sealed class HomeData
    {
        public int Count { get; set; }
        public int[] X { get; set; } = Array.Empty<int>();
        public int[] Y { get; set; } = Array.Empty<int>();
    }

    private static string HomeFile => Path.Combine(AppSettings.DataDir, "icon-home.json");

    public static bool HasHome => File.Exists(HomeFile);

    public static string? Pile(IntPtr desktopHost, int screenWidth, int screenHeight, float[] rugBounds)
    {
        IntPtr listView = FindListView(desktopHost);
        if (listView == IntPtr.Zero) return "Could not find the desktop icons.";
        if ((GetWindowLongPtr(listView, GWL_STYLE).ToInt64() & LVS_AUTOARRANGE) != 0)
            return "Desktop auto arrange is on, so the icons snap back. Right-click the desktop, View, and turn off \"Auto arrange icons\".";

        IntPtr process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, false, ProcessIdOf(listView));
        if (process == IntPtr.Zero) return "Could not open Explorer to move the icons.";
        try
        {
            int count = ItemCount(listView);
            if (count <= 0) return "There are no desktop icons to move.";

            var x = new int[count];
            var y = new int[count];
            if (!ReadPositions(listView, process, count, x, y)) return "Could not read the icon positions.";

            if (!HasHome) SaveHome(count, x, y);

            GetWindowRect(listView, out RECT origin);
            float cx = (rugBounds[0] + rugBounds[2]) / 2f;
            float cy = (rugBounds[1] + rugBounds[3]) / 2f;
            float hw = (rugBounds[2] - rugBounds[0]) * 0.3f;
            float hh = (rugBounds[3] - rugBounds[1]) * 0.3f;
            int left = (int)(cx - hw) - origin.left;
            int right = (int)(cx + hw) - origin.left;
            int top = (screenHeight - (int)(cy + hh)) - origin.top;
            int bottom = (screenHeight - (int)(cy - hh)) - origin.top;

            var rng = new Random();
            int moved = 0;
            for (int i = 0; i < count; i++)
                if (SetPosition(listView, i, rng.Next(left, Math.Max(left + 1, right)), rng.Next(top, Math.Max(top + 1, bottom)))) moved++;
            return moved == 0 ? "Explorer did not accept the new icon positions." : null;
        }
        finally
        {
            CloseHandle(process);
        }
    }

    public static string? Restore(IntPtr desktopHost)
    {
        if (!HasHome) return "There is no saved layout to put back.";
        HomeData? home;
        try { home = JsonSerializer.Deserialize<HomeData>(File.ReadAllText(HomeFile)); }
        catch (Exception) { home = null; }
        if (home == null) return "The saved icon layout could not be read.";

        IntPtr listView = FindListView(desktopHost);
        if (listView == IntPtr.Zero) return "Could not find the desktop icons.";
        int count = ItemCount(listView);
        if (count != home.Count)
            return "The number of desktop icons has changed, so the old layout cannot be restored.";

        int restored = 0;
        for (int i = 0; i < count; i++)
            if (SetPosition(listView, i, home.X[i], home.Y[i])) restored++;
        if (restored == 0) return "Explorer did not accept the icon positions. The saved layout was kept.";
        File.Delete(HomeFile);
        return null;
    }

    private static void SaveHome(int count, int[] x, int[] y)
    {
        Directory.CreateDirectory(AppSettings.DataDir);
        File.WriteAllText(HomeFile, JsonSerializer.Serialize(new HomeData { Count = count, X = x, Y = y }));
    }

    private static IntPtr FindListView(IntPtr desktopHost)
    {
        IntPtr defView = FindWindowEx(desktopHost, IntPtr.Zero, "SHELLDLL_DefView", null);
        if (defView == IntPtr.Zero) return IntPtr.Zero;
        return FindWindowEx(defView, IntPtr.Zero, "SysListView32", null);
    }

    private static uint ProcessIdOf(IntPtr listView)
    {
        GetWindowThreadProcessId(listView, out uint pid);
        return pid;
    }

    private static int ItemCount(IntPtr listView)
    {
        SendMessageTimeout(listView, LVM_GETITEMCOUNT, IntPtr.Zero, IntPtr.Zero, SMTO_ABORTIFHUNG, 200, out IntPtr count);
        return count.ToInt32();
    }

    private static bool ReadPositions(IntPtr listView, IntPtr process, int count, int[] x, int[] y)
    {
        IntPtr remote = VirtualAllocEx(process, IntPtr.Zero, (UIntPtr)8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remote == IntPtr.Zero) return false;
        try
        {
            for (int i = 0; i < count; i++)
            {
                SendMessageTimeout(listView, LVM_GETITEMPOSITION, new IntPtr(i), remote, SMTO_ABORTIFHUNG, 200, out IntPtr ok);
                if (ok == IntPtr.Zero) return false;
                ReadProcessMemory(process, remote, out POINT p, (UIntPtr)8, out _);
                x[i] = p.x;
                y[i] = p.y;
            }
            return true;
        }
        finally
        {
            VirtualFreeEx(process, remote, UIntPtr.Zero, MEM_RELEASE);
        }
    }

    private static bool SetPosition(IntPtr listView, int index, int x, int y)
    {
        int packed = ((y & 0xFFFF) << 16) | (x & 0xFFFF);
        SendMessageTimeout(listView, LVM_SETITEMPOSITION, new IntPtr(index), new IntPtr(packed), SMTO_ABORTIFHUNG, 200, out IntPtr ok);
        return ok != IntPtr.Zero;
    }
}
