using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Rug;

internal static class DesktopIcons
{
    private const uint LVM_GETITEMCOUNT = 0x1004;
    private const uint LVM_GETITEMRECT = 0x100E;
    private const int LVIR_ICON = 1;
    private const uint SMTO_ABORTIFHUNG = 0x0002;

    private const uint PROCESS_VM_OPERATION = 0x0008;
    private const uint PROCESS_VM_READ = 0x0010;
    private const uint PROCESS_VM_WRITE = 0x0020;
    private const uint PROCESS_QUERY_INFORMATION = 0x0400;
    private const uint MEM_COMMIT = 0x1000;
    private const uint MEM_RESERVE = 0x2000;
    private const uint MEM_RELEASE = 0x8000;
    private const uint PAGE_READWRITE = 0x04;

    private const float BumpHeight = 22f;

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int left, top, right, bottom; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string? cls, string? title);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] private static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out IntPtr result);
    [DllImport("kernel32.dll")] private static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll")] private static extern bool VirtualFreeEx(IntPtr process, IntPtr address, UIntPtr size, uint type);
    [DllImport("kernel32.dll")] private static extern bool WriteProcessMemory(IntPtr process, IntPtr address, ref RECT buffer, UIntPtr size, out UIntPtr written);
    [DllImport("kernel32.dll")] private static extern bool ReadProcessMemory(IntPtr process, IntPtr address, out RECT buffer, UIntPtr size, out UIntPtr read);

    public static float[] Scan(IntPtr desktopHost, int screenWidth, int screenHeight)
    {
        IntPtr defView = FindWindowEx(desktopHost, IntPtr.Zero, "SHELLDLL_DefView", null);
        if (defView == IntPtr.Zero) return Array.Empty<float>();
        IntPtr listView = FindWindowEx(defView, IntPtr.Zero, "SysListView32", null);
        if (listView == IntPtr.Zero) return Array.Empty<float>();

        GetWindowThreadProcessId(listView, out uint pid);
        IntPtr process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, false, pid);
        if (process == IntPtr.Zero) return Array.Empty<float>();

        var bumps = new List<float>();
        try
        {
            SendMessageTimeout(listView, LVM_GETITEMCOUNT, IntPtr.Zero, IntPtr.Zero, SMTO_ABORTIFHUNG, 200, out IntPtr countResult);
            int count = countResult.ToInt32();
            if (count <= 0) return Array.Empty<float>();

            IntPtr remote = VirtualAllocEx(process, IntPtr.Zero, (UIntPtr)16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (remote == IntPtr.Zero) return Array.Empty<float>();
            try
            {
                GetWindowRect(listView, out RECT origin);
                for (int i = 0; i < count; i++)
                {
                    var request = new RECT { left = LVIR_ICON };
                    WriteProcessMemory(process, remote, ref request, (UIntPtr)16, out _);
                    SendMessageTimeout(listView, LVM_GETITEMRECT, new IntPtr(i), remote, SMTO_ABORTIFHUNG, 200, out IntPtr ok);
                    if (ok == IntPtr.Zero) continue;
                    ReadProcessMemory(process, remote, out RECT r, (UIntPtr)16, out _);

                    int w = r.right - r.left, h = r.bottom - r.top;
                    if (w <= 0 || h <= 0) continue;

                    float cx = origin.left + r.left + w / 2f;
                    float cy = origin.top + r.top + h / 2f;
                    if (cx < 0 || cx > screenWidth || cy < 0 || cy > screenHeight) continue;

                    float side = Math.Min(w, h);

                    float radius = Math.Max(18f, Math.Min(side, 64f) * 0.47f);
                    bumps.Add(cx);
                    bumps.Add(screenHeight - cy);
                    bumps.Add(radius);
                    bumps.Add(radius);
                    bumps.Add(BumpHeight);
                }
            }
            finally
            {
                VirtualFreeEx(process, remote, UIntPtr.Zero, MEM_RELEASE);
            }
        }
        finally
        {
            CloseHandle(process);
        }
        return bumps.ToArray();
    }
}
