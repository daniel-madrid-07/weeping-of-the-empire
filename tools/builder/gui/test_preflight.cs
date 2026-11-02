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
        bool novk = args.Length > 0 && args[0] == "novk";
        string tmp = Path.Combine(Path.GetTempPath(), "preflight-" + Guid.NewGuid().ToString("N").Substring(0, 8));
        string dir = Path.Combine(tmp, "install");
        Directory.CreateDirectory(dir);
        try
        {
            Expect("need: first run", Preflight.NeedGb(dir) == 30, Preflight.NeedGb(dir).ToString());
            Directory.CreateDirectory(Path.Combine(dir, "tools", "llvm"));
            File.WriteAllText(Path.Combine(dir, "tools", "llvm", ".toolchain.json"), "{}");
            Expect("need: tools there", Preflight.NeedGb(dir) == 25, Preflight.NeedGb(dir).ToString());
            Directory.CreateDirectory(Path.Combine(dir, "work", "extracted", "v100"));
            Expect("need: extract unfinished", Preflight.NeedGb(dir) == 25);
            File.WriteAllText(Path.Combine(dir, "work", "extracted", "v100", "READY.json"), "{}");
            Expect("need: game extracted", Preflight.NeedGb(dir) == 9, Preflight.NeedGb(dir).ToString());
            Expect("texts: first install 30 / 25 / 48", Preflight.FirstInstallGb(true, false) == 30 && Preflight.FirstInstallGb(false, false) == 25
                                                       && Preflight.FirstInstallGb(true, true) == 48);
            // with the 1.4.3 update dump: its own version folder, and its RomFS folder unpacked (builder.py UPDATE_UNPACK_GB)
            Expect("need: update, 1.4.3 not out", Preflight.NeedGb(dir, true) == 43, Preflight.NeedGb(dir, true).ToString());
            Directory.CreateDirectory(Path.Combine(dir, "work", "extracted", "v143"));
            File.WriteAllText(Path.Combine(dir, "work", "extracted", "v143", "READY.json"), "{}");
            Expect("need: update, 1.4.3 extracted", Preflight.NeedGb(dir, true) == 9, Preflight.NeedGb(dir, true).ToString());

            string od = Path.Combine(tmp, "OneDrive");
            Environment.SetEnvironmentVariable("OneDrive", od);
            Expect("onedrive: inside", Preflight.FolderProblem(Path.Combine(od, "Games", "WotE")) != null);
            Expect("onedrive: the folder itself", Preflight.FolderProblem(od) != null);
            Expect("onedrive: sibling prefix", Preflight.FolderProblem(od + "Backup") == null);
            Expect("onedrive: elsewhere", Preflight.FolderProblem(dir) == null);
            Expect("writable: a new folder in temp", Preflight.FolderProblem(Path.Combine(tmp, "new", "deeper")) == null);
            Expect("writable: probe left nothing", Directory.GetDirectories(tmp).All(d => !Path.GetFileName(d).StartsWith(".setup-write-test")));
            string sys = Preflight.FolderProblem(Path.Combine(Environment.SystemDirectory, "nxr-preflight-test"));
            if (new System.Security.Principal.WindowsPrincipal(System.Security.Principal.WindowsIdentity.GetCurrent())
                    .IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator))
                Console.WriteLine("writable: System32 case skipped (elevated)");
            else Expect("writable: System32 refused", sys != null && sys.StartsWith("Windows does not let this account"), sys ?? "");
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
