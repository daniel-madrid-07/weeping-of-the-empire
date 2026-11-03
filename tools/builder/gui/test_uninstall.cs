// Uninstaller (Builder.cs) on a made-up install, compiled together with Builder.cs by test_uninstall.py: what Setup made
// goes; the player's own files, what a link leads to, other shortcuts and (unless asked) the saves stay
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;

static class UninstallTest
{
    static int fails;
    static void Expect(string name, bool ok, string detail = "")
    {
        Console.WriteLine("{0,-58} {1} {2}", name, ok ? "ok" : "FAIL", detail);
        if (!ok) fails++;
    }

    static void Write(string p, string text = "x")
    {
        Directory.CreateDirectory(Path.GetDirectoryName(p));
        File.WriteAllText(p, text);
    }

    static void Junction(string link, string target)
    {
        var p = Process.Start(new ProcessStartInfo("cmd.exe", "/d /c mklink /J \"" + link + "\" \"" + target + "\"")
                              { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true });
        p.StandardOutput.ReadToEnd(); p.WaitForExit();
    }

    // one made-up install under t: returns its root
    static string Make(string t, out string links, out string desktop, out string local, out string roaming, out string outside)
    {
        string root = Path.Combine(t, "My Games", "Weeping");   // (not a plain path: Setup reached it through a junction)
        foreach (var f in new[] { @"tools\llvm\bin\clang.exe", @"src\.git\objects\ab\cd", @"work\engine\objcache\aa\x.o", @"setup.log",
                                  @"tools\python\.setup.json", @"Setup.exe", @"game\TotK-PC-1\TotK Launcher.exe", @"game\TotK-PC-1\totk.exe" })
            Write(Path.Combine(root, f));
        File.WriteAllText(Path.Combine(root, ".setup-root"), root);   // (Setup's mark: the folder's own path)
        File.SetAttributes(Path.Combine(root, @"src\.git\objects\ab\cd"), FileAttributes.ReadOnly);   // (git's objects are read-only)
        Write(Path.Combine(root, @"game\TotK-PC-1\mods\MyMod\a.txt"));          // the player's mod beside the launcher
        Write(Path.Combine(root, "dump.nsp")); Write(Path.Combine(root, "prod.keys"));   // the player keeps them in the folder
        outside = Path.Combine(t, "elsewhere");                                  // a link inside the install leads here
        Write(Path.Combine(outside, "keep.txt"));
        Junction(Path.Combine(root, @"work\extracted"), outside);
        links = Path.Combine(t, "ProgramData", "WeepingOfTheEmpire");
        Directory.CreateDirectory(links);
        Junction(Path.Combine(links, Uninstaller.JunctionName(root)), root);
        desktop = Path.Combine(t, "Desktop");
        Write(Path.Combine(desktop, "Weeping of the Empire.lnk"));               // -> the install (through the junction)
        Write(Path.Combine(desktop, "Weeping of the Empire 1.4.3.lnk"));         // -> the install
        Write(Path.Combine(desktop, "Weeping of the Empire (old).lnk"));         // -> another folder: stays
        Write(Path.Combine(desktop, "Notes.lnk"));                               // not ours: stays
        local = Path.Combine(t, "Local", "nxrecomp", "TotK-engine");
        Write(Path.Combine(local, @"run\cache\glsl\a.tgz")); Write(Path.Combine(local, @"logs\totk-1.log"));
        roaming = Path.Combine(t, "Roaming", "nxrecomp", "TotK-engine");
        Write(Path.Combine(roaming, @"userdata\save\slot0.sav")); Write(Path.Combine(roaming, "settings.ini"));
        return root;
    }

    static int Main()
    {
        string t = Path.Combine(Path.GetTempPath(), "uninstall-" + Guid.NewGuid().ToString("N").Substring(0, 8));
        string links, desktop, local, roaming, outside;
        string root = Make(Path.Combine(t, "a"), out links, out desktop, out local, out roaming, out outside);
        string link = Path.Combine(links, Uninstaller.JunctionName(root));
        Func<string, string> target = lnk =>
            lnk.EndsWith("Empire.lnk") ? Path.Combine(link, @"game\TotK-PC-1\TotK Launcher.exe")
            : lnk.EndsWith("1.4.3.lnk") ? Path.Combine(root, @"game\TotK-PC-1-v143\TotK Launcher.exe")
            : lnk.EndsWith("(old).lnk") ? @"D:\Other\TotK Launcher.exe" : @"C:\Windows\notepad.exe";
        var r = Uninstaller.Run(root, new[] { links }, desktop, "Weeping of the Empire", local, null, target);
        Func<string, bool> has = p => File.Exists(p) || Directory.Exists(p);
        Expect("1 Setup's folders and files go", new[] { "tools", "src", "work", "setup.log", ".setup-root", "Setup.exe" }
               .All(n => !has(Path.Combine(root, n))) && !has(Path.Combine(root, @"game\TotK-PC-1\totk.exe")),
               string.Join(" | ", r.Failed));
        Expect("2 the player's dump and keys stay, and so does the folder", has(Path.Combine(root, "dump.nsp")) &&
               has(Path.Combine(root, "prod.keys")) && r.Kept.Any(k => k.EndsWith("dump.nsp")), string.Join(" | ", r.Kept));
        Expect("3 a link inside goes as a link: what it leads to stays", has(Path.Combine(outside, "keep.txt")));
        Expect("4 the junction goes (link only) and its empty parent", !has(link) && !has(links));
        Expect("5 the shortcuts into the install go, the others stay",
               !has(Path.Combine(desktop, "Weeping of the Empire.lnk")) && !has(Path.Combine(desktop, "Weeping of the Empire 1.4.3.lnk")) &&
               has(Path.Combine(desktop, "Weeping of the Empire (old).lnk")) && has(Path.Combine(desktop, "Notes.lnk")));
        Expect("6 caches and logs go, with their empty parent", !has(local) && !has(Path.GetDirectoryName(local)));
        Expect("7 saves and settings stay (not asked to delete them)", has(Path.Combine(roaming, @"userdata\save\slot0.sav")) &&
               has(Path.Combine(roaming, "settings.ini")));
        Expect("8 a game folder's mods stay with the saves", has(Path.Combine(root, @"game\TotK-PC-1\mods\MyMod\a.txt")));
        Expect("9 nothing failed", r.Failed.Count == 0, string.Join(" | ", r.Failed));

        // the second answer Yes: saves, settings and mods go too; a folder with nothing of the player's goes whole
        root = Make(Path.Combine(t, "b"), out links, out desktop, out local, out roaming, out outside);
        File.Delete(Path.Combine(root, "dump.nsp")); File.Delete(Path.Combine(root, "prod.keys"));
        r = Uninstaller.Run(root, new[] { links }, desktop, "Weeping of the Empire", local, roaming, lnk => null);
        Expect("10 with Yes: saves, settings and mods go", !has(roaming) && !has(Path.Combine(root, @"game\TotK-PC-1\mods")));
        Expect("11 nothing of the player's left: the install folder goes", !has(root) && r.Kept.Count == 0, string.Join(" | ", r.Kept));
        Expect("12 the link's target outside still there", has(Path.Combine(outside, "keep.txt")));
        // run from the install folder's own Setup.exe (Windows' Apps entry): that running file stays, everything else goes
        root = Make(Path.Combine(t, "d"), out links, out desktop, out local, out roaming, out outside);
        File.Delete(Path.Combine(root, "dump.nsp")); File.Delete(Path.Combine(root, "prod.keys"));
        string me = Path.Combine(root, "Setup.exe");
        r = Uninstaller.Run(root, new[] { links }, desktop, "Weeping of the Empire", local, roaming, lnk => null, me);
        Expect("16 the running Setup.exe stays (with its folder), the rest goes", File.Exists(me) &&
               !Directory.Exists(Path.Combine(root, "tools")) && !Directory.Exists(Path.Combine(root, "game")) &&
               Directory.GetFileSystemEntries(root).Length == 1 && r.Failed.Count == 0, string.Join(" | ", r.Kept));

        // Setup's real shortcuts (BuilderForm.Shortcut into a temp desktop): the game's and '<game> - Settings' (the same
        // launcher with --settings), and uninstall finds both by what they point at (Win32.ShortcutTarget)
        root = Make(Path.Combine(t, "e"), out links, out desktop, out local, out roaming, out outside);
        string desk = Path.Combine(t, "e", "RealDesktop"), launcher = Path.Combine(root, @"game\TotK-PC-1\TotK Launcher.exe");
        Directory.CreateDirectory(desk);
        string game = Path.Combine(desk, Release.Product + ".lnk"), settings = Path.Combine(desk, Release.Product + " - Settings.lnk");
        Expect("17 Setup saves the game's shortcut and '- Settings'", BuilderForm.Shortcut(launcher, desk) && File.Exists(game) &&
               File.Exists(settings), string.Join(" | ", Directory.GetFiles(desk).Select(Path.GetFileName)));
        Expect("18 both start the launcher; only '- Settings' passes --settings",
               string.Equals(Win32.ShortcutTarget(game), launcher, StringComparison.OrdinalIgnoreCase) &&
               string.Equals(Win32.ShortcutTarget(settings), launcher, StringComparison.OrdinalIgnoreCase) &&
               Win32.ShortcutArgs(game) == "" && Win32.ShortcutArgs(settings) == "--settings",
               Win32.ShortcutArgs(game) + " / " + Win32.ShortcutArgs(settings));
        r = Uninstaller.Run(root, new[] { links }, desk, Release.Product, local, null, Win32.ShortcutTarget);
        Expect("19 uninstall removes both", !File.Exists(game) && !File.Exists(settings), string.Join(" | ", r.Failed));

        // never a folder Setup did not install into: no mark, a mark naming another folder, a mark but no stamp
        string home = Path.Combine(t, "c", "Documents");
        foreach (var f in new[] { @"src\thesis.tex", @"work\notes.txt", @"game\save.dat", @"tools\x.txt" }) Write(Path.Combine(home, f));
        Func<bool> intact = () => File.Exists(Path.Combine(home, @"src\thesis.tex")) && File.Exists(Path.Combine(home, @"work\notes.txt")) &&
                                  File.Exists(Path.Combine(home, @"game\save.dat")) && File.Exists(Path.Combine(home, @"tools\x.txt"));
        r = Uninstaller.Run(home, new string[0], null, "Weeping of the Empire", null, null, lnk => null);
        Expect("13 no .setup-root: refused, its src / work / game kept", r.Refused != null && intact(), r.Refused ?? "");
        File.WriteAllText(Path.Combine(home, ".setup-root"), @"D:\Elsewhere");
        r = Uninstaller.Run(home, new string[0], null, "Weeping of the Empire", null, null, lnk => null);
        Expect("14 a .setup-root naming another folder: refused", r.Refused != null && intact(), r.Refused ?? "");
        File.WriteAllText(Path.Combine(home, ".setup-root"), home);
        r = Uninstaller.Run(home, new string[0], null, "Weeping of the Empire", null, null, lnk => null);
        Expect("15 the mark but none of Setup's tools or builds: refused", r.Refused != null && intact(), r.Refused ?? "");
        try { Directory.Delete(t, true); } catch { }
        Console.WriteLine(fails == 0 ? "all passed" : fails + " FAILED");
        return fails == 0 ? 0 : 1;
    }
}
