// Kit (Builder.cs): Setup's side of the download kit on a folder made here. Compiled and run by tools/builder/test_kit.py
// (arg: an empty folder).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

static class KitTest
{
    static int fails;
    static void Expect(string what, bool ok, string got = "")
    {
        Console.WriteLine((ok ? "PASS " : "FAIL ") + what + (ok || got.Length == 0 ? "" : ": " + got));
        if (!ok) fails++;
    }

    static int Main(string[] args)
    {
        string root = args[0], k = Path.Combine(root, "nxr-kit");
        Directory.CreateDirectory(Path.Combine(k, "files"));
        Expect("no kit.json: no kit", Kit.Dir(root) == null && Kit.Dir(k) == null);
        File.WriteAllText(Path.Combine(k, "kit.json"), "{\"format\": 1, \"setup\": \"v9.9.9\", \"complete\": false, \"files\": []}");
        Expect("the kit folder itself", Kit.Dir(k) == k);
        Expect("the folder holding nxr-kit", Kit.Dir(root) == k);
        Expect("its Setup tag", Kit.Tag(k) == "v9.9.9");
        Expect("not complete", !Kit.Complete(k));
        File.WriteAllText(Path.Combine(k, "kit.json"), "{\"format\": 1, \"setup\": \"v9.9.9\", \"complete\": true, \"files\": []}");
        Expect("complete", Kit.Complete(k));

        var data = Encoding.UTF8.GetBytes("a zip, say");
        string sha;
        using (var h = SHA256.Create()) sha = BitConverter.ToString(h.ComputeHash(data)).Replace("-", "").ToLowerInvariant();
        string zip = Path.Combine(root, "x.zip");
        File.WriteAllBytes(zip, data);
        var it = new Fetch.Item { Name = "Thing 1.0", Url = "https://example.invalid/x.zip", Sha256 = sha.ToUpperInvariant() };
        Expect("not in the kit yet", Kit.Find(k, sha) == null);
        Kit.Put(k, zip, it);
        string p = Path.Combine(k, "files", sha);
        Expect("Put: the file by its sha256", File.Exists(p) && File.ReadAllBytes(p).SequenceEqual(data));
        Expect("Put: listed for builder.py", Kit.Items.Count == 1 && Kit.Items[0] == sha + "\t" + data.Length + "\tThing 1.0\thttps://example.invalid/x.zip",
               Kit.Items.FirstOrDefault() ?? "");
        Expect("Find: by sha256 (any case)", Kit.Find(k, sha.ToUpperInvariant()) == p);
        Expect("Find: no pin, nothing", Kit.Find(k, "") == null && Kit.Find(k, null) == null);
        File.WriteAllText(p, "damaged");
        Expect("Find: a damaged copy is never used", Kit.Find(k, sha) == null);
        Kit.Put(k, zip, it);
        Expect("Put: a damaged copy is replaced", Kit.Find(k, sha) == p);
        Expect("no src.git: no source", Kit.Source(k) == null && Kit.Source(null) == null);
        Directory.CreateDirectory(Path.Combine(k, "src.git"));
        string src = Kit.Source(k);
        Expect("src.git as a file:// url", src != null && src.StartsWith("file:///") && src.EndsWith("/nxr-kit/src.git"), src ?? "");
        // the fetched commit Setup checks against Release.Commit: the commit for an annotated tag too (FETCH_HEAD then
        // holds the tag object), as for a lightweight one; none before a fetch
        if (args.Length > 1)
        {
            string git = args[1], a = Path.Combine(root, "a"), b = Path.Combine(root, "b");
            Func<string, string> run = x =>
            {
                var psi = new System.Diagnostics.ProcessStartInfo(git, x) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
                psi.EnvironmentVariables["GIT_CONFIG_NOSYSTEM"] = "1";
                using (var pr = System.Diagnostics.Process.Start(psi)) { string o = pr.StandardOutput.ReadToEnd().Trim(); pr.WaitForExit(); return o; }
            };
            run("init --quiet \"" + a + "\"");
            File.WriteAllText(Path.Combine(a, "f.txt"), "source");
            run("-C \"" + a + "\" add f.txt");
            run("-C \"" + a + "\" -c user.name=t -c user.email=t@t -c commit.gpgsign=false commit --quiet -m one");
            string commit = run("-C \"" + a + "\" rev-parse HEAD");
            run("-C \"" + a + "\" -c user.name=t -c user.email=t@t -c tag.gpgsign=false tag -a vA -m annotated");
            run("-C \"" + a + "\" tag vL");
            string tagObject = run("-C \"" + a + "\" rev-parse vA");
            run("init --quiet \"" + b + "\"");
            Expect("fetched commit: none before a fetch", BuilderForm.FetchedCommit(git, b) == null);
            string url = new Uri(a).AbsoluteUri;
            run("-C \"" + b + "\" fetch --quiet --depth 1 --force \"" + url + "\" vA");
            Expect("annotated tag: FETCH_HEAD is the tag object", tagObject != commit && File.ReadAllText(Path.Combine(b, ".git", "FETCH_HEAD")).StartsWith(tagObject));
            Expect("annotated tag: the commit it names", BuilderForm.FetchedCommit(git, b) == commit, BuilderForm.FetchedCommit(git, b) ?? "null");
            run("-C \"" + b + "\" fetch --quiet --depth 1 --force \"" + url + "\" vL");
            Expect("lightweight tag: its commit", BuilderForm.FetchedCommit(git, b) == commit, BuilderForm.FetchedCommit(git, b) ?? "null");
        }
        Console.WriteLine(fails == 0 ? "kit (Setup): all passed" : "kit (Setup): " + fails + " FAIL");
        return fails == 0 ? 0 : 1;
    }
}
