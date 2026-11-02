// Fetch.Download (Builder.cs) against test_fetch.py's local server: a whole file, a line that drops every response, a
// .part left by another file, a wrong checksum, an unreachable host. No window, no real download.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

static class FetchTest
{
    static int fails;
    static void Expect(string name, bool ok, string detail = "")
    {
        Console.WriteLine("{0,-62} {1} {2}", name, ok ? "ok" : "FAIL", detail);
        if (!ok) fails++;
    }

    static int Main(string[] args)
    {
        string url = args[0], sha = args[1], dir = args[2];
        byte[] blob = File.ReadAllBytes(args[3]);
        Fetch.PauseMs = 0;
        var notes = new List<string>(); var rates = new List<double>();
        Func<string, string, Fetch.Item> item = (u, s) => new Fetch.Item { Name = "blob", Url = u, Sha256 = s };
        Action<long, long, double> progress = (dn, tt, rr) => rates.Add(rr);
        Func<bool> never = () => false;

        string a = Path.Combine(dir, "a.zip");
        Fetch.Download(item(url + "/blob", sha), a, progress, never, notes.Add);
        Expect("1 a whole file, checksum ok, no .part left", File.ReadAllBytes(a).SequenceEqual(blob) && !File.Exists(a + ".part"));

        string b = Path.Combine(dir, "b.zip");
        notes.Clear();
        Fetch.Download(item(url + "/flaky", sha), b, progress, never, notes.Add);
        Expect("2 a line that drops every response still finishes (resumes)", File.ReadAllBytes(b).SequenceEqual(blob) &&
               notes.Count >= 5 && notes.All(n => n.Contains("retrying in")), notes.Count + " retries");

        string c = Path.Combine(dir, "c.zip");
        File.WriteAllBytes(c + ".part", new byte[4096]);
        notes.Clear();
        Fetch.Download(item(url + "/blob", sha), c, progress, never, notes.Add);
        Expect("3 a .part of another file: once more from the start, then right", File.ReadAllBytes(c).SequenceEqual(blob) &&
               notes.Any(n => n.Contains("from the start")), string.Join(" | ", notes));

        string d = Path.Combine(dir, "d.zip");
        string err = null;
        try { Fetch.Download(item(url + "/blob", new string('0', 64)), d, progress, never, notes.Add); }
        catch (InvalidDataException e) { err = e.Message; }
        Expect("4 a wrong checksum: an error, and no file (whole or partial)", err != null && err.Contains("checksum") &&
               !File.Exists(d) && !File.Exists(d + ".part"), err ?? "");

        err = null;
        try { Fetch.Download(item("http://127.0.0.1:9/x", sha), Path.Combine(dir, "e.zip"), progress, never, notes.Add); }
        catch (IOException e) { err = e.Message; }
        Expect("5 unreachable: one plain sentence, says a rerun continues", err != null && err.Contains("cannot be reached") &&
               err.Contains("continues where it stopped"), err ?? "");

        err = null; var clock = System.Diagnostics.Stopwatch.StartNew();
        Fetch.PauseMs = 1000;   // (a retry would show as seconds)
        try { Fetch.Download(item(url + "/missing", sha), Path.Combine(dir, "f.zip"), progress, never, notes.Add); }
        catch (IOException e) { err = e.Message; }
        Fetch.PauseMs = 0;
        Expect("7 a 404: said at once (no retries), with the host and the code", err != null && err.Contains("answered 404") &&
               clock.Elapsed.TotalSeconds < 1.5, (err ?? "") + " " + clock.Elapsed.TotalSeconds.ToString("0.0") + " s");
        Expect("6 RateText: speed and time left", Fetch.RateText(0, 0, 0) == "" &&
               Fetch.RateText(50L << 20, 100L << 20, 2 << 20) == ", 2.0 MB/s, 25 s left" &&
               Fetch.RateText(0, 600L << 20, 1 << 20) == ", 1.0 MB/s, 10 min left" && Fetch.RateText(10, 0, 50 * 1024) == ", 50 KB/s",
               Fetch.RateText(50L << 20, 100L << 20, 2 << 20));
        Console.WriteLine(fails == 0 ? "all passed" : fails + " FAILED");
        return fails == 0 ? 0 : 1;
    }
}
