// Win32 (Builder.cs): the API calls that replaced cmd mklink / taskkill / explorer.exe / WScript.Shell do what those did.
// Compiled with Builder.cs by test_win32.py; its child processes are this exe (--sleep / --spawn), so no window opens.
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

static class Win32Test
{
    static int fails;
    static void Expect(string name, bool ok, string detail = "")
    {
        Console.WriteLine("{0,-44} {1} {2}", name, ok ? "ok" : "FAIL", detail);
        if (!ok) fails++;
    }

    static bool Alive(int pid) { try { using (var p = Process.GetProcessById(pid)) return !p.HasExited; } catch { return false; } }

    // the processes ended within `ms` (an ended process leaves the table after a moment; a loaded host takes longer)
    static bool Gone(int ms, params int[] pids)
    {
        for (int t = 0; t < ms; t += 100) { bool any = false; foreach (var p in pids) any |= Alive(p); if (!any) return true; Thread.Sleep(100); }
        return false;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObject(IntPtr a, string n);
    [StructLayout(LayoutKind.Sequential)]
    struct Basic { public long A, B; public uint Flags; public UIntPtr C, D; public uint E; public UIntPtr F; public uint G, H; }
    [StructLayout(LayoutKind.Sequential)] struct Io { public ulong A, B, C, D, E, F; }
    [StructLayout(LayoutKind.Sequential)] struct Ext { public Basic B; public Io I; public UIntPtr A, C, D, E; }
    [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr j, int c, ref Ext i, int s);
    [DllImport("kernel32.dll")] static extern bool AssignProcessToJobObject(IntPtr j, IntPtr p);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool TerminateJobObject(IntPtr j, uint code);

    static int Main(string[] args)
    {
        string self = Process.GetCurrentProcess().MainModule.FileName;
        if (args.Length > 1 && args[0] == "--sleep") { Thread.Sleep(int.Parse(args[1]) * 1000); return 0; }
        if (args.Length > 1 && args[0] == "--spawn")   // a parent with a child below it, for KillTree
        {
            var c = Process.Start(new ProcessStartInfo(self, "--sleep 60") { UseShellExecute = false });
            File.WriteAllText(args[1], c.Id.ToString());
            Thread.Sleep(60000);
            return 0;
        }
        if (args.Length > 1 && args[0] == "--in-job")   // inside a kill-on-close job that allows breakaway, as Setup is
        {
            IntPtr job = CreateJobObject(IntPtr.Zero, null);
            var info = new Ext(); info.B.Flags = 0x2000 | 0x800;
            SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(typeof(Ext)));
            AssignProcessToJobObject(job, Process.GetCurrentProcess().Handle);
            var inside = Process.Start(new ProcessStartInfo(self, "--sleep 60") { UseShellExecute = false });
            int outside = Win32.StartOutsideJob(self, "--sleep 60");
            File.WriteAllText(args[1], inside.Id + " " + outside);
            return 0;   // (exiting closes the job: KILL_ON_JOB_CLOSE ends what is still in it)
        }

        string tmp = Path.Combine(Path.GetTempPath(), "win32test-" + Guid.NewGuid().ToString("N").Substring(0, 8));
        Directory.CreateDirectory(tmp);
        try
        {
            // junction: made without administrator, reads through to the target; removing it keeps the target
            string target = Path.Combine(tmp, "Prueba ñandú dir"), link = Path.Combine(tmp, "j0123456789");
            Directory.CreateDirectory(target);
            File.WriteAllText(Path.Combine(target, ".setup-root"), target);
            bool made = Win32.Junction(link, target);
            Expect("junction: made", made);
            Expect("junction: reads through", made && File.Exists(Path.Combine(link, ".setup-root")) &&
                   File.ReadAllText(Path.Combine(link, ".setup-root")) == target);
            Expect("junction: is a reparse point", made && (new DirectoryInfo(link).Attributes & FileAttributes.ReparsePoint) != 0);
            if (made) Directory.Delete(link);
            Expect("junction: removing it keeps the target", File.Exists(Path.Combine(target, ".setup-root")));
            Expect("junction: over a folder with files: refused, files kept", !Win32.Junction(target, tmp) &&
                   File.Exists(Path.Combine(target, ".setup-root")));

            // a drive root keeps its backslash (\??\C:\): the link reads through to the root
            string rootLink = Path.Combine(tmp, "jroot");
            bool rootMade = Win32.Junction(rootLink, Path.GetPathRoot(Environment.SystemDirectory));
            Expect("junction: to a drive root", rootMade && Directory.Exists(Path.Combine(rootLink, "Windows")));
            if (rootMade) Directory.Delete(rootLink);   // (non-recursive: the link only)

            // Stop's build job (BuildJob): a nested kill-on-close job, ended by TerminateJobObject, takes the whole tree
            string pidFile2 = Path.Combine(tmp, "child2.pid");
            var p2 = Process.Start(new ProcessStartInfo(self, "--spawn \"" + pidFile2 + "\"") { UseShellExecute = false });
            IntPtr bj = CreateJobObject(IntPtr.Zero, null);
            var bi = new Ext(); bi.B.Flags = 0x2000;
            bool joined = SetInformationJobObject(bj, 9, ref bi, Marshal.SizeOf(typeof(Ext))) && AssignProcessToJobObject(bj, p2.Handle);
            for (int i = 0; i < 50 && !File.Exists(pidFile2); i++) Thread.Sleep(100);
            int child2 = int.Parse(File.ReadAllText(pidFile2));
            Expect("build job: joined", joined);
            TerminateJobObject(bj, 1);
            Expect("build job: parent and child ended at once", Gone(10000, p2.Id, child2));
            CloseHandle(bj);

            // KillTree: the parent and its child both end
            string pidFile = Path.Combine(tmp, "child.pid");
            var parent = Process.Start(new ProcessStartInfo(self, "--spawn \"" + pidFile + "\"") { UseShellExecute = false });
            for (int i = 0; i < 50 && !File.Exists(pidFile); i++) Thread.Sleep(100);
            int child = int.Parse(File.ReadAllText(pidFile));
            Expect("killtree: both running first", Alive(parent.Id) && Alive(child));
            Win32.KillTree(parent.Id);
            Expect("killtree: parent and child ended", Gone(10000, parent.Id, child));

            // StartOutsideJob: a process started from a kill-on-close job outlives it; one started inside does not
            string jobFile = Path.Combine(tmp, "job.pids");
            using (var j = Process.Start(new ProcessStartInfo(self, "--in-job \"" + jobFile + "\"") { UseShellExecute = false })) j.WaitForExit(20000);
            Thread.Sleep(500);
            var ids = File.ReadAllText(jobFile).Split(' ');
            int inside2 = int.Parse(ids[0]), outside2 = int.Parse(ids[1]);
            Expect("outside job: survives the job's close", outside2 > 0 && Alive(outside2), ids[1]);
            Expect("inside job: ended with it", Gone(10000, inside2));
            if (outside2 > 0) Win32.KillTree(outside2);

            // shortcuts: saved and read back
            string lnk = Path.Combine(tmp, "Game.lnk");
            Win32.SaveShortcut(lnk, self, Path.GetDirectoryName(self));
            Expect("shortcut: saved", File.Exists(lnk));
            Expect("shortcut: target read back", string.Equals(Win32.ShortcutTarget(lnk), self, StringComparison.OrdinalIgnoreCase),
                   Win32.ShortcutTarget(lnk) ?? "null");
            Expect("shortcut: not a link -> null", Win32.ShortcutTarget(Path.Combine(tmp, "none.lnk")) == null);
        }
        finally { try { Directory.Delete(tmp, true); } catch { } }
        Console.WriteLine(fails == 0 ? "PASS" : "FAIL " + fails);
        return fails == 0 ? 0 : 1;
    }
}
