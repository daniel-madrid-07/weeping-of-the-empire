// Preflight (Builder.cs) checks, compiled together with Builder.cs by test_preflight.py (which also runs the no-driver case)
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

static class PreflightTest
{
    static int fails;
    static void Expect(string name, bool ok, string detail = "")
    {
        Console.WriteLine("{0,-28} {1} {2}", name, ok ? "ok" : "FAIL", detail);
        if (!ok) fails++;
    }

    static int Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--vkcheck") return Preflight.VulkanChild();   // (Check runs this exe as its child)
        if (args.Length > 0 && args[0] == "inputs")   // (test_preflight.py: inputs <case file>: lines dump TAB keys TAB update|-)
        {
            foreach (var line in File.ReadAllLines(args[1]))
            {
                var f = line.Split('\t');
                Console.WriteLine("INPUTS\t" + (Inputs.Check(f[0] == "-" ? null : f[0], f[1], f[2] == "-" ? null : f[2]) ?? ""));
            }
            return 0;
        }
        bool novk = args.Length > 0 && args[0] == "novk";
        bool wine = args.Length > 0 && args[0] == "wine";   // (test_preflight.py: SETUP_WINE=1, Setup's Wine paths on Windows)
        if (args.Length > 0 && args[0] == "proton")   // (SETUP_WINE=1 + STEAM_COMPAT_DATA_PATH: C: is the Steam entry's prefix)
        {
            string onC = Preflight.FolderProblem(@"C:\WeepingOfTheEmpire");
            Expect("proton: detected", Wine.Proton);
            Expect("proton: C: refused", onC != null && onC.StartsWith("Under Proton drive C:"), onC ?? "null");
            Console.WriteLine(fails == 0 ? "PASS" : "FAIL " + fails);
            return fails == 0 ? 0 : 1;
        }
        string tmp = Path.Combine(Path.GetTempPath(), "preflight-" + Guid.NewGuid().ToString("N").Substring(0, 8));
        string dir = Path.Combine(tmp, "install");
        Directory.CreateDirectory(dir);
        try
        {
            // (builder.py's sum: the data read in the dump by default, romfs.bin's 17 GB only when copied)
            Expect("need: first run", Preflight.NeedGb(dir) == 13, Preflight.NeedGb(dir).ToString());
            Expect("need: first run, copied", Preflight.NeedGb(dir, false, true) == 30, Preflight.NeedGb(dir, false, true).ToString());
            Directory.CreateDirectory(Path.Combine(dir, "tools", "llvm"));
            File.WriteAllText(Path.Combine(dir, "tools", "llvm", ".toolchain.json"), "{}");
            Expect("need: tools there", Preflight.NeedGb(dir) == 8, Preflight.NeedGb(dir).ToString());
            Directory.CreateDirectory(Path.Combine(dir, "work", "extracted", "v100"));
            Expect("need: extract unfinished", Preflight.NeedGb(dir) == 8);
            File.WriteAllText(Path.Combine(dir, "work", "extracted", "v100", "READY.json"), "{}");
            Expect("need: game extracted", Preflight.NeedGb(dir) == 9, Preflight.NeedGb(dir).ToString());
            Expect("need: in place, then copied", Preflight.NeedGb(dir, false, true) == 26, Preflight.NeedGb(dir, false, true).ToString());
            File.WriteAllText(Path.Combine(dir, "work", "extracted", "v100", "romfs.bin"), "");
            Expect("need: copy already there", Preflight.NeedGb(dir, false, true) == 9, Preflight.NeedGb(dir, false, true).ToString());
            File.Delete(Path.Combine(dir, "work", "extracted", "v100", "romfs.bin"));
            Expect("texts: first install 13 / 8 / 30 / 48", Preflight.FirstInstallGb(true, false) == 13 && Preflight.FirstInstallGb(false, false) == 8
                                                       && Preflight.FirstInstallGb(true, false, true) == 30 && Preflight.FirstInstallGb(true, true) == 48);
            // with the 1.4.3 update dump: its own version folder, and its RomFS folder unpacked (builder.py UPDATE_UNPACK_GB)
            Expect("need: update, 1.4.3 not out", Preflight.NeedGb(dir, true) == 43, Preflight.NeedGb(dir, true).ToString());
            Directory.CreateDirectory(Path.Combine(dir, "work", "extracted", "v143"));
            File.WriteAllText(Path.Combine(dir, "work", "extracted", "v143", "READY.json"), "{}");
            Expect("need: update, 1.4.3 extracted", Preflight.NeedGb(dir, true) == 9, Preflight.NeedGb(dir, true).ToString());

            Expect("exfat: an NTFS temp folder links", !Preflight.NoHardLinks(dir), new DriveInfo(Path.GetPathRoot(dir)).DriveFormat);
            foreach (var d in DriveInfo.GetDrives().Where(x => { try { return x.IsReady && x.DriveFormat == "exFAT"; } catch { return false; } }))
                Expect("exfat: " + d.Name + " said", Preflight.NoHardLinks(Path.Combine(d.Name, "WotE")));   // (only where this PC has one)

            // an existing install, by Setup's own files only (a player's own tools / game folders are not one)
            string mine = Path.Combine(tmp, "drive");
            Directory.CreateDirectory(Path.Combine(mine, "tools")); Directory.CreateDirectory(Path.Combine(mine, "game"));
            Expect("built: own tools/game folders, no", !BuilderForm.Built(mine));
            File.WriteAllText(Path.Combine(mine, ".setup-root"), Path.Combine(tmp, "elsewhere"));
            Expect("built: .setup-root of another folder, no", !BuilderForm.Built(mine));
            File.WriteAllText(Path.Combine(mine, ".setup-root"), mine + "\r\n");
            Expect("built: .setup-root naming it", BuilderForm.Built(mine + "\\"));
            Expect("built: the LLVM stamp", BuilderForm.Built(dir));

            string od = Path.Combine(tmp, "OneDrive");
            Environment.SetEnvironmentVariable("OneDrive", od);
            Expect("onedrive: inside", Preflight.FolderProblem(Path.Combine(od, "Games", "WotE")) != null);
            Expect("onedrive: the folder itself", Preflight.FolderProblem(od) != null);
            Expect("onedrive: sibling prefix", Preflight.FolderProblem(od + "Backup") == null);
            Expect("onedrive: elsewhere", Preflight.FolderProblem(dir) == null);
            // a game already built there (the launcher's update, --install-to): OneDrive was its own choice, not a refusal now
            Expect("onedrive: built there", Preflight.FolderProblem(Path.Combine(od, "Games", "WotE"), true) == null);
            Expect("onedrive: built, form", BuilderForm.FolderProblem(Path.Combine(od, "Games", "WotE"), true) == null);
            Expect("onedrive: form, new", BuilderForm.FolderProblem(Path.Combine(od, "Games", "WotE")) != null);
            // a dump in OneDrive can become an online-only stub: its data is copied, as from a drive that can go away
            Expect("onedrive: dump copied", BuilderForm.DumpDriveCanGo(Path.Combine(od, "Desktop", "TotK.nsp")) == "OneDrive");
            Expect("onedrive: dump beside it read in place", BuilderForm.DumpDriveCanGo(Path.Combine(od + "Backup", "TotK.nsp")) == null);
            Expect("writable: a new folder in temp", Preflight.FolderProblem(Path.Combine(tmp, "new", "deeper")) == null);
            Expect("writable: probe left nothing", Directory.GetDirectories(tmp).All(d => !Path.GetFileName(d).StartsWith(".setup-write-test")));
            string sys = Preflight.FolderProblem(Path.Combine(Environment.SystemDirectory, "nxr-preflight-test"));
            if (new System.Security.Principal.WindowsPrincipal(System.Security.Principal.WindowsIdentity.GetCurrent())
                    .IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator))
                Console.WriteLine("writable: System32 case skipped (elevated)");
            else
            {
                Expect("writable: System32 refused", sys != null && sys.StartsWith("Windows does not let this account"), sys ?? "");
                string built = BuilderForm.FolderProblem(Path.Combine(Environment.SystemDirectory, "nxr-preflight-test"), true);
                Expect("writable: refused if built", built != null && built.StartsWith("Windows does not let this account"), built ?? "");
            }
            Environment.SetEnvironmentVariable("OneDrive", null);
            Expect("cfa: read without admin", true, "block mode = " + Preflight.ControlledFolderAccess());

            // a tool an antivirus removed after Setup installed it (its stamp is there, its program is not)
            string tools = Path.Combine(dir, "tools");
            Expect("removed: nothing installed", BuilderForm.RemovedTool(tools) == null);
            foreach (var t in new[] { @"python\python.exe", @"git\cmd\git.exe", @"llvm\bin\clang.exe", @"llvm\bin\clang-cl.exe", @"llvm\bin\lld-link.exe" })
            {
                Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(tools, t)));
                File.WriteAllText(Path.Combine(tools, t), "");
                File.WriteAllText(Path.Combine(tools, t.Split('\\')[0], ".setup.json"), "{}");
            }
            Expect("removed: all there", BuilderForm.RemovedTool(tools) == null);
            File.Delete(Path.Combine(tools, @"llvm\bin\clang-cl.exe"));
            string gone = BuilderForm.RemovedTool(tools);
            Expect("removed: clang-cl named", gone != null && gone.EndsWith(@"llvm\bin\clang-cl.exe"), gone ?? "");

            // the Setup's own release assets: where they come from, and a copy next to the exe used only if it is the pinned one
            Expect("asset: llvm url (its pinned tag)", Fetch.Llvm.Url == "https://github.com/" + Release.Repo + "/releases/download/" + Release.LlvmTag + "/setup-llvm.zip", Fetch.Llvm.Url);
            Expect("asset: vulkan url", Fetch.Vulkan.Url.EndsWith("/releases/download/" + Release.VulkanTag + "/setup-vulkan-sdk.zip"), Fetch.Vulkan.Url);
            string side = Path.Combine(tmp, "side");
            Directory.CreateDirectory(side);
            File.WriteAllText(Path.Combine(side, "setup-llvm.zip"), "llvm bytes");
            var pinned = Fetch.Asset("t", "v1", "setup-llvm.zip", Fetch.Sha256(Path.Combine(side, "setup-llvm.zip")), "llvm", new string[0]);
            var other = Fetch.Asset("t", "v1", "setup-llvm.zip", new string('0', 64), "llvm", new string[0]);
            var absent = Fetch.Asset("t", "v1", "setup-vulkan-sdk.zip", new string('0', 64), "vulkan-sdk", new string[0]);
            Expect("asset: local copy, pinned sha", Fetch.Local(pinned, side) == Path.Combine(side, "setup-llvm.zip"));
            Expect("asset: local copy, other sha -> download", Fetch.Local(other, side) == null);
            Expect("asset: no local copy -> download", Fetch.Local(absent, side) == null);

            // Wine / Proton detection and its paths (on Windows: no wine_get_version; SETUP_WINE=1 forces the answer)
            Expect("wine: detected as forced", wine ? Wine.Active && Wine.Version == "forced" : !Wine.Active, Wine.Version ?? "windows");
            long freeHere = Wine.FreeBytes(dir), freeRoot = new DriveInfo(Path.GetPathRoot(dir)).AvailableFreeSpace;
            Expect("wine: free space of the folder", freeHere > 0 && Math.Abs(freeHere - freeRoot) < (1L << 30), freeHere + " vs " + freeRoot);
            Expect("wine: free space, folder not made yet", Math.Abs(Wine.FreeBytes(Path.Combine(dir, "a", "b")) - freeHere) < (1L << 30));
            Expect("wine: linux path, no export -> as is", Wine.LinuxPath(@"C:\x\y.exe") == @"C:\x\y.exe");
            Expect("wine: home (no Z: here)", Wine.Home() == null, Wine.Home() ?? "");
            string fsHere = Wine.FileSystem(Path.Combine(dir, "not", "yet"));   // (Windows answers for drive roots only: null, the drive's then)
            Expect("wine: file system, Windows -> the drive's", fsHere == null, fsHere ?? "null");
            Expect("wine: no linux link on Windows", !Wine.LinkFolder(Path.Combine(tmp, "link"), dir) && !Directory.Exists(Path.Combine(tmp, "link")));

            // --play's pick: the newest 1.0.0 build with a launcher, a 1.4.3 one only alone
            string inst = Path.Combine(tmp, "inst");
            Expect("play: nothing built", BuilderForm.NewestLauncher(inst) == null);
            Func<string, int, string> build = (name, age) =>
            {
                string d = Path.Combine(inst, "game", name);
                Directory.CreateDirectory(d);
                File.WriteAllText(Path.Combine(d, "TotK Launcher.exe"), "");
                Directory.SetLastWriteTimeUtc(d, DateTime.UtcNow.AddHours(-age));
                return Path.Combine(d, "TotK Launcher.exe");
            };
            string v143 = build("TotK-PC-20261102-0100-aaaa-v143", 0);
            Expect("play: a 1.4.3 build alone", BuilderForm.NewestLauncher(inst) == v143);
            build("TotK-PC-20261101-0100-bbbb", 0);   // (written last: a mod edited in a kept older build)
            string newer = build("TotK-PC-20261102-0000-cccc", 2);
            Directory.CreateDirectory(Path.Combine(inst, "game", "TotK-PC-20261102-0200-dddd.tmp"));
            Directory.CreateDirectory(Path.Combine(inst, "game", "TotK-PC-20261102-0300-eeee"));   // (no launcher: half made)
            Expect("play: newest 1.0.0 build, by name", BuilderForm.NewestLauncher(inst) == newer, BuilderForm.NewestLauncher(inst) ?? "null");
            string onC = Preflight.FolderProblem(@"C:\WeepingOfTheEmpire");   // (Proton alone refuses C:: the proton run)
            Expect("proton: C: kept unless Proton", onC == null || !onC.StartsWith("Under Proton"), onC ?? "null");

            // the child's verdict lines, as the parent reads them
            var vp = new List<string>(); var vn = new List<string>();
            Preflight.Verdict("result\tunfit\t1.2.0\t8086\tIntel(R) HD Graphics 620\tits driver offers Vulkan 1.2, 1.3 is needed", vp, vn);
            Expect("verdict: unfit -> problem", vp.Count == 1 && vp[0].StartsWith("Intel(R) HD Graphics 620 cannot run the game: its driver offers Vulkan 1.2")
                                                && vp[0].Contains("intel.com/support"), vp.Count > 0 ? vp[0] : "");
            vp.Clear(); vn.Clear();
            Preflight.Verdict("result\tunfit\t1.3.0\t1002\tAMD X\tits driver lacks VK_EXT_external_memory_host", vp, vn);
            Expect("verdict: missing extension", vp.Count == 1 && vp[0].Contains("lacks VK_EXT_external_memory_host") && vp[0].Contains("amd.com"));
            vp.Clear(); vn.Clear();
            Preflight.Verdict("result\tok\t1.4.0\t10de\tNVIDIA X", vp, vn);
            Expect("verdict: ok -> note", vp.Count == 0 && vn.Count == 1 && vn[0] == "Graphics: NVIDIA X, Vulkan 1.4.0");
            vp.Clear(); vn.Clear();
            Preflight.Verdict("result\terror\tvkCreateInstance -3", vp, vn);
            Expect("verdict: error -> note only", vp.Count == 0 && vn.Count == 1);
            vp.Clear(); vn.Clear();
            Preflight.Verdict("result\tnodevice", vp, vn);
            Expect("verdict: nodevice -> problem", vp.Count == 1 && vn.Count == 0);

            var problems = new List<string>(); var notes = new List<string>(); var details = new List<string>();
            Preflight.Check(dir, problems, notes, details);
            foreach (var p in problems) Console.WriteLine("  problem: " + p);
            foreach (var n in notes) Console.WriteLine("  note:    " + n);
            foreach (var d in details) Console.WriteLine("  log:     " + d);
            if (!novk) Expect("details: one line per GPU", details.Count > 0 && details.All(d => d.StartsWith("vkcheck: ")));
            if (novk)
                Expect("vulkan: no driver -> problem", problems.Any(p => p.StartsWith("No graphics driver with Vulkan")));
            else
            {
                Expect("check: no problems here", problems.Count == 0);
                Expect("vulkan: named GPU", notes.Any(n => n.StartsWith("Graphics: ") && n.Contains("Vulkan 1.")));
                Expect("crt: version noted", notes.Any(n => n.StartsWith("Visual C++ runtime in Windows: 14.")));
                Expect("wine: noted only under Wine", notes.Any(n => n.StartsWith("Running under Wine forced")) == wine);
            }
            // the VP9 note (#22, #25): one line either way; the missing case names the fallback and the Store package
            Expect("vp9: noted", notes.Count(n => n.StartsWith("Video: Windows has")) == 1);
            Expect("vp9: present", Preflight.Vp9Note(1).Contains("decoded by Windows"));
            Expect("vp9: missing -> fallback", Preflight.Vp9Note(0).Contains("libvpx") && Preflight.Vp9Note(0).Contains("\"VP9 Video Extensions\" from the Microsoft Store")
                   && !Preflight.Vp9Note(0).Contains("Media Feature Pack"));
            Expect("vp9: no MF -> N edition", Preflight.Vp9Note(-1).Contains("Media Feature Pack") && Preflight.Vp9Note(-1).Contains("libvpx"));
        }
        finally { try { Directory.Delete(tmp, true); } catch { } }
        Console.WriteLine(fails == 0 ? "PASS" : "FAIL " + fails);
        return fails == 0 ? 0 : 1;
    }
}
