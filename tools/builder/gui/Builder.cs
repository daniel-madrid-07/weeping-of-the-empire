// Setup.exe / Builder.exe: drop the three files you dumped from your own console (the dump, the keys and the firmware),
// pick a folder and press Install. One window for two cases:
//   Setup:   the exe on its own (downloaded from the GitHub release). It fetches what the build needs into the install
//            folder (Python, git, LLVM's part and the Vulkan files from its own release's assets, this repository at the
//            exe's own release tag; each pinned by sha256), then runs tools/builder/builder.py
//            --install, which fetches the compilers and builds. Nothing is installed system-wide; no administrator.
//   Builder: the exe inside a clone of the repository (tools\builder\builder.py next to it): builds from that clone.
// Build: tools\builder\gui\build.ps1 (the C# compiler that ships with Windows, .NET Framework 4.x; no install needed).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

// Design tokens. A lab intake form: a graphite field, hairline rules, one cyan accent for "this is done".
static class T
{
    public static readonly Color Bg = Color.FromArgb(30, 33, 38);           // app background
    public static readonly Color RowHot = Color.FromArgb(39, 43, 49);       // row hover / drag-over
    public static readonly Color Rule = Color.FromArgb(56, 61, 69);         // hairline rules
    public static readonly Color Ring = Color.FromArgb(92, 100, 111);       // an empty status ring
    public static readonly Color Text = Color.FromArgb(236, 238, 241);      // primary text
    public static readonly Color TextDim = Color.FromArgb(166, 173, 182);   // secondary text
    public static readonly Color TextFaint = Color.FromArgb(124, 131, 141); // index numbers, log, footer
    public static readonly Color Accent = Color.FromArgb(94, 196, 210);     // the one accent
    public static readonly Color AccentHot = Color.FromArgb(124, 211, 222);
    public static readonly Color AccentInk = Color.FromArgb(9, 30, 34);     // text on the accent
    public static readonly Color ButtonOff = Color.FromArgb(43, 47, 54);
    public static readonly Color Error = Color.FromArgb(236, 118, 108);

    // Sizes in 96-dpi units, scaled to the display (the process is DPI-aware, so Windows never stretches a bitmap)
    public static readonly float K = Dpi() / 96f;
    public static int D(float px) { return (int)Math.Round(px * K); }
    static float Dpi() { using (var g = Graphics.FromHwnd(IntPtr.Zero)) return g.DpiX; }
    public static readonly int S2 = D(8), S3 = D(12), S4 = D(16), S5 = D(20), S6 = D(24), S8 = D(32), S10 = D(40);
    public static readonly int RadiusButton = D(6);

    public static readonly Font Display = Pick("Bahnschrift", 21f, FontStyle.Regular);
    public static readonly Font Index = Pick("Bahnschrift", 15f, FontStyle.Regular);
    public static readonly Font Status = Pick("Bahnschrift", 11.5f, FontStyle.Regular);
    public static readonly Font Body = Pick("Segoe UI", 10f, FontStyle.Regular);
    public static readonly Font Label = Pick("Segoe UI Semibold", 10.5f, FontStyle.Regular);
    public static readonly Font Small = Pick("Segoe UI", 9f, FontStyle.Regular);
    public static readonly Font Log = Pick("Cascadia Mono", 8.5f, FontStyle.Regular, "Consolas");

    static Font Pick(string family, float size, FontStyle style, string fallback = "Segoe UI")
    {
        var f = new Font(family, size, style);
        return f.Name == family ? f : new Font(fallback, size, style);
    }

    public static GraphicsPath Round(RectangleF r, float radius)
    {
        var p = new GraphicsPath(); float d = radius * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90); p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90); p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure(); return p;
    }

    public static void Hq(Graphics g) { g.SmoothingMode = SmoothingMode.AntiAlias; g.PixelOffsetMode = PixelOffsetMode.HighQuality; }
}

abstract class Painted : Control
{
    protected bool hot;
    protected Painted()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
        BackColor = T.Bg;
        MouseEnter += (s, e) => SetHot(true); MouseLeave += (s, e) => SetHot(false);
        GotFocus += (s, e) => Invalidate(); LostFocus += (s, e) => Invalidate();
    }
    protected void SetHot(bool h) { if (hot != h) { hot = h; Invalidate(); } }
}

// One requirement of the intake list: index, name, what to drop (or the file you dropped), and a status ring.
class Row : Painted
{
    readonly int index; readonly string title, hint;
    readonly Func<string, bool> accepts;
    public string FilePath { get; private set; }
    public bool Locked, FolderOnly, Clearable;   // Clearable: an optional row, emptied by a right-click
    public Func<string, string> Describe;   // the line under the title once set (default: the file name)
    public event Action Changed;
    public Action<string[]> DropMany;   // every dropped file goes to the row that takes it

    public Row(int index, string title, string hint, Func<string, bool> accepts)
    {
        this.index = index; this.title = title; this.hint = hint; this.accepts = accepts;
        AllowDrop = true; Cursor = Cursors.Hand; TabStop = true;
        DragEnter += (s, e) => { bool ok = !Locked && e.Data.GetDataPresent(DataFormats.FileDrop); e.Effect = ok ? DragDropEffects.Copy : DragDropEffects.None; SetHot(ok); };
        DragLeave += (s, e) => SetHot(false);
        DragDrop += (s, e) =>
        {
            SetHot(false);
            var files = e.Data.GetData(DataFormats.FileDrop) as string[];
            if (files == null) return;
            foreach (var f in files) if (accepts(f)) { Set(f); break; }
            if (DropMany != null) DropMany(files);
        };
        MouseClick += (s, e) => { if (e.Button == MouseButtons.Left) Browse(); };   // (a right-click is no choose)
        MouseUp += (s, e) => { if (e.Button == MouseButtons.Right && Clearable && !Locked && FilePath != null) { FilePath = null; Invalidate(); if (Changed != null) Changed(); } };
        KeyDown += (s, e) => { if (e.KeyCode == Keys.Enter || e.KeyCode == Keys.Space) Browse(); };
    }

    public bool Offer(string f) { if (FilePath == null && accepts(f)) { Set(f); return true; } return false; }

    public void Set(string p) { if (Locked) return; FilePath = p; Invalidate(); if (Changed != null) Changed(); }

    void Browse()
    {
        if (Locked) return;
        if (FolderOnly)
        {
            using (var d = new FolderBrowserDialog { Description = title, SelectedPath = FilePath ?? "", ShowNewFolderButton = true })
                if (d.ShowDialog() == DialogResult.OK)
                {
                    if (accepts(d.SelectedPath)) Set(d.SelectedPath);
                    else MessageBox.Show(BuilderForm.FolderProblem(d.SelectedPath) ?? "This folder cannot be used.", title);
                }
            return;
        }
        using (var d = new OpenFileDialog { Title = title, CheckFileExists = false, FileName = "(folder or file)" })
            if (d.ShowDialog() == DialogResult.OK)
            {
                var p = File.Exists(d.FileName) ? d.FileName : Path.GetDirectoryName(d.FileName);
                if (accepts(p)) Set(p);
            }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics; T.Hq(g);
        if (hot && !Locked) using (var b = new SolidBrush(T.RowHot)) g.FillRectangle(b, 0, 0, Width, Height - 1);
        using (var pen = new Pen(T.Rule)) g.DrawLine(pen, 0, Height - 1, Width, Height - 1);   // the rule under each row
        bool done = FilePath != null;
        int mid = Height / 2;
        TextRenderer.DrawText(g, index.ToString(), T.Index, new Rectangle(T.S2, 0, T.S8, Height), done ? T.Accent : T.TextFaint,
            TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
        int x = T.S2 + T.S8 + T.S2;
        TextRenderer.DrawText(g, title, T.Label, new Point(x, mid - T.D(19)), T.Text, TextFormatFlags.NoPadding);
        var sub = !done ? hint : Describe != null ? Describe(FilePath) : Path.GetFileName(FilePath.TrimEnd('\\'));
        TextRenderer.DrawText(g, sub, T.Small, new Rectangle(x, mid + T.D(3), Width - x - T.S10 - T.S4, T.D(18)),
            done ? T.TextDim : T.TextFaint, TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);
        // status ring: empty until the requirement is met, then a filled accent disc with a check
        float d = T.D(18), rx = Width - T.S4 - d, ry = mid - d / 2;
        if (!done) using (var pen = new Pen(Focused ? T.TextDim : T.Ring, 1.5f * T.K)) g.DrawEllipse(pen, rx, ry, d, d);
        else
        {
            using (var b = new SolidBrush(T.Accent)) g.FillEllipse(b, rx, ry, d, d);
            using (var pen = new Pen(T.AccentInk, 1.8f * T.K) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
                g.DrawLines(pen, new[] { new PointF(rx + d * .28f, ry + d * .52f), new PointF(rx + d * .44f, ry + d * .68f), new PointF(rx + d * .73f, ry + d * .35f) });
        }
    }
}

// One switch under the intake list, drawn as a Row: a check where the index goes, a title, and a line under it that says
// what the current choice does (it changes with the choice).
class OptionRow : Painted
{
    readonly string title; readonly Func<bool, string> says;
    bool on;
    public bool Locked;
    public bool Touched { get; private set; }   // the player chose (the default no longer follows the dump)
    public event Action Changed;

    public OptionRow(string title, Func<bool, string> says)
    {
        this.title = title; this.says = says;
        Cursor = Cursors.Hand; TabStop = true;
        MouseClick += (s, e) => { if (e.Button == MouseButtons.Left) Set(!on, true); };
        KeyDown += (s, e) => { if (e.KeyCode == Keys.Enter || e.KeyCode == Keys.Space) Set(!on, true); };
    }

    public bool Checked { get { return on; } }

    public void Set(bool value, bool byPlayer)
    {
        if (Locked && byPlayer) return;
        on = value; if (byPlayer) Touched = true;
        Invalidate(); if (Changed != null) Changed();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics; T.Hq(g);
        if (hot && !Locked) using (var b = new SolidBrush(T.RowHot)) g.FillRectangle(b, 0, 0, Width, Height - 1);
        using (var pen = new Pen(T.Rule)) g.DrawLine(pen, 0, Height - 1, Width, Height - 1);
        int mid = Height / 2;
        float d = T.D(16), bx = T.S2 + T.D(1), by = mid - d / 2;   // the check, in the rows' index column
        using (var path = T.Round(new RectangleF(bx, by, d, d), T.D(3)))
        {
            if (!on) using (var pen = new Pen(Focused ? T.TextDim : T.Ring, 1.5f * T.K)) g.DrawPath(pen, path);
            else
            {
                using (var b = new SolidBrush(Locked ? T.Ring : T.Accent)) g.FillPath(b, path);
                using (var pen = new Pen(T.AccentInk, 1.8f * T.K) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
                    g.DrawLines(pen, new[] { new PointF(bx + d * .26f, by + d * .53f), new PointF(bx + d * .43f, by + d * .70f), new PointF(bx + d * .75f, by + d * .33f) });
            }
        }
        int x = T.S2 + T.S8 + T.S2;
        TextRenderer.DrawText(g, title, T.Label, new Point(x, mid - T.D(19)), Locked ? T.TextDim : T.Text, TextFormatFlags.NoPadding);
        TextRenderer.DrawText(g, says(on), T.Small, new Rectangle(x, mid + T.D(3), Width - x - T.S4, T.D(18)),
            on ? T.TextDim : T.TextFaint, TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);
    }
}

class PrimaryButton : Painted
{
    bool down;
    public PrimaryButton()
    {
        Cursor = Cursors.Hand; TabStop = true;
        MouseDown += (s, e) => { down = true; Invalidate(); };
        MouseUp += (s, e) => { down = false; Invalidate(); };
        MouseLeave += (s, e) => { down = false; };
        KeyDown += (s, e) => { if (Enabled && (e.KeyCode == Keys.Enter || e.KeyCode == Keys.Space)) OnClick(EventArgs.Empty); };
        EnabledChanged += (s, e) => { Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); };
        TextChanged += (s, e) => Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics; T.Hq(g);
        var r = new RectangleF(0, 0, Width - 1, Height - 1);
        if (down && Enabled) r.Inflate(-1.5f * T.K, -1f * T.K);   // the press: a slight sink
        using (var path = T.Round(r, T.RadiusButton))
        using (var b = new SolidBrush(!Enabled ? T.ButtonOff : hot ? T.AccentHot : T.Accent)) g.FillPath(b, path);
        if (Focused && Enabled)
        {
            var f = r; f.Inflate(-3 * T.K, -3 * T.K);
            using (var fp = T.Round(f, T.RadiusButton - 2 * T.K)) using (var pen = new Pen(T.AccentInk, 1.2f * T.K)) g.DrawPath(pen, fp);
        }
        TextRenderer.DrawText(g, Text, T.Label, Rectangle.Round(r), Enabled ? T.AccentInk : T.TextFaint,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
    }
}

// The status line and its hairline track, then the last lines of the log (no scrollbar: it is a glance, not a reader).
class StatusView : Painted
{
    public string Step = ""; public float Value; public bool Failed, Running;
    readonly List<string> lines = new List<string>();
    const int Keep = 5;

    public void Add(string line) { lines.Add(line); if (lines.Count > Keep) lines.RemoveAt(0); Invalidate(); }
    public void Clear() { lines.Clear(); Invalidate(); }
    public void Remove(string line) { if (lines.Remove(line)) Invalidate(); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        TextRenderer.DrawText(g, Step, T.Status, new Rectangle(0, 0, Width, T.D(24)), Failed ? T.Error : T.Text,
            TextFormatFlags.NoPadding | TextFormatFlags.EndEllipsis);
        int y = T.D(28), h = Math.Max(1, T.D(2));
        using (var b = new SolidBrush(T.Rule)) g.FillRectangle(b, 0, y, Width, h);
        if (Running || Value > 0)
            using (var b = new SolidBrush(Failed ? T.Error : T.Accent)) g.FillRectangle(b, 0, y, Width * Math.Max(0f, Math.Min(1f, Value)), h);
        int ly = y + T.S3, lh = T.Log.Height + T.D(2);
        foreach (var l in lines)
        {
            TextRenderer.DrawText(g, l, T.Log, new Rectangle(0, ly, Width, lh), T.TextFaint, TextFormatFlags.EndEllipsis | TextFormatFlags.NoPadding);
            ly += lh;
        }
    }
}

// What Setup fetches before Python can take over: pinned downloads (sha256-checked; a .part file resumes) and the
// repository at this exe's release tag. Release.cs (written by build.ps1) holds the tag and the repository.
static class Fetch
{
    public class Item { public string Name, Url, Sha256, Dir, Strip; public string[] Keys; }   // Keys: files that must be there (else fetched again)

    // Python (python.org's NuGet package: a full CPython with venv and pip, no installer) and MinGit (git for Windows'
    // portable build for tools). Both unpack into <install>\tools.
    public static readonly Item Python = new Item
    {
        Name = "Python 3.13.14", Dir = "python", Strip = "tools/", Keys = new[] { "python.exe" },   // (the release PC's version: same package wheels)
        Url = "https://api.nuget.org/v3-flatcontainer/python/3.13.14/python.3.13.14.nupkg",
        Sha256 = "9ac15cfa6cab1115c83d48f2af55c554efa4d1bb044bbc4ab1c9d17ad426e16c",
    };
    public static readonly Item Git = new Item
    {
        Name = "MinGit 2.56.0", Dir = "git", Strip = "", Keys = new[] { @"cmd\git.exe" },
        Url = "https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.4/MinGit-2.56.0.4-64-bit.zip",
        Sha256 = "e3bf78351fe777fba47ddee5950082f8a98bb0f02604fdc7ba3ee2cb68ffa877",
    };

    // The part of LLVM the build runs and the Vulkan files it links: release assets, pinned in tools/builder/assets.json
    // (the release that carries each, its sha256) and baked in here by make_setup.py (a Builder.exe has none: the PC's
    // own tools serve). They used to ride inside the exe: 89 MB of compressed data in its code section, which antivirus
    // heuristics read as packed (issue #14).
    public static readonly Item Llvm = Asset("LLVM " + Release.LlvmVersion, Release.LlvmTag, Release.LlvmAsset, Release.LlvmSha256, "llvm",
                                             new[] { @"bin\clang.exe", @"bin\clang-cl.exe", @"bin\lld-link.exe" });
    public static readonly Item Vulkan = Asset("the Vulkan headers and libraries", Release.VulkanTag, Release.VulkanAsset, Release.VulkanSha256,
                                               "vulkan-sdk", new[] { @"Lib\vulkan-1.lib", @"Lib\shaderc_combined.lib" });

    // tag: the release that carries the file (pinned; an unchanged zip stays on the release that first carried it)
    public static Item Asset(string name, string tag, string file, string sha, string dir, string[] keys)
    {
        return new Item { Name = name, Dir = dir, Strip = "", Keys = keys, Sha256 = sha,
                          Url = "https://github.com/" + Release.Repo + "/releases/download/" + tag + "/" + file };
    }

    // The item's file next to this exe, if it is the pinned one (a release folder before it is published, a zip with
    // the Setup and its files loose): used instead of the download
    public static string Local(Item it, string dir)
    {
        try
        {
            string p = Path.Combine(dir, Path.GetFileName(new Uri(it.Url).LocalPath));
            return File.Exists(p) && string.Equals(Sha256(p), it.Sha256, StringComparison.OrdinalIgnoreCase) ? p : null;
        }
        catch { return null; }
    }

    // Download url to path with progress(done, total, bytes a second); resumes a .part file after a cut, with a growing
    // pause (2, 4, 8 ... 60 s, said through note) between tries, and gives up only after 6 failures in a row that gained
    // no byte (a slow line that keeps dropping still finishes). path exists only once complete and its sha256 checked:
    // a half file is never used; a resumed .part whose checksum fails (another version's file of the same name) is
    // fetched once more from the start.
    public static void Download(Item it, string path, Action<long, long, double> progress, Func<bool> cancelled, Action<string> note)
    {
        ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;   // TLS 1.2 (.NET 4.x defaults lower)
        string part = path + ".part";
        int fails = 0; bool refetched = false;
        while (true)
        {
            long start = File.Exists(part) ? new FileInfo(part).Length : 0;
            Exception err = null;
            bool complete = false;
            try
            {
                long have = start;
                var req = (HttpWebRequest)WebRequest.Create(it.Url);
                req.UserAgent = "Setup"; req.Timeout = 30000; req.ReadWriteTimeout = 60000;
                if (have > 0) req.AddRange(have);
                using (var resp = (HttpWebResponse)req.GetResponse())
                {
                    bool partial = resp.StatusCode == HttpStatusCode.PartialContent;
                    if (!partial) have = start = 0;   // (the server ignored the range: start over)
                    long total = resp.ContentLength < 0 ? -1 : resp.ContentLength + have;
                    using (var src = resp.GetResponseStream())
                    using (var dst = new FileStream(part, partial ? FileMode.Append : FileMode.Create, FileAccess.Write))
                    {
                        var buf = new byte[1 << 16]; int n; long done = have, markDone = have; double rate = 0;
                        var clock = Stopwatch.StartNew(); double last = 0, markT = 0;
                        while ((n = src.Read(buf, 0, buf.Length)) > 0)
                        {
                            if (cancelled()) throw new OperationCanceledException();
                            dst.Write(buf, 0, n); done += n;
                            double now = clock.Elapsed.TotalSeconds;
                            if (now - last >= 1.5)
                            {
                                double inst = (done - markDone) / Math.Max(now - markT, 1e-3);
                                rate = rate == 0 ? inst : 0.7 * rate + 0.3 * inst;
                                markDone = done; markT = now; last = now;
                                progress(done, total, rate);
                            }
                        }
                        if (total > 0 && done < total) throw new IOException("the connection closed early");
                    }
                }
                complete = true;
            }
            catch (WebException e)
            {
                var r = e.Response as HttpWebResponse;
                int code = r == null ? 0 : (int)r.StatusCode;
                if (code == 416) complete = true;   // (a stale .part at least as long as the file: checked as it is)
                else if (code >= 400 && code < 500 && code != 408 && code != 429)   // (no retry helps: said at once)
                    throw new IOException(string.Format("Could not download {0}: {1} answered {2} for it. Press Install again later; if it stays " +
                                                        "like this, report it.", it.Name, new Uri(it.Url).Host, code));
                else err = e;
            }
            catch (IOException e)   // (a connection cut mid-body, or the disk)
            {
                int hr = Marshal.GetHRForException(e);
                if (hr == unchecked((int)0x80070070) || hr == unchecked((int)0x80070027))   // ERROR_DISK_FULL, ERROR_HANDLE_DISK_FULL
                    throw new IOException(string.Format("Could not download {0}: the disk is full ({1}). Free some space, then press Install " +
                                                        "again: the download continues where it stopped.", it.Name, Path.GetDirectoryName(path)));
                err = e;
            }
            if (complete)
            {
                string sum = Sha256(part);
                if (!string.Equals(sum, it.Sha256, StringComparison.OrdinalIgnoreCase))
                {
                    File.Delete(part);
                    if (start > 0 && !refetched) { refetched = true; note(it.Name + ": the resumed file does not match its checksum: from the start"); continue; }
                    throw new InvalidDataException(string.Format("{0}: the download does not match its checksum (got {1}); press Install again", it.Name, sum));
                }
                if (File.Exists(path)) File.Delete(path);
                File.Move(part, path);
                return;
            }
            bool gained = File.Exists(part) && new FileInfo(part).Length > start;
            fails = gained ? 1 : fails + 1;
            if (fails >= 6)
            {
                string host = new Uri(it.Url).Host;
                var we = err as WebException;
                bool offline = we != null && (we.Status == WebExceptionStatus.NameResolutionFailure || we.Status == WebExceptionStatus.ConnectFailure ||
                                              we.Status == WebExceptionStatus.ProxyNameResolutionFailure || we.Status == WebExceptionStatus.Timeout);
                throw new IOException(string.Format("Could not download {0}: {1} cannot be reached ({2}). {3}Then press Install again: it continues where it stopped.",
                    it.Name, host, err == null ? "?" : err.Message,
                    offline ? "Check that this PC is connected to the internet; a firewall, proxy or school / work network can block " + host + ". "
                            : "Check the internet connection. "));
            }
            int pause = Math.Min(60, 1 << fails);
            note(string.Format("{0}: {1}; retrying in {2} s", it.Name, err == null ? "?" : err.Message, pause));
            for (long w = 0; w < (long)pause * PauseMs; w += 250) { if (cancelled()) throw new OperationCanceledException(); Thread.Sleep((int)Math.Min(250, (long)pause * PauseMs - w)); }
        }
    }

    public static int PauseMs = 1000;   // (a second of the retry pause; the tests make it 0)

    // ', 2.1 MB/s, 3 min left' for the progress line ('' before there is a speed)
    public static string RateText(long done, long total, double rate)
    {
        if (rate <= 0) return "";
        string s = rate >= 1048576 / 10.0 ? string.Format(System.Globalization.CultureInfo.InvariantCulture, ", {0:0.0} MB/s", rate / 1048576) : string.Format(System.Globalization.CultureInfo.InvariantCulture, ", {0:0} KB/s", rate / 1024);
        if (total > 0 && done < total)
        {
            double left = (total - done) / rate;
            s += left >= 90 ? string.Format(System.Globalization.CultureInfo.InvariantCulture, ", {0:0} min left", left / 60) : string.Format(System.Globalization.CultureInfo.InvariantCulture, ", {0:0} s left", left);
        }
        return s;
    }

    public static string Sha256(string path)
    {
        using (var s = File.OpenRead(path)) using (var h = SHA256.Create())
            return BitConverter.ToString(h.ComputeHash(s)).Replace("-", "").ToLowerInvariant();
    }

    // Unpack the zip's entries under it.Strip into dir (through a temporary folder, so a half-unpacked dir never looks done)
    public static void Unzip(Item it, string zip, string dir)
    {
        string tmp = dir + ".tmp";
        if (Directory.Exists(tmp)) Directory.Delete(tmp, true);
        using (var z = ZipFile.OpenRead(zip))
            foreach (var e in z.Entries)
            {
                string name = Uri.UnescapeDataString(e.FullName).Replace('\\', '/');   // (NuGet escapes some names)
                if (!name.StartsWith(it.Strip, StringComparison.OrdinalIgnoreCase) || name.EndsWith("/")) continue;
                string rel = name.Substring(it.Strip.Length);
                if (rel.Length == 0 || rel.Contains("..")) continue;
                string target = Path.Combine(tmp, rel.Replace('/', '\\'));
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                e.ExtractToFile(target, true);
            }
        if (Directory.Exists(dir)) Directory.Delete(dir, true);
        Directory.Move(tmp, dir);
    }
}

// Wine / Proton (Linux, Steam Deck): its ntdll exports wine_get_version, Windows' never does (Proton keeps it too).
// Setup takes another path where Wine differs from Windows: no kill-on-close job (Wine honours it, and its explorer.exe
// is a new process in the job, so the game Play starts through it would die with this window), the game and the
// uninstall started directly (no job to leave), free space asked of the folder itself (Z:\ is the Linux root, often
// another partition than /home), the default folder in the Linux home (C: is the Proton prefix, gone when Steam
// deletes it), no desktop shortcut (the prefix's Desktop, which no Linux desktop shows). SETUP_WINE=1 / 0 forces the
// answer (a test on Windows).
static class Wine
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, BestFitMapping = false)] static extern IntPtr GetProcAddress(IntPtr module, string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr VersionFn();
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool GetDiskFreeSpaceExW(string dir, out ulong freeToCaller, out ulong total, out ulong free);

    // null on Windows, else the Wine version ("9.0", "10.0-rc", or "forced")
    public static readonly string Version = Detect();
    public static bool Active { get { return Version != null; } }

    static string Detect()
    {
        string force = Environment.GetEnvironmentVariable("SETUP_WINE");
        if (force == "0") return null;
        try
        {
            IntPtr f = GetProcAddress(GetModuleHandleW("ntdll.dll"), "wine_get_version");
            if (f != IntPtr.Zero) return Marshal.PtrToStringAnsi(((VersionFn)Marshal.GetDelegateForFunctionPointer(f, typeof(VersionFn)))()) ?? "?";
        }
        catch { }
        return force == "1" ? "forced" : null;
    }

    // Steam's Proton (STEAM_COMPAT_DATA_PATH: the prefix's folder, set for every game it starts) rather than plain Wine
    public static bool Proton { get { return Active && !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("STEAM_COMPAT_DATA_PATH")); } }

    // Free bytes where dir really is (its nearest folder that exists): Windows' answer for a drive root, Wine's statfs()
    // of that very folder (a drive letter's root can sit on another Linux partition). -1: unknown
    public static long FreeBytes(string dir)
    {
        try
        {
            string at = Path.GetFullPath(dir);
            while (at != null && !Directory.Exists(at)) at = Path.GetDirectoryName(at);
            ulong caller, total, free;
            if (at != null && GetDiskFreeSpaceExW(at.EndsWith("\\") ? at : at + "\\", out caller, out total, out free)) return (long)caller;
        }
        catch { }
        try { return new DriveInfo(Path.GetPathRoot(Path.GetFullPath(dir))).AvailableFreeSpace; } catch { return -1; }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "wine_get_unix_file_name")] static extern IntPtr UnixName(string dos);
    [DllImport("kernel32.dll")] static extern IntPtr GetProcessHeap();
    [DllImport("kernel32.dll")] static extern bool HeapFree(IntPtr heap, uint flags, IntPtr mem);

    // A Windows path as Linux sees it (what a player types into Steam's "Add a Non-Steam Game"), else the path as is
    public static string LinuxPath(string p)
    {
        if (!Active || p == null) return p;
        try
        {
            IntPtr s = UnixName(p);   // (UTF-8, from the process heap)
            if (s == IntPtr.Zero) return p;
            int n = 0;
            while (Marshal.ReadByte(s, n) != 0) n++;
            var b = new byte[n];
            Marshal.Copy(s, b, 0, n);
            HeapFree(GetProcessHeap(), 0, s);
            return Encoding.UTF8.GetString(b);
        }
        catch { return p; }   // (SETUP_WINE=1 on Windows: no such export)
    }

    // A Linux symbolic link at link to the folder dir: Setup's junction under Wine (Proton 10's Wine makes no junctions;
    // a Linux link works with every Wine, which shows it as a folder and as a reparse point, so Uninstall removes the
    // link alone). ln runs as an exe would: Wine starts a Linux program it is asked to run (ntdll's fork_and_exec), but
    // with no process handle to wait on, so the link's arrival is the answer. link's folder must exist (the Linux name
    // of a path that does not exist yet is not asked for)
    public static bool LinkFolder(string link, string dir)
    {
        if (!Active) return false;
        string parent = LinuxPath(Path.GetDirectoryName(link)), ud = LinuxPath(dir);
        if (parent == null || ud == null || !parent.StartsWith("/") || !ud.StartsWith("/")) return false;
        string ln = new[] { @"Z:\usr\bin\ln", @"Z:\bin\ln" }.FirstOrDefault(File.Exists);
        if (ln == null) return false;
        try
        {
            var p = Process.Start(new ProcessStartInfo(ln, "-s -- \"" + ud + "\" \"" + parent.TrimEnd('/') + "/" + Path.GetFileName(link) + "\"")
                                  { UseShellExecute = false, CreateNoWindow = true });
            if (p != null) p.Dispose();
        }
        catch { }   // (a Process without a handle may throw after ln started: the folder below says)
        for (int i = 0; i < 100 && !Directory.Exists(link); i++) Thread.Sleep(100);
        return Directory.Exists(link);
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool GetVolumeInformationW(string root, StringBuilder name, int nameSize, IntPtr serial, IntPtr maxComponent, IntPtr flags,
                                             StringBuilder fs, int fsSize);

    // Under Wine the file system of the folder itself (its nearest folder that exists), as builder.py's filesystem():
    // Wine answers with the statfs() of the Linux file system holding it (a FAT32 SD card below /run/media), the drive
    // root (Z:\ = /) for /. null: not Wine, or no answer (then the drive's)
    public static string FileSystem(string dir)
    {
        if (!Active) return null;
        try
        {
            string at = Path.GetFullPath(dir);
            while (at != null && !Directory.Exists(at)) at = Path.GetDirectoryName(at);
            var fs = new StringBuilder(64);
            return at != null && GetVolumeInformationW(at.EndsWith("\\") ? at : at + "\\", null, 0, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, fs, 64) ? fs.ToString() : null;
        }
        catch { return null; }
    }

    // The Linux home as a Windows path (Wine maps / to Z:), or null: HOME comes through from Linux under Wine
    public static string Home()
    {
        string h = Environment.GetEnvironmentVariable("HOME");
        if (!Active || string.IsNullOrEmpty(h) || !h.StartsWith("/")) return null;
        string p = "Z:" + h.TrimEnd('/').Replace('/', '\\');
        return Directory.Exists(p) ? p : null;
    }
}

// What the PC must have before Setup downloads anything (the first seconds, not after 30 minutes of building): each
// problem is one sentence saying what to do. builder.py checks the same again (and the keys, the dump) once it runs.
static class Preflight
{
    const double MinRamGb = 7.5;                       // (builder.py MIN_RAM_GB: an "8 GB" PC reports ~7.8)
    // (builder.py FIRST_RUN_GB, COPY_ROMFS_GB, RESUME_GB, TOOLS_GB, UPDATE_UNPACK_GB)
    const int FirstRunGb = 8, CopyRomfsGb = 17, ResumeGb = 9, ToolsGb = 5, UpdateUnpackGb = 18;

    // What a first install asks for, as the window says it (Builder mode: no tools; copy: the game's data copied out of
    // the dump, romfs.bin; the 1.4.3 update: always copied, and its RomFS unpacked too)
    public static int FirstInstallGb(bool tools, bool update, bool copy = false)
    {
        return FirstRunGb + (copy || update ? CopyRomfsGb : 0) + (tools ? ToolsGb : 0) + (update ? UpdateUnpackGb : 0);
    }
    const uint Vk13 = (1u << 22) | (3u << 12);

    [DllImport("kernel32.dll")] static extern bool IsProcessorFeaturePresent(int feature);
    [StructLayout(LayoutKind.Sequential)]
    struct MemoryStatus { public uint Length, Load; public ulong Total, Avail, PageTotal, PageAvail, VirtTotal, VirtAvail, ExtAvail; }
    [DllImport("kernel32.dll")] static extern bool GlobalMemoryStatusEx(ref MemoryStatus m);

    // Problems (the install cannot work: Setup stops) and notes (said once, the install goes on); details: for setup.log
    // only (each GPU the check saw, for support). dir: the install folder (null in Builder mode: builder.py checks it).
    public static void Check(string dir, List<string> problems, List<string> notes, List<string> details = null, bool update = false,
                             bool copy = false)
    {
        if (!Environment.Is64BitOperatingSystem)
            problems.Add("This is 32-bit Windows; the game needs 64-bit Windows 10 or 11.");
        if (!(IsProcessorFeaturePresent(36) && IsProcessorFeaturePresent(37) && IsProcessorFeaturePresent(38)))   // PF_SSSE3, PF_SSE4_1, _2
            problems.Add("This processor is too old for the game: it needs SSE4.2 (Intel from 2008, AMD from 2011 on).");
        else if (!IsProcessorFeaturePresent(40))   // PF_AVX2_INSTRUCTIONS_AVAILABLE
            notes.Add("This processor has no AVX2: Setup builds the version for older processors (it runs a little slower).");
        var m = new MemoryStatus { Length = (uint)Marshal.SizeOf(typeof(MemoryStatus)) };
        if (GlobalMemoryStatusEx(ref m) && m.Total / 1073741824.0 < MinRamGb)
            problems.Add(string.Format("This PC has {0:0.0} GB of memory; building the game needs at least 8 GB.", m.Total / 1073741824.0));
        if (dir != null) Disk(dir, update, copy, problems);
        if (dir != null && NoHardLinks(dir))
            notes.Add("This drive is exFAT: the install works, but each update rebuilds the game from scratch (25 to 45 minutes: " +
                      "exFAT has no hard links, so no build cache is kept). A folder on an NTFS drive updates in minutes.");
        Vulkan(problems, notes, details ?? new List<string>());
        notes.Add(Vp9Note(Vp9Mft()));
        notes.Add("Visual C++ runtime in Windows: " + (CrtVersion() ?? "none"));
        if (Wine.Active) notes.Add("Running under Wine " + Wine.Version + (Wine.Proton ? " (Steam's Proton)" : "") + ": Setup takes its Linux paths (README, Linux and Steam Deck).");
    }

    // The cutscene movies are VP9: Windows decodes them with the Store's "VP9 Video Extensions" (a Media Foundation
    // MFT), which installs without the Store (LTSC, N, offline images) lack; the game then uses its own libvpx
    // (engine/runtime/movie_core.h; issues #22, #25). Not a problem either way: a note. 1 = MFT, 0 = none, -1 = no
    // Media Foundation at all (an N edition without the Media Feature Pack). The launcher says it too, once.
    [StructLayout(LayoutKind.Sequential)] struct MftTypeInfo { public Guid Major, Sub; }
    [DllImport("mfplat.dll", ExactSpelling = true)]
    static extern int MFTEnumEx(Guid category, uint flags, ref MftTypeInfo input, IntPtr output, out IntPtr activates, out uint count);

    static int Vp9Mft()
    {
        try
        {
            var input = new MftTypeInfo { Major = new Guid("73646976-0000-0010-8000-00aa00389b71"),     // MFMediaType_Video
                                          Sub = new Guid("30395056-0000-0010-8000-00aa00389b71") };     // MFVideoFormat_VP90
            IntPtr acts; uint n;
            // MFT_CATEGORY_VIDEO_DECODER; SYNCMFT | LOCALMFT | SORTANDFILTER | UNTRUSTED_STOREMFT, as the game enumerates
            int hr = MFTEnumEx(new Guid("d6c02d4b-6833-45b4-971a-05a4b04bab91"), 0x1 | 0x8 | 0x40 | 0x400, ref input, IntPtr.Zero, out acts, out n);
            for (uint i = 0; hr >= 0 && i < n; i++) Marshal.Release(Marshal.ReadIntPtr(acts, (int)i * IntPtr.Size));
            if (acts != IntPtr.Zero) Marshal.FreeCoTaskMem(acts);
            return hr >= 0 && n > 0 ? 1 : 0;
        }
        catch (DllNotFoundException) { return -1; }
        catch (EntryPointNotFoundException) { return -1; }
    }

    public static string Vp9Note(int mft)
    {
        if (mft > 0) return "Video: Windows has the VP9 Video Extensions; cutscenes are decoded by Windows (on the graphics card where it can).";
        return "Video: Windows has no " + (mft < 0 ? "Media Foundation (an N edition without the Media Feature Pack) and no " : "") +
               "VP9 Video Extensions. Cutscenes still play and look the same: the game decodes them with its own decoder (libvpx) on " +
               "the processor. To decode them on the graphics card, install \"VP9 Video Extensions\" from the Microsoft Store.";
    }

    // The same sum as builder.py's: a first run 8 GB, a run whose game files (of the version built: v100, or v143 with
    // the update dump) are already extracted 9; romfs.bin 17 more when the data is copied out of the dump (copy: the
    // window's switch, always with the update) and not yet there; the update's unpacked RomFS 18 until it is; the tools 5
    public static int NeedGb(string dir, bool update = false, bool copy = false)
    {
        string extracted = Path.Combine(dir, "work", "extracted");
        bool resume = File.Exists(Path.Combine(extracted, update ? "v143" : "v100", "READY.json"));
        bool copied = File.Exists(Path.Combine(extracted, "v100", "romfs.bin"));   // (switching an in-place install over)
        return (resume ? ResumeGb : FirstRunGb) + ((copy || update) && (!resume || (!update && !copied)) ? CopyRomfsGb : 0)
             + (update && !resume ? UpdateUnpackGb : 0)
             + (File.Exists(Path.Combine(dir, "tools", "llvm", ".toolchain.json")) ? 0 : ToolsGb);
    }

    static void Disk(string dir, bool update, bool copy, List<string> problems)
    {
        string p = DiskProblem(dir, update, copy);
        if (p != null) problems.Add(p);
    }

    // An exFAT drive (FAT32 is refused: BuilderForm.FolderProblem): the build copies instead of linking there
    // (tools/engine/build.py since 57feee4c) and keeps no object cache, so every update compiles every object again
    public static bool NoHardLinks(string dir)
    {
        try
        {
            string format = Wine.FileSystem(dir) ?? new DriveInfo(Path.GetPathRoot(Path.GetFullPath(dir))).DriveFormat;
            return string.Equals(format, "exFAT", StringComparison.OrdinalIgnoreCase);
        }
        catch { return false; }
    }

    // The install's free-space sentence, or null (the window says it again when the folder, the update or the copy
    // switch changes it)
    public static string DiskProblem(string dir, bool update, bool copy)
    {
        try
        {
            int need = NeedGb(dir, update, copy);
            long bytes = Wine.FreeBytes(dir);   // (the folder's own: under Wine Z:\ is / and the folder often on /home)
            double free = bytes / 1073741824.0;
            if (bytes >= 0 && free < need)
                return string.Format("Drive {0} has {1:0} GB free; this install needs about {2} GB. Free some space or choose a folder on another drive.",
                                     Wine.Active ? dir : Path.GetPathRoot(dir).TrimEnd('\\'), free, need);
        }
        catch { }   // (builder.py checks again)
        return null;
    }

    // A GPU must be able to run the game (Vulkan 1.3 and the rest of device.c's device_unfit). Asked in a child process
    // (this exe with --vkcheck): a broken driver can crash the process that loads it, and that must not be Setup's window.
    static void Vulkan(List<string> problems, List<string> notes, List<string> details)
    {
        string line = null;
        try
        {
            var psi = new ProcessStartInfo(Process.GetCurrentProcess().MainModule.FileName, "--vkcheck")
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
            using (var p = Process.Start(psi))
            {
                var read = p.StandardOutput.ReadToEndAsync();
                if (!p.WaitForExit(20000)) { try { p.Kill(); } catch { } notes.Add("Could not check the graphics driver (no answer in 20 s); going on."); return; }
                var lines = read.Result.Split('\n').Select(l => l.TrimEnd('\r')).ToList();
                details.AddRange(lines.Where(l => l.StartsWith("gpu\t")).Select(l => "vkcheck: " + l.Substring(4).Replace("\t", " | ").TrimEnd(' ', '|')));
                line = lines.LastOrDefault(l => l.StartsWith("result\t"));
            }
        }
        catch (Exception e) { notes.Add("Could not check the graphics driver (" + e.Message + "); going on."); return; }
        if (line == null) { notes.Add("Could not check the graphics driver (it stopped the check); going on."); return; }
        Verdict(line, problems, notes);
    }

    // The child's last line: result <TAB> ok|unfit|nodevice|noloader|error <TAB> api <TAB> vendor id <TAB> GPU <TAB> why
    public static void Verdict(string line, List<string> problems, List<string> notes)
    {
        var f = line.Split('\t');
        string kind = f.Length > 1 ? f[1] : "";
        if (kind == "noloader" || kind == "nodevice")
            problems.Add("No graphics driver with Vulkan was found. Install the current driver for your graphics card (from NVIDIA, AMD or Intel), then run Setup again.");
        else if (kind == "unfit" && f.Length == 6)
            problems.Add(string.Format("{0} cannot run the game: {1}. Update the graphics driver{2}, then run Setup again " +
                                       "(if it still says this, this graphics card cannot run the game).", f[4], f[5], Vendor(f[3])));
        else if (kind == "ok" && f.Length >= 5)
            notes.Add(string.Format("Graphics: {0}, Vulkan {1}", f[4], f[2]));
        else notes.Add("Could not check the graphics driver (" + string.Join(" ", f.Skip(1)) + "); going on.");
    }

    static string Vendor(string id)
    {
        switch (id)
        {
            case "10de": return " from nvidia.com/drivers";
            case "1002": return " from amd.com/support";
            case "8086": return " from intel.com/support";
            default: return "";
        }
    }

    // msvcp140.dll in System32: its version, or null when Windows has none (the game carries what it needs)
    static string CrtVersion()
    {
        try
        {
            var v = FileVersionInfo.GetVersionInfo(Path.Combine(Environment.SystemDirectory, "msvcp140.dll"));
            return string.Format("{0}.{1}.{2}", v.FileMajorPart, v.FileMinorPart, v.FileBuildPart);
        }
        catch { return null; }
    }

    // ---- --vkcheck: the child process. One line: result ok|old <api> <vendor id> <name> (the best real GPU) ----
    [DllImport("kernel32", CharSet = CharSet.Unicode)] static extern IntPtr LoadLibraryW(string name);
    [DllImport("vulkan-1")] static extern int vkEnumerateInstanceVersion(out uint version);
    [DllImport("vulkan-1")] static extern int vkCreateInstance(IntPtr info, IntPtr alloc, out IntPtr instance);
    [DllImport("vulkan-1")] static extern int vkEnumeratePhysicalDevices(IntPtr instance, ref uint count, IntPtr[] devices);
    [DllImport("vulkan-1")] static extern void vkGetPhysicalDeviceProperties(IntPtr device, IntPtr props);
    [DllImport("vulkan-1")] static extern void vkDestroyInstance(IntPtr instance, IntPtr alloc);
    [DllImport("vulkan-1")] static extern int vkEnumerateDeviceExtensionProperties(IntPtr device, IntPtr layer, ref uint count, IntPtr props);
    [DllImport("vulkan-1")] static extern void vkGetPhysicalDeviceFeatures(IntPtr device, IntPtr features);
    [DllImport("vulkan-1")] static extern void vkGetPhysicalDeviceProperties2(IntPtr device, IntPtr props2);

    static string Ver(uint v) { return string.Format("{0}.{1}.{2}", v >> 22, (v >> 12) & 0x3ff, v & 0xfff); }

    // Why a device cannot run the game (null: it can): device.c's device_unfit, all but the queue that presents to the
    // window (that needs one). Vulkan 1.3, VK_KHR_swapchain, VK_EXT_external_memory_host, shaderInt64, and the subgroup
    // operations basic | vote | ballot | shuffle | shuffle_relative in the vertex, fragment and compute stages.
    static string Unfit(IntPtr pd, uint api, uint loader)
    {
        if (api < Vk13) return string.Format("its driver offers Vulkan {0}.{1}, 1.3 is needed", api >> 22, (api >> 12) & 0x3ff);
        if (loader < Vk13) return string.Format("the Vulkan runtime in Windows is {0}.{1}, 1.3 is needed", loader >> 22, (loader >> 12) & 0x3ff);
        uint ne = 0;
        vkEnumerateDeviceExtensionProperties(pd, IntPtr.Zero, ref ne, IntPtr.Zero);
        var names = new HashSet<string>();
        IntPtr ep = Marshal.AllocHGlobal((int)Math.Max(1, ne) * 260);   // VkExtensionProperties: char extensionName[256], uint32 specVersion
        try
        {
            vkEnumerateDeviceExtensionProperties(pd, IntPtr.Zero, ref ne, ep);
            for (int i = 0; i < ne; i++) names.Add(Marshal.PtrToStringAnsi(new IntPtr(ep.ToInt64() + i * 260)));
        }
        finally { Marshal.FreeHGlobal(ep); }
        foreach (var e in new[] { "VK_KHR_swapchain", "VK_EXT_external_memory_host" })
            if (!names.Contains(e)) return "its driver lacks " + e;
        IntPtr feat = Marshal.AllocHGlobal(256), p2 = Marshal.AllocHGlobal(4096), sg = Marshal.AllocHGlobal(32);
        try
        {
            for (int i = 0; i < 256; i += 4) Marshal.WriteInt32(feat, i, 0);   // (zeroed: a pass is the driver's own 1)
            vkGetPhysicalDeviceFeatures(pd, feat);   // VkPhysicalDeviceFeatures: 55 VkBool32, shaderInt64 the 41st
            if (Marshal.ReadInt32(feat, 40 * 4) == 0) return "no 64-bit integers in shaders (shaderInt64)";
            for (int i = 0; i < 32; i += 4) Marshal.WriteInt32(sg, i, 0);
            Marshal.WriteInt32(sg, 0, 1000094000);   // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES
            Marshal.WriteInt32(p2, 0, 1000059001);   // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
            Marshal.WriteIntPtr(p2, 8, sg);          // .pNext
            vkGetPhysicalDeviceProperties2(pd, p2);
            int stages = Marshal.ReadInt32(sg, 20), ops = Marshal.ReadInt32(sg, 24);   // .supportedStages, .supportedOperations
            const int needOps = 0x01 | 0x02 | 0x08 | 0x10 | 0x20, needStages = 0x01 | 0x10 | 0x20;
            if ((ops & needOps) != needOps || (stages & needStages) != needStages)
                return string.Format("too few subgroup operations in shaders (ops 0x{0:x} in stages 0x{1:x})", ops, stages);
        }
        finally { Marshal.FreeHGlobal(feat); Marshal.FreeHGlobal(p2); Marshal.FreeHGlobal(sg); }
        return null;
    }

    static uint Loader()
    {
        uint v;
        try { if (vkEnumerateInstanceVersion(out v) == 0) return v; }
        catch (EntryPointNotFoundException) { }   // (a 1.0 loader has no such call)
        return 1u << 22;
    }

    // The whole child in one try: whatever a driver or an old loader throws ends as a result line, never a silent death
    public static int VulkanChild()
    {
        try { return VulkanChildBody(); }
        catch (Exception e) { Console.WriteLine("result\terror\t" + e.GetType().Name + ": " + e.Message); return 3; }
    }

    static int VulkanChildBody()
    {
        if (IntPtr.Size != 8) { Console.WriteLine("result\terror\tnot a 64-bit process"); return 3; }   // (the layouts below are x64's)
        if (LoadLibraryW("vulkan-1.dll") == IntPtr.Zero) { Console.WriteLine("result\tnoloader"); return 2; }
        uint loader = Loader();
        IntPtr app = Marshal.AllocHGlobal(48), info = Marshal.AllocHGlobal(64), props = Marshal.AllocHGlobal(4096);
        try
        {
            for (int i = 0; i < 48; i += 4) Marshal.WriteInt32(app, i, 0);   // VkApplicationInfo
            for (int i = 0; i < 64; i += 4) Marshal.WriteInt32(info, i, 0);  // VkInstanceCreateInfo
            Marshal.WriteInt32(app, 44, (int)Math.Min(loader, Vk13));        // .apiVersion
            Marshal.WriteInt32(info, 0, 1);                                  // .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
            Marshal.WriteIntPtr(info, 24, app);                              // .pApplicationInfo
            IntPtr inst;
            int r = vkCreateInstance(info, IntPtr.Zero, out inst);
            if (r == -9) { Console.WriteLine("result\tnodevice"); return 4; }   // VK_ERROR_INCOMPATIBLE_DRIVER: no driver speaks Vulkan
            if (r != 0) { Console.WriteLine("result\terror\tvkCreateInstance " + r); return 3; }
            uint n = 0;
            vkEnumeratePhysicalDevices(inst, ref n, null);
            var devs = new IntPtr[n];
            if (n > 0) vkEnumeratePhysicalDevices(inst, ref n, devs);
            // device.c's pick: the best device that can run the game, discrete before integrated before the rest; none
            // can: name the most capable one turned down and why. (A software renderer counts as no GPU here.)
            string fit = null, rejected = null; int fitScore = -1, rejScore = -1;
            for (int d = 0; d < n; d++)
            {
                vkGetPhysicalDeviceProperties(devs[d], props);   // VkPhysicalDeviceProperties: apiVersion, driverVersion, vendorID, deviceID, deviceType, deviceName[256]
                uint api = (uint)Marshal.ReadInt32(props, 0), vendor = (uint)Marshal.ReadInt32(props, 8), type = (uint)Marshal.ReadInt32(props, 16);
                var raw = new byte[256];
                Marshal.Copy(new IntPtr(props.ToInt64() + 20), raw, 0, 256);
                int len = Array.IndexOf(raw, (byte)0);
                string name = Encoding.UTF8.GetString(raw, 0, len < 0 ? 256 : len);
                string why = type == 4 ? "a software renderer" : Unfit(devs[d], api, loader);
                Console.WriteLine("gpu\t{0}\t{1:x4}\t{2}\t{3}\t{4}", type, vendor, Ver(api), name, why ?? "");
                if (type == 4) continue;   // VK_PHYSICAL_DEVICE_TYPE_CPU
                int score = type == 2 ? 4 : type == 1 ? 3 : 2;   // discrete, integrated, virtual / other
                string verdict = string.Format("{0}\t{1:x4}\t{2}", Ver(api), vendor, name);
                if (why == null && score > fitScore) { fitScore = score; fit = verdict; }
                if (why != null && score > rejScore) { rejScore = score; rejected = verdict + "\t" + why; }
            }
            vkDestroyInstance(inst, IntPtr.Zero);
            if (fit != null) { Console.WriteLine("result\tok\t" + fit); return 0; }
            if (rejected != null) { Console.WriteLine("result\tunfit\t" + rejected); return 1; }
            Console.WriteLine("result\tnodevice");
            return 4;
        }
        catch (Exception e) { Console.WriteLine("result\terror\t" + e.GetType().Name + ": " + e.Message); return 3; }
        finally { Marshal.FreeHGlobal(app); Marshal.FreeHGlobal(info); Marshal.FreeHGlobal(props); }
    }

    // A folder that would break the install although its drive is fine (null: none). OneDrive uploads what lands in it
    // (the build files, FirstRunGb) and turns files into online-only stubs; Windows Security's Controlled folder access blocks
    // unknown programs (our compilers) from writing into Documents, Desktop, Pictures, Videos and Music. built: a game
    // is already built there (an update): only what stops a build counts, not what the first install was allowed under.
    public static string FolderProblem(string f, bool built = false)
    {
        string full;
        try { full = Path.GetFullPath(f).TrimEnd('\\') + "\\"; } catch { return null; }
        if (!built && InOneDrive(full))
            return "This folder is inside OneDrive, which would upload the " + FirstRunGb + " GB of build files. Choose a folder outside OneDrive.";
        // Steam's Proton: C: is the entry's prefix (steamapps/compatdata/<n>/pfx), which Steam deletes with the entry
        if (!built && Wine.Proton && string.Equals(Path.GetPathRoot(full), @"C:\", StringComparison.OrdinalIgnoreCase))
            return "Under Proton drive C: is this Steam entry's own folder, which Steam deletes with the entry. Choose a folder on a Linux drive" +
                   (Wine.Home() != null ? ", for example " + Path.Combine(Wine.Home(), BuilderForm.FolderName) : " (drive Z:)") + ".";
        if (ControlledFolderAccess())
            foreach (var sf in new[] { Environment.SpecialFolder.MyDocuments, Environment.SpecialFolder.DesktopDirectory, Environment.SpecialFolder.MyPictures,
                                       Environment.SpecialFolder.MyVideos, Environment.SpecialFolder.MyMusic })
            {
                string p = Environment.GetFolderPath(sf);
                if (p.Length > 0 && full.StartsWith(p.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase))
                    return "Windows Security's Controlled folder access protects this folder and would block the build. Choose a folder outside Documents, Desktop, Pictures, Videos and Music.";
            }
        return Unwritable(full.TrimEnd('\\'));
    }

    // A folder this account may not create files in (Program Files, Windows, another user's folder: Setup needs no
    // administrator and asks for none), else .NET's "Access to the path is denied" mid-install. Tried in the nearest
    // folder that exists (a new folder and away again); anything but a refusal is left to the install itself.
    static string Unwritable(string dir)
    {
        string at = dir;
        while (at != null && !Directory.Exists(at)) at = Path.GetDirectoryName(at);
        if (at == null) return null;
        string probe = Path.Combine(at, ".setup-write-test-" + Guid.NewGuid().ToString("N").Substring(0, 8));
        try { Directory.CreateDirectory(probe); Directory.Delete(probe); return null; }
        catch (UnauthorizedAccessException)
        {
            return string.Format("Windows does not let this account create files in {0}. Choose a folder you can write in, for example C:\\Games.", at);
        }
        catch { return null; }
    }

    // A path in one of this account's OneDrive folders (builder.py's in_onedrive)
    public static bool InOneDrive(string path)
    {
        string full;
        try { full = Path.GetFullPath(path).TrimEnd('\\') + "\\"; } catch { return false; }
        foreach (var v in new[] { "OneDrive", "OneDriveConsumer", "OneDriveCommercial" })
        {
            string od = Environment.GetEnvironmentVariable(v);
            if (!string.IsNullOrEmpty(od) && full.StartsWith(od.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase)) return true;
        }
        return false;
    }

    // Controlled folder access in block mode (1; 2 = audit only), as set by the user or by a policy
    public static bool ControlledFolderAccess()
    {
        foreach (var key in new[] { @"SOFTWARE\Microsoft\Windows Defender\Windows Defender Exploit Guard\Controlled Folder Access",
                                    @"SOFTWARE\Policies\Microsoft\Windows Defender\Windows Defender Exploit Guard\Controlled Folder Access" })
            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(key))
                    if (k != null && Convert.ToInt32(k.GetValue("EnableControlledFolderAccess", 0)) == 1) return true;
            }
            catch { }
        return false;
    }
}

// The dump, the update and the keys, read in their headers before anything is downloaded (a second, not after the
// tools and step 0): pipeline/nx/container.py's program_sections / update_program up to the title key, and builder.py's
// check_keys and dump_problem sentences, which builder.py's check_dump says again before step 1 (test_preflight.py
// compares the two on synthetic dumps). Key values are only used, never written anywhere.
static class Inputs
{
    const ulong Title = 0x0100F2C0115B6000, UpdateTitle = Title | 0x800;   // (container.py TOTK_TITLE_ID, TOTK_UPDATE_TITLE_ID)
    const string KeysFix = "dump prod.keys again on the console the game was dumped from, with a current key dumper (one that " +
                           "knows that console's firmware), and choose that file";

    class Bad : Exception { public Bad(string why) : base(why) { } }
    static void Require(bool ok, string why) { if (!ok) throw new Bad(why); }

    struct Entry { public string Name; public long Offset, Size; }
    class Nca { public byte[] Hdr; public int Type; public ulong Title; public bool Bktr; }

    // null when the files open; else one sentence saying what to do (update: null without the update)
    public static string Check(string dump, string keys, string update)
    {
        string name = Path.GetFileName(keys);
        Dictionary<string, byte[]> keyset;
        try
        {
            var lines = File.ReadAllLines(keys);
            var names = new HashSet<string>(lines.Where(l => l.Contains('=')).Select(l => l.Split('=')[0].Trim().ToLowerInvariant()));
            if (!new[] { "header_key", "key_area_key_application_00", "titlekek_00" }.All(names.Contains))
                return name + " does not look like a complete keys file. Use the one from your own console.";
            keyset = LoadKeys(lines);
        }
        catch (Bad e) { return name + " cannot be read as a keys file (" + e.Message + "): " + KeysFix + "."; }
        catch (Exception e) { return name + " cannot be read (" + e.Message + "): " + keys; }
        foreach (var x in new[] { Tuple.Create(dump, "the dump", false), Tuple.Create(update, "the update dump", true) })
        {
            if (x.Item1 == null) continue;
            try
            {
                using (var f = new FileStream(x.Item1, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 4096, FileOptions.RandomAccess))
                    Open(f, keyset, x.Item3);
            }
            catch (Bad e) { return Problem(x.Item2, e.Message); }
            catch (Exception e) { return Cap(x.Item2) + " cannot be read (" + e.Message + "): " + x.Item1; }
        }
        return null;
    }

    static string Cap(string s) { return char.ToUpperInvariant(s[0]) + s.Substring(1); }

    // builder.py's dump_problem: the reason a dump / update could not be opened, as one sentence with what to do
    public static string Problem(string what, string msg)
    {
        if (msg.Contains("prod.keys") || msg.Contains("NCA3") || msg.Contains("titlekek") || msg.Contains("key_area_key"))
            return "Your prod.keys cannot open " + what + " (" + msg + "): " + KeysFix + ".";
        if (msg.Contains("FS header hash") || msg.Contains("NCA key area"))
            return Cap(what) + " could not be read correctly (" + msg + "): either your prod.keys is not the one from the console it was " +
                   "dumped on (" + KeysFix + "), or the file is damaged (copy or dump it again).";
        if (msg.Contains("Personalized"))
            return Cap(what) + " has a personalized ticket (tied to one console account), which Setup cannot use: dump the game again " +
                   "so that it carries a common ticket.";
        if (msg.ToLowerInvariant().Contains("ticket"))
            return Cap(what) + " has no usable ticket (" + msg + "): dump the game again with its ticket (a common ticket).";
        if (msg.Contains("Expected an XCI or NSP") || msg.Contains("Truncated") || msg.Contains("does not match") || msg.Contains("out of bounds"))
            return Cap(what) + " is not a complete .nsp / .xci file (" + msg + "): copy or dump it again (an interrupted copy or download " +
                   "leaves it short).";
        if (what == "the dump")
            return "The dump is not Tears of the Kingdom's base game, version 1.0.0 (title 0100F2C0115B6000): choose the base game's " +
                   ".nsp / .xci. The " + BuilderForm.UpdateVersion + " update goes in the Update row, and DLC is not used (" + msg + ").";
        return "The update dump is not Tears of the Kingdom's update: choose its own .nsp, or leave the Update row empty (" + msg + ").";
    }

    // container.py load_keys: name = hex value lines, '#' starts a comment
    static Dictionary<string, byte[]> LoadKeys(string[] lines)
    {
        var keys = new Dictionary<string, byte[]>();
        for (int i = 0; i < lines.Length; i++)
        {
            string line = lines[i].Split('#')[0].Trim();
            int eq = line.IndexOf('=');
            if (eq < 0) continue;
            string hex = new string(line.Substring(eq + 1).Where(c => !char.IsWhiteSpace(c)).ToArray());
            if (hex.Length % 2 != 0 || !hex.All(Uri.IsHexDigit)) throw new Bad("Invalid key encoding at line " + (i + 1));
            var v = new byte[hex.Length / 2];
            for (int j = 0; j < v.Length; j++) v[j] = Convert.ToByte(hex.Substring(2 * j, 2), 16);
            keys[line.Substring(0, eq).Trim().ToLowerInvariant()] = v;
        }
        return keys;
    }

    static void Open(Stream f, Dictionary<string, byte[]> keys, bool update)
    {
        var entries = Entries(f, f.Length);
        var tickets = Tickets(f, entries);
        var ncas = entries.Where(e => e.Name.EndsWith(".nca", StringComparison.OrdinalIgnoreCase)).Select(e => Header(f, e, keys)).ToList();
        var meta = ncas.Where(n => n.Type == 1).ToList();
        var programs = ncas.Where(n => n.Type == 0).ToList();
        Nca program;
        if (!update)   // (program_nca)
        {
            Require(meta.Count <= 1 && meta.All(n => n.Title == Title), "Expected base game metadata; updates, DLC and combined packages are unsupported");
            Require(programs.Count == 1, "Expected one base Program NCA; updates, DLC and combined packages are unsupported");
            program = programs[0];
            Require(program.Title == Title, string.Format("Expected base game {0:X16}; updates, DLC and other games are unsupported", Title));
            Require(!program.Bktr, "Expected a base game, not a BKTR update");
        }
        else   // (update_program: its Program NCA carries the base title's id or the update's)
        {
            Require(meta.Count == 1 && meta[0].Title == UpdateTitle,
                    string.Format("Expected the update {0:X16} (one metadata NCA); base games, DLC and combined packages go elsewhere", UpdateTitle));
            Require(programs.Count == 1 && (programs[0].Title == Title || programs[0].Title == UpdateTitle),
                    string.Format("Expected one Program NCA of {0:X16} / {1:X16}", Title, UpdateTitle));
            program = programs[0];
            Require(program.Bktr, "Expected an update (BKTR) Program NCA");
        }
        TitleKeyPresent(program.Hdr, keys, tickets);
        if (update) return;
        using (var sha = SHA256.Create())   // (program_sections' NcaSection: each used section's FS header hash)
            for (int i = 0; i < 4; i++)
                if (BitConverter.ToUInt32(program.Hdr, 0x240 + 16 * i) != 0)
                    Require(sha.ComputeHash(program.Hdr, 0x400 + 512 * i, 512).SequenceEqual(program.Hdr.Skip(0x280 + 32 * i).Take(32)),
                            "FS header hash mismatch");
    }

    static byte[] ReadAt(Stream f, long offset, long length)
    {
        Require(offset >= 0 && length >= 0 && length <= 512L << 20, "Invalid/big read");
        var b = new byte[length];
        f.Seek(offset, SeekOrigin.Begin);
        int got = 0, n;
        while (got < length && (n = f.Read(b, got, (int)length - got)) > 0) got += n;
        Require(got == length, "Truncated input");
        return b;
    }

    // container_entries: an NSP's files, or those of an XCI's secure partition
    static List<Entry> Entries(Stream f, long size)
    {
        if (Encoding.ASCII.GetString(ReadAt(f, 0, 4)) == "PFS0") return Partition(f, 0, size, "PFS0", 24);
        Require(size >= 0x138 && Encoding.ASCII.GetString(ReadAt(f, 0x100, 4)) == "HEAD", "Expected an XCI or NSP (PFS0) game file");
        ulong at = BitConverter.ToUInt64(ReadAt(f, 0x130, 8), 0);
        long root = at > (ulong)size ? size + 1 : (long)at;   // (past the end: the bound below is negative, as in Python)
        var secure = Partition(f, root, size - root, "HFS0", 64).Where(e => e.Name == "secure").ToList();
        Require(secure.Count == 1, "Expected one XCI secure partition");
        return Partition(f, secure[0].Offset, secure[0].Size, "HFS0", 64);
    }

    // partition_entries: a bounded PFS0 / HFS0 table, [(name, absolute offset, size)]
    static List<Entry> Partition(Stream f, long offset, long bound, string magic, int stride)
    {
        Require(bound >= 16, "Truncated partition header");
        var hdr = ReadAt(f, offset, 16);
        Require(Encoding.ASCII.GetString(hdr, 0, 4) == magic, "Expected " + magic);
        uint count = BitConverter.ToUInt32(hdr, 4), stringsSize = BitConverter.ToUInt32(hdr, 8);
        Require(count <= 4096 && stringsSize <= 1024 * 1024, "Invalid partition table");
        long size = 16 + count * stride + stringsSize;
        Require(size <= bound, "Partition table out of bounds");
        var raw = ReadAt(f, offset, size);
        int strings = 16 + (int)count * stride;
        var entries = new List<Entry>();
        var names = new HashSet<string>();
        for (int i = 0; i < count; i++)
        {
            ulong off = BitConverter.ToUInt64(raw, 16 + i * stride), length = BitConverter.ToUInt64(raw, 24 + i * stride);
            uint nameOff = BitConverter.ToUInt32(raw, 32 + i * stride);
            Require(nameOff < stringsSize, "Invalid partition filename offset");
            int end = Array.IndexOf(raw, (byte)0, strings + (int)nameOff);
            Require(end >= 0, "Unterminated partition filename");
            string name = Encoding.UTF8.GetString(raw, strings + (int)nameOff, end - strings - (int)nameOff);
            Require(name != "" && name != "." && name != ".." && name.IndexOfAny(new[] { '/', '\\' }) < 0, "Unsafe partition filename");
            Require(names.Add(name), "Duplicate partition filename");
            Require(off <= (ulong)bound && length <= (ulong)bound && (ulong)size + off + length <= (ulong)bound, "Partition entry out of bounds");
            entries.Add(new Entry { Name = name, Offset = offset + size + (long)off, Size = (long)length });
        }
        long last = offset + size;
        foreach (var e in entries.OrderBy(e => e.Offset))
        {
            Require(e.Offset >= last, "Overlapping partition entries");
            last = e.Offset + e.Size;
        }
        return entries;
    }

    // common_tickets: the rights ids of the container's common tickets (the title keys themselves are not needed here)
    static HashSet<string> Tickets(Stream f, List<Entry> entries)
    {
        var bodies = new Dictionary<uint, int> { { 0x10000, 0x240 }, { 0x10001, 0x140 }, { 0x10002, 0x80 },
                                                 { 0x10003, 0x240 }, { 0x10004, 0x140 }, { 0x10005, 0x80 } };
        var keys = new Dictionary<string, string>();
        foreach (var e in entries.Where(e => e.Name.EndsWith(".tik", StringComparison.OrdinalIgnoreCase)))
        {
            Require(e.Size >= 4, "Truncated NSP ticket");
            uint sig = BitConverter.ToUInt32(ReadAt(f, e.Offset, 4), 0);
            Require(bodies.ContainsKey(sig), "Unsupported NSP ticket signature type");
            Require(e.Size >= bodies[sig] + 0x180, "Truncated NSP ticket body");
            var data = ReadAt(f, e.Offset + bodies[sig], 0x180);
            Require(data[0x141] == 0, "Personalized NSP tickets are unsupported; a common ticket is required");
            string rights = BitConverter.ToString(data, 0x160, 16), key = BitConverter.ToString(data, 0x40, 16);
            Require(!keys.ContainsKey(rights) || keys[rights] == key, "Conflicting NSP tickets");
            keys[rights] = key;
        }
        return new HashSet<string>(keys.Keys);
    }

    // read_nca_header: the 0xC00-byte header, AES-XTS with header_key, sectors numbered big-endian (Nintendo's tweak)
    static Nca Header(Stream f, Entry e, Dictionary<string, byte[]> keys)
    {
        Require(e.Size >= 0xc00, "Truncated NCA header");
        Require(keys.ContainsKey("header_key"), "Missing header_key in prod.keys");
        Require(keys["header_key"].Length == 32, "header_key in prod.keys is not 32 bytes");
        var hdr = XtsDecrypt(keys["header_key"], ReadAt(f, e.Offset, 0xc00));
        Require(Encoding.ASCII.GetString(hdr, 0x200, 4) == "NCA3", "Expected NCA3 (check prod.keys and the game file)");
        Require(BitConverter.ToInt64(hdr, 0x208) == e.Size, "NCA size does not match container entry");
        for (int i = 0; i < 4; i++)
        {
            long start = BitConverter.ToUInt32(hdr, 0x240 + i * 16) * 512L, end = BitConverter.ToUInt32(hdr, 0x244 + i * 16) * 512L;
            Require((start == 0 && end == 0) || (0xc00 <= start && start < end && end <= e.Size), "NCA section out of bounds");
        }
        bool bktr = false;
        for (int i = 0; i < 4; i++) bktr |= hdr[0x400 + i * 512 + 4] == 4;   // (an FS header of crypt type 4: an update's patch)
        return new Nca { Hdr = hdr, Type = hdr[0x205], Title = BitConverter.ToUInt64(hdr, 0x210), Bktr = bktr };
    }

    // nca_ctr_key, short of decrypting: the key it needs is in prod.keys (and, rights-managed, its ticket in the file)
    static void TitleKeyPresent(byte[] hdr, Dictionary<string, byte[]> keys, HashSet<string> tickets)
    {
        int generation = Math.Max(Math.Max(hdr[0x206], hdr[0x220]) - 1, 0);
        if (hdr.Skip(0x230).Take(16).Any(b => b != 0))
        {
            Require(tickets.Contains(BitConverter.ToString(hdr, 0x230, 16)), "Missing common ticket for rights-managed NCA in the NSP");
            string name = string.Format("titlekek_{0:x2}", generation);
            Require(keys.ContainsKey(name), "Missing " + name + " in prod.keys");
            return;
        }
        Require(hdr[0x207] < 3, "Unsupported NCA key area");
        string area = string.Format("key_area_key_{0}_{1:x2}", new[] { "application", "ocean", "system" }[hdr[0x207]], generation);
        Require(keys.ContainsKey(area), "Missing " + area + " in prod.keys");
    }

    public static byte[] XtsDecrypt(byte[] key, byte[] data)
    {
        var plain = new byte[data.Length];
        using (var aes = Aes.Create())
        {
            aes.Mode = CipherMode.ECB; aes.Padding = PaddingMode.None;
            using (var dec = aes.CreateDecryptor(key.Take(16).ToArray(), new byte[16]))
            using (var enc = aes.CreateEncryptor(key.Skip(16).ToArray(), new byte[16]))
                for (int s = 0; s * 512 < data.Length; s++)
                {
                    byte[] sector = new byte[16], t = new byte[16], block = new byte[16], clear = new byte[16];
                    for (int i = 0; i < 8; i++) sector[15 - i] = (byte)((long)s >> (8 * i));
                    enc.TransformBlock(sector, 0, 16, t, 0);
                    for (int b = s * 512; b < (s + 1) * 512 && b + 16 <= data.Length; b += 16)
                    {
                        for (int i = 0; i < 16; i++) block[i] = (byte)(data[b + i] ^ t[i]);
                        dec.TransformBlock(block, 0, 16, clear, 0);
                        for (int i = 0; i < 16; i++) plain[b + i] = (byte)(clear[i] ^ t[i]);
                        int carry = t[15] >> 7;   // (the tweak times x in GF(2^128), little-endian)
                        for (int i = 15; i > 0; i--) t[i] = (byte)((t[i] << 1) | (t[i - 1] >> 7));
                        t[0] = (byte)((t[0] << 1) ^ (carry != 0 ? 0x87 : 0));
                    }
                }
        }
        return plain;
    }
}

// Uninstall (Setup --uninstall: Windows' Settings > Apps entry, or the window's Uninstall link). What Setup made goes:
// its folders in the install folder (tools, src, work, game, setup.log, .setup-root, its own copy Setup.exe), the
// ProgramData junction to it (the link only, never what it leads to), the desktop shortcuts that point into it, the
// game's caches and logs (%LOCALAPPDATA%\nxrecomp\TotK-engine) and Setup's registry keys. Anything else in the install
// folder (a dump or keys the player keeps there) stays, and so does the folder then; links inside are removed as links,
// never followed. The saves, settings and mods (%APPDATA%\nxrecomp\TotK-engine, and a game folder's mods\) go only when
// the player says so (asked separately, default No).
static class Uninstaller
{
    public static readonly string AppsKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\" + BuilderForm.FolderName;
    static readonly string[] Ours = { "tools", "src", "work", "game", "setup.log", ".setup-root", "Setup.exe" };

    public class Result { public long Freed; public string Refused; public List<string> Kept = new List<string>(); public List<string> Failed = new List<string>(); }

    // Setup's own install folder, proven before anything is deleted (a wrong InstallDir, or a Setup.exe copy run from a
    // home or Documents folder, must never take that folder's own src / work / game): its .setup-root names the folder
    // (Setup writes it at every install) AND one of Setup's stamps is there (a tool's .setup.json, a TotK-PC-* build).
    // null when it is, else why not
    public static string NotOurs(string root)
    {
        string mark;
        try { mark = File.ReadAllText(Path.Combine(root, ".setup-root")).Trim(); }
        catch { return root + " has no .setup-root (the mark Setup leaves in its install folder)"; }
        Func<string, string> norm = p => { try { return Path.GetFullPath(p).TrimEnd('\\'); } catch { return p; } };
        if (!string.Equals(norm(mark), norm(root), StringComparison.OrdinalIgnoreCase))
            return root + "'s .setup-root names another folder (" + mark + ")";
        bool stamp = false;
        try
        {
            string tools = Path.Combine(root, "tools"), game = Path.Combine(root, "game");
            stamp = (Directory.Exists(tools) && Directory.GetDirectories(tools).Any(d => File.Exists(Path.Combine(d, ".setup.json")))) ||
                    (Directory.Exists(game) && Directory.GetDirectories(game, "TotK-PC-*").Length > 0);
        }
        catch { }
        return stamp ? null : root + " holds none of Setup's tools or builds";
    }

    public static string JunctionName(string root)
    {
        using (var h = SHA256.Create())
            return "j" + BitConverter.ToString(h.ComputeHash(Encoding.UTF8.GetBytes(root.ToLowerInvariant()))).Replace("-", "").Substring(0, 10).ToLowerInvariant();
    }

    static bool IsLink(string p) { try { return (File.GetAttributes(p) & FileAttributes.ReparsePoint) != 0; } catch { return false; } }

    // a tree, bottom up, links removed as links; keep(p) true: p stays (and so do its parents)
    static bool Delete(string p, Result r, Func<string, bool> keep)
    {
        if (keep != null && keep(p)) { r.Kept.Add(p); return false; }
        try
        {
            if (IsLink(p))
            {
                if (Directory.Exists(p)) Directory.Delete(p); else File.Delete(p);   // (a junction or symlink: the link alone)
                return true;
            }
            if (File.Exists(p)) { File.SetAttributes(p, FileAttributes.Normal); File.Delete(p); return true; }   // (git's read-only objects)
            if (!Directory.Exists(p)) return true;
            bool all = true;
            foreach (var e in Directory.GetFileSystemEntries(p)) all &= Delete(e, r, keep);
            if (!all) return false;
            File.SetAttributes(p, FileAttributes.Normal); Directory.Delete(p);
            return true;
        }
        catch (Exception e) { r.Failed.Add(p + " (" + e.Message + ")"); return false; }
    }

    static long Free(IEnumerable<string> paths)
    {
        long n = 0;
        foreach (var d in paths.Where(p => !string.IsNullOrEmpty(p)).Select(p => Path.GetPathRoot(Path.GetFullPath(p)).ToUpperInvariant()).Distinct())
            try { n += new DriveInfo(d).AvailableFreeSpace; } catch { }
        return n;
    }

    // The desktop shortcuts named after the game whose target is in the install folder (or reached through its junction)
    public static IEnumerable<string> Shortcuts(string desktop, string product, IEnumerable<string> roots, Func<string, string> target)
    {
        if (desktop == null || !Directory.Exists(desktop)) yield break;
        var inside = roots.Select(x => x.TrimEnd('\\') + "\\").ToList();
        foreach (var lnk in Directory.GetFiles(desktop, product + "*.lnk"))
        {
            string t = target(lnk);
            if (t != null && inside.Any(i => t.StartsWith(i, StringComparison.OrdinalIgnoreCase))) yield return lnk;
        }
    }

    public static string ShortcutTarget(string lnk) { return Win32.ShortcutTarget(lnk); }

    // root: the install folder; linkParents: where Setup puts its junctions (ProgramData\<name>, C:\<name>); local /
    // roaming: the game's %LOCALAPPDATA% / %APPDATA% folders (roaming null: the saves, settings and mods stay)
    public static Result Run(string root, IEnumerable<string> linkParents, string desktop, string product, string local, string roaming,
                             Func<string, string> target, string keepFile = null)
    {
        var r = new Result();
        root = Path.GetFullPath(root).TrimEnd('\\');
        r.Refused = NotOurs(root);
        if (r.Refused != null) return r;   // (nothing deleted)
        var drives = new List<string> { root, local, roaming };
        long before = Free(drives);
        var parents = (linkParents ?? new string[0]).ToList();
        var roots = new List<string> { root };
        roots.AddRange(parents.Select(x => Path.Combine(x, JunctionName(root))));
        foreach (var lnk in Shortcuts(desktop, product, roots, target).ToList()) Delete(lnk, r, null);
        foreach (var parent in parents)
        {
            string link = Path.Combine(parent, JunctionName(root));
            if (Directory.Exists(link) && IsLink(link)) { try { Directory.Delete(link); } catch (Exception e) { r.Failed.Add(link + " (" + e.Message + ")"); } }
            try { if (Directory.Exists(parent) && !Directory.EnumerateFileSystemEntries(parent).Any()) Directory.Delete(parent); } catch { }
        }
        // a game folder's mods\ (the player's, beside the launcher) stays with the saves
        Func<string, bool> keepMods = p => roaming == null && Path.GetFileName(p).Equals("mods", StringComparison.OrdinalIgnoreCase) &&
            Directory.Exists(p) && Directory.EnumerateFileSystemEntries(p).Any() &&
            Path.GetFileName(Path.GetDirectoryName(Path.GetDirectoryName(p)) ?? "").Equals("game", StringComparison.OrdinalIgnoreCase);
        // (keepFile: the running Setup.exe when it is the install folder's own copy: it cannot delete itself)
        Func<string, bool> keep = p => (keepFile != null && string.Equals(Path.GetFullPath(p), keepFile, StringComparison.OrdinalIgnoreCase)) || keepMods(p);
        foreach (var name in Ours) Delete(Path.Combine(root, name), r, keep);
        if (Directory.Exists(root))
        {
            var left = Directory.GetFileSystemEntries(root);
            if (left.Length == 0) Delete(root, r, null);
            else r.Kept.AddRange(left.Where(p => !r.Kept.Any(k => k.StartsWith(p, StringComparison.OrdinalIgnoreCase))));
        }
        foreach (var d in new[] { local, roaming }.Where(d => d != null))
        {
            Delete(d, r, null);
            string parent = Path.GetDirectoryName(d);   // (%...%\nxrecomp, when nothing else lives there)
            try { if (Directory.Exists(parent) && !Directory.EnumerateFileSystemEntries(parent).Any()) Directory.Delete(parent); } catch { }
        }
        r.Freed = Math.Max(0, Free(drives) - before);
        return r;
    }

    public static void Register(string root, string setupCopy, string launcher)
    {
        try
        {
            using (var k = Registry.CurrentUser.CreateSubKey(AppsKey))
            {
                k.SetValue("DisplayName", Release.Product); k.SetValue("DisplayVersion", Release.Tag);
                k.SetValue("Publisher", Release.Product); k.SetValue("InstallLocation", root);
                if (launcher != null) k.SetValue("DisplayIcon", launcher);
                k.SetValue("UninstallString", "\"" + setupCopy + "\" --uninstall");
                k.SetValue("NoModify", 1, RegistryValueKind.DWord); k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            }
        }
        catch { }
    }
}

// The Windows calls Setup used to reach through system tools (cmd mklink, taskkill, explorer.exe, WScript.Shell): the
// same results from the API directly. A small unsigned exe that starts those tools from strings is what antivirus
// heuristics weigh as a downloader / dropper; the behaviour is the same.
static class Win32
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, byte[] inBuf, int inSize, IntPtr outBuf, int outSize, out int returned, IntPtr overlapped);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    // A junction (mount point) at `link` to `target`, as `mklink /J`: no administrator needed. link must not exist.
    public static bool Junction(string link, string target)
    {
        string full = Path.GetFullPath(target).TrimEnd('\\');
        if (full.Length == 2 && full[1] == ':') full += "\\";
        byte[] sub = Encoding.Unicode.GetBytes(@"\??\" + full), print = Encoding.Unicode.GetBytes(full);
        // REPARSE_DATA_BUFFER, MountPointReparseBuffer: tag, data length, reserved; the two names' offsets and lengths;
        // then "\??\<target>" NUL "<target>" NUL
        int data = 8 + sub.Length + 2 + print.Length + 2;
        var buf = new byte[8 + data];
        BitConverter.GetBytes(0xA0000003u).CopyTo(buf, 0);                         // IO_REPARSE_TAG_MOUNT_POINT
        BitConverter.GetBytes((ushort)data).CopyTo(buf, 4);
        BitConverter.GetBytes((ushort)0).CopyTo(buf, 8);                           // substitute name: offset
        BitConverter.GetBytes((ushort)sub.Length).CopyTo(buf, 10);
        BitConverter.GetBytes((ushort)(sub.Length + 2)).CopyTo(buf, 12);          // print name: offset
        BitConverter.GetBytes((ushort)print.Length).CopyTo(buf, 14);
        sub.CopyTo(buf, 16);
        print.CopyTo(buf, 16 + sub.Length + 2);
        Directory.CreateDirectory(link);
        IntPtr h = CreateFileW(link, 0x40000000, 0, IntPtr.Zero, 3, 0x02000000 | 0x00200000, IntPtr.Zero);   // GENERIC_WRITE, OPEN_EXISTING, BACKUP_SEMANTICS | OPEN_REPARSE_POINT
        bool ok = false;
        if (h != new IntPtr(-1))
        {
            int got;
            ok = DeviceIoControl(h, 0x000900A4, buf, buf.Length, IntPtr.Zero, 0, out got, IntPtr.Zero);   // FSCTL_SET_REPARSE_POINT
            CloseHandle(h);
        }
        if (!ok) try { Directory.Delete(link); } catch { }   // (the empty folder made above)
        return ok;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct ProcessEntry
    {
        public uint Size, Usage, Pid; public IntPtr HeapId; public uint ModuleId, Threads, ParentPid; public int PriClassBase; public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string ExeFile;
    }
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32FirstW(IntPtr snap, ref ProcessEntry e);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32NextW(IntPtr snap, ref ProcessEntry e);

    // A process and everything below it, ended as `taskkill /T /F /PID` does (the tree as one snapshot sees it)
    public static void KillTree(int pid)
    {
        var kids = new Dictionary<int, List<int>>();
        IntPtr snap = CreateToolhelp32Snapshot(2, 0);   // TH32CS_SNAPPROCESS
        if (snap != new IntPtr(-1))
        {
            var e = new ProcessEntry { Size = (uint)Marshal.SizeOf(typeof(ProcessEntry)) };
            for (bool more = Process32FirstW(snap, ref e); more; more = Process32NextW(snap, ref e))
            {
                List<int> l;
                if (!kids.TryGetValue((int)e.ParentPid, out l)) kids[(int)e.ParentPid] = l = new List<int>();
                if ((int)e.Pid != (int)e.ParentPid) l.Add((int)e.Pid);
            }
            CloseHandle(snap);
        }
        // the parent first: once it is gone it starts no new step, and the snapshot already holds the children's ids
        // (still theirs: a child outlives its parent's end)
        var order = new List<int>();
        var seen = new HashSet<int>();
        Action<int> walk = null;
        walk = p => { if (!seen.Add(p)) return; order.Add(p); List<int> l; if (kids.TryGetValue(p, out l)) foreach (var c in l) walk(c); };
        walk(pid);
        foreach (var p in order)
            try { using (var x = Process.GetProcessById(p)) x.Kill(); } catch { }   // (gone already, or not ours to end)
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct StartupInfo
    {
        public int Cb; public string Reserved, Desktop, Title; public int X, Y, XSize, YSize, XChars, YChars, Fill, Flags;
        public short ShowWindow, Reserved2; public IntPtr Reserved3, StdIn, StdOut, StdErr;
    }
    [StructLayout(LayoutKind.Sequential)] struct ProcessInfo { public IntPtr Process, Thread; public int Pid, Tid; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env,
                                      string dir, ref StartupInfo si, out ProcessInfo pi);

    // `exe` started outside this process's job (CREATE_BREAKAWAY_FROM_JOB: Setup's job allows it, Main), so closing
    // Setup leaves it running, as a start through Explorer did. Its process id; 0: no breakaway here (a job above ours
    // forbids it)
    public static int StartOutsideJob(string exe, string args)
    {
        var si = new StartupInfo { Cb = Marshal.SizeOf(typeof(StartupInfo)) };
        ProcessInfo pi;
        var cmd = new StringBuilder("\"" + exe + "\"" + (string.IsNullOrEmpty(args) ? "" : " " + args));
        if (!CreateProcessW(null, cmd, IntPtr.Zero, IntPtr.Zero, false, 0x01000000, IntPtr.Zero, Path.GetDirectoryName(exe), ref si, out pi))
            return 0;
        CloseHandle(pi.Thread); CloseHandle(pi.Process);
        return pi.Pid;
    }

    [ComImport, Guid("00021401-0000-0000-C000-000000000046")] class ShellLinkObject { }
    [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("000214F9-0000-0000-C000-000000000046")]
    interface IShellLinkW
    {
        void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder file, int max, IntPtr findData, uint flags);
        void GetIDList(out IntPtr pidl);
        void SetIDList(IntPtr pidl);
        void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder name, int max);
        void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string name);
        void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder dir, int max);
        void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string dir);
        void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder args, int max);
        void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string args);
        void GetHotkey(out short key);
        void SetHotkey(short key);
        void GetShowCmd(out int cmd);
        void SetShowCmd(int cmd);
        void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder path, int max, out int index);
        void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string path, int index);
        void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string rel, uint reserved);
        void Resolve(IntPtr hwnd, uint flags);
        void SetPath([MarshalAs(UnmanagedType.LPWStr)] string file);
    }

    // A .lnk at `lnk` that starts `target` in `dir` with its icon (IShellLink, as WScript.Shell's CreateShortcut did)
    public static void SaveShortcut(string lnk, string target, string dir, string args = null)
    {
        var sl = (IShellLinkW)new ShellLinkObject();
        try
        {
            sl.SetPath(target);
            sl.SetWorkingDirectory(dir);
            if (args != null) sl.SetArguments(args);
            sl.SetIconLocation(target, 0);
            ((System.Runtime.InteropServices.ComTypes.IPersistFile)sl).Save(lnk, true);
        }
        finally { Marshal.FinalReleaseComObject(sl); }
    }

    // What a .lnk starts (its target path), or null
    public static string ShortcutTarget(string lnk)
    {
        var sl = (IShellLinkW)new ShellLinkObject();
        try
        {
            ((System.Runtime.InteropServices.ComTypes.IPersistFile)sl).Load(lnk, 0);
            var sb = new StringBuilder(1024);
            sl.GetPath(sb, sb.Capacity, IntPtr.Zero, 0);
            return sb.Length > 0 ? sb.ToString() : null;
        }
        catch { return null; }
        finally { Marshal.FinalReleaseComObject(sl); }
    }
}

class BuilderForm : Form
{
    readonly Row dump, keys, update, firmware, install;
    readonly Row[] rows;
    readonly PrimaryButton build;
    readonly LinkLabel uninstall;
    readonly StatusView status;
    readonly string repo;          // Builder mode: the clone this exe sits in; null in Setup mode
    Process proc;
    Thread worker;
    volatile bool stopping;
    string launcher;               // the game's launcher, once a build is done
    string source;                 // --source=: the repository Setup fetches from instead of GitHub's
    bool acceptTerms, auto, hidden, noShortcut, noPreflight, scanNote;
    string toolsDir;               // <install>\tools once a run set it up (a failed run checks what an antivirus took)
    string builderArgs = "";
    StreamWriter log;              // <install>\setup.log: every line the window shows (for a bug report)

    protected override void SetVisibleCore(bool value) { base.SetVisibleCore(hidden ? false : value); }

    // --auto: press Install once the window runs (and, --hidden, end with the build)
    void AutoStart()
    {
        if (!auto) return;
        if (build.Enabled) Start();
        else if (hidden) { Console.Error.WriteLine("Setup: inputs missing"); Environment.Exit(2); }
    }
    static readonly Regex StepLine = new Regex(@"step (\d+)/(\d+) (\w+)");
    static readonly Regex Percent = new Regex(@": (\d+)% \(");
    static readonly Regex GitPercent = new Regex(@"^(Receiving objects|Resolving deltas|Updating files):\s+(\d+)%");
    const string RegKey = @"Software\" + Release.Product;
    // How long each step takes, roughly (minutes on a first run): the bar moves by these, not by step count
    static readonly Dictionary<string, float> Weight = new Dictionary<string, float> {
        { "tools", 8 }, { "extract", 6 }, { "romfs", 3 }, { "images", 1 }, { "patches", 3 }, { "build", 35 },
        { "sets", 4 }, { "overlay", 2 }, { "package", 1 }, { "shaders", 5 } };

    [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int value, int size);

    OptionRow copy;
    string copyWhy;   // the line under "Copy the game files" when the dump's drive chose it

    // The switch follows the install and the dump until the player chooses (then it is remembered: CopyGameFiles): an
    // install that has the copy already keeps it (v0.1.4-v0.1.7 said the install stands alone; many deleted the dump),
    // else copied when the dump is on a drive that can go away. An update build always copies (its data is the update
    // merged over the game's)
    void CopyDefault()
    {
        if (copy == null) return;
        copy.Locked = update.FilePath != null || Busy;
        if (update.FilePath != null) { copy.Set(true, false); return; }
        if (copy.Touched) { copy.Invalidate(); return; }
        bool had = install != null && install.FilePath != null &&
                   File.Exists(Path.Combine(install.FilePath, "work", "extracted", "v100", "romfs.bin"));
        string kind = had ? null : DumpDriveCanGo(dump.FilePath);
        copyWhy = had ? "Kept from your earlier install, which runs without the dump"
                : kind == null ? null
                : kind == "OneDrive" ? "Your dump is in OneDrive, which can turn it into an online-only file, so they are copied"
                : "Your dump is on a " + kind + " drive, which can be unplugged, so they are copied";
        copy.Set(had || kind != null, false);
    }

    // The kind of drive a dump is on when that drive can go away (removable, USB, network, disc, or OneDrive, which
    // frees space by leaving an online-only stub of a file), else null; the same test as builder.py's dump_drive
    public static string DumpDriveCanGo(string path)
    {
        try
        {
            if (path == null) return null;
            if (path.StartsWith(@"\\")) return "network";
            if (Preflight.InOneDrive(path)) return "OneDrive";
            string root = Path.GetPathRoot(Path.GetFullPath(path));
            var t = new DriveInfo(root).DriveType;
            if (t == DriveType.Removable) return "removable";
            if (t == DriveType.Network) return "network";
            if (t == DriveType.CDRom) return "disc";
            return t == DriveType.Fixed && UsbBus(root.TrimEnd('\\')) ? "USB" : null;
        }
        catch { return null; }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern Microsoft.Win32.SafeHandles.SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(Microsoft.Win32.SafeHandles.SafeFileHandle h, uint code, byte[] inBuf, int inSize, byte[] outBuf, int outSize, out uint got, IntPtr overlapped);

    // an external disk on USB reports itself fixed: the storage bus says what it is (IOCTL_STORAGE_QUERY_PROPERTY, no
    // admin rights needed)
    static bool UsbBus(string drive)
    {
        using (var h = CreateFile(@"\\.\" + drive, 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero))
        {
            if (h.IsInvalid) return false;
            var query = new byte[12]; var desc = new byte[1024]; uint got;
            if (!DeviceIoControl(h, 0x2D1400, query, query.Length, desc, desc.Length, out got, IntPtr.Zero) || got < 32) return false;
            uint bus = BitConverter.ToUInt32(desc, 28);   // STORAGE_DEVICE_DESCRIPTOR.BusType
            return bus == 7 || bus == 0xC || bus == 0xD;   // USB, SD, MMC
        }
    }

    string CopyArg() { return copy.Checked ? " --copy-romfs" : " --no-copy-romfs"; }

    BuilderForm()
    {
        repo = FindRepo();
        Text = repo != null ? "Builder" : Release.Product + " Setup";
        DoubleBuffered = true; AutoScaleMode = AutoScaleMode.None;   // (sizes are already scaled by T.D)
        FormBorderStyle = FormBorderStyle.FixedSingle; MaximizeBox = false;
        BackColor = T.Bg; ForeColor = T.Text; Font = T.Body;
        int W = T.D(560), pad = T.S8, inner = W - 2 * pad;
        StartPosition = FormStartPosition.CenterScreen;

        var title = new Label { Text = repo != null ? "Builder" : Release.Product, Font = T.Display, ForeColor = T.Text, AutoSize = true, Location = new Point(pad - T.D(2), T.S6) };
        var sub = new Label { Text = "Turns a dump of your own console into a native PC build.", Font = T.Body, ForeColor = T.TextDim,
                              AutoSize = true, Location = new Point(pad, T.S6 + T.D(36)) };

        dump = new Row(1, "Dump", "Drop it here or click to choose", f => IsDump(f));
        keys = new Row(2, "Keys", "The keys file from the same console", f => File.Exists(f) &&
            f.EndsWith(".keys", StringComparison.OrdinalIgnoreCase) && !f.EndsWith("title.keys", StringComparison.OrdinalIgnoreCase));
        // (the game's 1.4.3 update dump: builds that version, next to a 1.0.0 one; without it, 1.0.0 as before)
        update = new Row(3, "Update (optional)", "Your " + UpdateVersion + " update dump: builds version " + UpdateVersion,
            f => IsDump(f)) { Clearable = true, Describe = f => Path.GetFileName(f) + "   (right-click to remove: builds 1.0.0)" };
        firmware = new Row(ShowUpdate ? 4 : 3, "Firmware (optional)",
            CjkWindows ? "Recommended: Korean and Chinese text looks right with its fonts" : "Only Korean and Chinese text needs it: its folder or .zip",
            f => Directory.Exists(f) ||
            (File.Exists(f) && f.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)))
            // (optional, and remembered from the last run: a firmware the keys cannot open must be removable, #26)
            { Clearable = true, Describe = f => Path.GetFileName(f) + "   (right-click to remove)" };
        install = new Row(ShowUpdate ? 5 : 4, "Install to", "A folder with about " + Preflight.FirstInstallGb(true, false) + " GB free", f => IsFolderTarget(f)) { FolderOnly = true, Describe = DescribeFolder };
        rows = (repo != null ? new[] { dump, keys, update, firmware } : new[] { dump, keys, update, firmware, install })
            .Where(r => ShowUpdate || r != update).ToArray();
        int rowY = T.D(104), rowH = T.D(62);
        var top = new Label { BackColor = T.Rule, Bounds = new Rectangle(pad, rowY - 1, inner, 1) };   // rule above the list
        for (int i = 0; i < rows.Length; i++) { rows[i].Bounds = new Rectangle(pad, rowY + i * rowH, inner, rowH); rows[i].Changed += UpdateState; }
        dump.DropMany = keys.DropMany = update.DropMany = firmware.DropMany = Distribute;
        update.Changed += () =>
        {
            if (update.FilePath == null || Busy) return;
            status.Add(SavesNote);
            status.Add(string.Format("With the {0} update the first build needs about {1} GB free (its game files are unpacked).",
                                     UpdateVersion, Preflight.FirstInstallGb(repo == null, true)));
        };
        // the game's files: read from the dump in place (no 17 GB copy), or copied out so the dump can go; by default
        // copied only when the dump is on a drive that can go away (builder.py dump_drive), an update's always
        copy = new OptionRow("Copy the game files (17 GB)", on => update.FilePath != null ? "Version " + UpdateVersion + " always copies them"
            : on ? (copyWhy ?? "Uses 17 GB more; afterwards the dump can be moved or deleted")
            : "The game reads them from your dump: keep it where it is")
        { Bounds = new Rectangle(pad, rowY + rows.Length * rowH, inner, rowH) };
        dump.Changed += CopyDefault;
        update.Changed += CopyDefault;
        install.Changed += CopyDefault;

        build = new PrimaryButton { Text = repo != null ? "Build" : "Install", Bounds = new Rectangle(pad, copy.Bottom + T.S6, inner, T.D(42)), Enabled = false };
        build.Click += (s, e) => { if (!build.Enabled) return; if (launcher != null && proc == null && worker == null) Play(); else Start(); };
        status = new StatusView { Bounds = new Rectangle(pad, build.Bottom + T.S6, inner, T.D(28) + T.S3 + 5 * (T.Log.Height + T.D(2))) };

        var rule = new Label { BackColor = T.Rule, Bounds = new Rectangle(pad, status.Bottom + T.S4, inner, 1) };
        var notice = new Label
        {
            Text = "Only use files you dumped yourself from a console you own. This tool includes no game content " +
                   "and does not support piracy or files obtained any other way.",
            Font = T.Small, ForeColor = T.TextFaint, Bounds = new Rectangle(pad, rule.Bottom + T.S3, inner, T.D(36)),
        };
        ClientSize = new Size(W, notice.Bottom + T.S5);
        Controls.AddRange(new Control[] { title, sub, top, build, status, rule, notice });
        // Uninstall: a faint secondary link on the title line, only when an install exists, hidden while building
        string installed = InstalledDir();
        if (repo == null && installed != null)
        {
            uninstall = new LinkLabel { Text = "Uninstall", Font = T.Small, AutoSize = true, BackColor = T.Bg, LinkColor = T.TextFaint,
                                        ActiveLinkColor = T.TextDim, VisitedLinkColor = T.TextFaint, LinkBehavior = LinkBehavior.HoverUnderline, TabStop = true };
            uninstall.Location = new Point(W - pad - uninstall.PreferredWidth, title.Top + (title.PreferredHeight - uninstall.PreferredHeight) / 2);
            uninstall.MouseEnter += (s, e) => uninstall.LinkColor = T.TextDim;
            uninstall.MouseLeave += (s, e) => uninstall.LinkColor = T.TextFaint;
            uninstall.GotFocus += (s, e) => { uninstall.LinkColor = T.Accent; uninstall.LinkBehavior = LinkBehavior.AlwaysUnderline; };
            uninstall.LostFocus += (s, e) => { uninstall.LinkColor = T.TextFaint; uninstall.LinkBehavior = LinkBehavior.HoverUnderline; };
            uninstall.LinkClicked += (s, e) =>
            {
                if (Busy) return;
                if (log != null) { log.Dispose(); log = null; }   // (setup.log is in the install folder: never held open while it goes)
                if (UninstallMain(new[] { "--uninstall", "--install-to=" + installed }) != 2) Close();   // (in this process: no helper copy or child)
            };
            Controls.Add(uninstall);
        }
        Controls.AddRange(rows);
        Controls.Add(copy);

        AllowDrop = true;   // dropping all three anywhere on the window fills the rows
        DragEnter += (s, e) => e.Effect = !Busy && e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None;
        DragDrop += (s, e) => Distribute((string[])e.Data.GetData(DataFormats.FileDrop));
        FormClosing += (s, e) =>
        {
            if (Busy && MessageBox.Show("A build is running. Stop it and close? Pressing Install again later continues where it stopped.",
                                        Text, MessageBoxButtons.YesNo) == DialogResult.No) e.Cancel = true;
            else
            {
                closing = true; Stop();   // (closing: late events from the build are dropped, see Post)
                if (log != null) { try { log.WriteLine("Stopped: the window was closed"); log.Dispose(); } catch { } log = null; }
            }
        };
        status.Add(string.Format("The first build takes 30 to 60 minutes and needs about {0} GB free.", Preflight.FirstInstallGb(repo == null, false)));   // the empty state says what to expect
        status.Add("Later builds and updates reuse what is already done and take minutes.");
        if (repo == null) { RestoreChoices(); CheckNewer(); }
        // Files dropped onto the exe's icon, and options for an unattended run (tests, scripted installs):
        //   --install-to=DIR  --accept-terms  --auto (press Install)  --hidden (no window; exits 0 / 1 at the end)  --no-shortcut
        //   --source=<git url or path>: fetch the tag from another repository (testing a Setup before its release)
        //   --no-preflight: skip the checks that run before any download (processor, memory, disk, graphics driver, the
        //     install folder, the dump / update / keys headers)
        //   --play: start the installed game instead (Main, PlayInstalled); with nothing installed yet, this window
        var files = new List<string>();
        string to = null;
        var argv = Environment.GetCommandLineArgs().Skip(1).ToList();
        int dd = argv.IndexOf("--");   // what follows "--" goes to builder.py as is (e.g. -- --until romfs)
        if (dd >= 0) { builderArgs = string.Concat(argv.Skip(dd + 1).Select(x => " \"" + x + "\"")); argv = argv.Take(dd).ToList(); }
        foreach (var a in argv)
            if (a.StartsWith("--source=")) source = a.Substring("--source=".Length);
            else if (a.StartsWith("--install-to=")) to = a.Substring("--install-to=".Length);
            else if (a == "--accept-terms") acceptTerms = true;
            else if (a == "--auto") auto = true;
            else if (a == "--hidden") hidden = true;
            else if (a == "--no-shortcut") noShortcut = true;
            else if (a == "--no-preflight") noPreflight = true;   // (support: a PC check that is wrong about this PC)
            else if (a == "--play") { }   // (nothing installed yet: Main showed this window instead)
            else files.Add(a);
        Distribute(files.ToArray());
        if (to != null && repo == null) install.Set(Path.GetFullPath(to));
        if (hidden) CreateHandle();   // (BeginInvoke needs it; the window itself never shows)
        if (auto)
        {
            var t = new System.Windows.Forms.Timer { Interval = 200 };   // (once the message loop runs)
            t.Tick += (s, e) => { t.Stop(); AutoStart(); };
            t.Start();
        }
        else if (repo == null && !noPreflight) EarlyCheck();
        UpdateState();
    }

    // What the PC lacks, shown as the window opens (before the three files are even chosen); Install checks again
    void EarlyCheck()
    {
        new Thread(() =>
        {
            var problems = new List<string>();
            try { Preflight.Check(null, problems, new List<string>()); } catch { return; }   // (the disk: DiskRecheck)
            if (problems.Count > 0) Post(() => { if (!Busy) foreach (var p in problems) status.Add(p); });
        }) { IsBackground = true }.Start();
        DiskRecheck();
        install.Changed += DiskRecheck; update.Changed += DiskRecheck; copy.Changed += DiskRecheck;
    }

    // The free-space sentence for the folder, the update and the copy switch as they are now, in place of the last one
    // (a switch flipped to copy needs 17 GB more; flipped back, the sentence goes)
    string diskLine;
    void DiskRecheck()
    {
        if (Busy || install.FilePath == null) return;
        string now = Preflight.DiskProblem(install.FilePath, update.FilePath != null, copy.Checked);
        if (now == diskLine) return;
        if (diskLine != null) status.Remove(diskLine);
        diskLine = now;
        if (now != null) status.Add(now);
    }

    bool Busy { get { return proc != null || worker != null; } }

    protected override CreateParams CreateParams
    {
        get { var cp = base.CreateParams; cp.ExStyle |= 0x02000000; return cp; }   // WS_EX_COMPOSITED: no child flicker
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        int on = 1;   // dark title bar (Windows 10 20H1+: attribute 20; older builds: 19)
        if (DwmSetWindowAttribute(Handle, 20, ref on, 4) != 0) DwmSetWindowAttribute(Handle, 19, ref on, 4);
    }

    // Every dropped file to the row that takes it: of two dumps the bigger is the game, the other its update (a file a
    // row already holds, e.g. dropped onto the Update row itself, stays there)
    void Distribute(string[] files)
    {
        if (Busy) return;
        foreach (var f in files.OrderByDescending(f => IsDump(f) ? new FileInfo(f).Length : 0))
            if (!rows.Any(r => r.FilePath == f) && !dump.Offer(f) && !keys.Offer(f) && !(ShowUpdate && IsDump(f) && update.Offer(f)))
                firmware.Offer(f);
    }

    public const string UpdateVersion = "1.4.3";   // (builder.py's UPDATE_VERSION)
    // v0.1.8 ships without the 1.4.3 row: the row is not shown, a dropped or remembered update dump is not taken;
    // builder.py's --update stays (Setup... -- --update <nsp>)
    static readonly bool ShowUpdate = false;
    const string SavesNote = UpdateVersion + " keeps its own saves: the first time it starts it copies your 1.0.0 saves once. " +
        "Progress you make later in 1.0.0 does not appear in " + UpdateVersion + ", and " + UpdateVersion + " saves never go back to 1.0.0.";
    static bool IsDump(string f)
    {
        return File.Exists(f) && (f.EndsWith(".nsp", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".xci", StringComparison.OrdinalIgnoreCase));
    }

    // Windows' display language is Korean or Chinese: the game follows it, and without the firmware's shared fonts it
    // shows that text in a Windows font instead (hle_pl)
    // (the game's own setting first: settings.ini game.language, then Windows' display language, as the runtime picks it)
    static readonly bool CjkWindows = IsCjk(GameLanguage() ?? System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName);
    static bool IsCjk(string lang) { return lang.StartsWith("ko") || lang.StartsWith("zh"); }

    static string GameLanguage()
    {
        try
        {
            string ini = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "nxrecomp", "TotK-engine", "settings.ini");
            foreach (var ln in File.ReadAllLines(ini))
            {
                var kv = ln.Split('#')[0].Split(new[] { '=' }, 2);
                if (kv.Length == 2 && kv[0].Trim() == "game.language" && kv[1].Trim().Length > 0) return kv[1].Trim().ToLowerInvariant();
            }
        }
        catch { }
        return null;
    }

    void UpdateState()
    {
        // the firmware is optional: its shared fonts are only for Korean and Chinese text (a Windows font without them);
        // the update too: without it the build is 1.0.0
        int n = 0, need = rows.Count(r => r != firmware && r != update);   // (the update row may be hidden: ShowUpdate)
        foreach (var r in rows) if (r != firmware && r != update && r.FilePath != null) n++;
        build.Enabled = n == need && !Busy;
        if (launcher != null && !Busy) return;   // (Done: the button plays)
        if (!Busy && !status.Failed)
        {
            status.Step = n < need ? string.Format("{0} of {1} ready", n, need)
                        : firmware.FilePath != null ? "Ready"
                        : CjkWindows ? "Ready: without the firmware, Korean and Chinese text uses a Windows font"
                        : "Ready";
            status.Value = 0; status.Invalidate();
        }
    }

    static string FindRepo()
    {
        for (var d = AppDomain.CurrentDomain.BaseDirectory; d != null; d = Path.GetDirectoryName(d.TrimEnd('\\')))
            if (File.Exists(Path.Combine(d, "tools", "builder", "builder.py"))) return d;
        return null;
    }

    // ---- the install folder ------------------------------------------------------------------------------------------

    static bool IsFolderTarget(string f) { return FolderProblem(f) == null; }

    // An install Setup already set up there (an update reuses it where it is): by Setup's own files, never a folder name
    // a player's drive may hold anyway (a D:\tools of their own): its .setup-root naming this folder, or the LLVM stamp
    // NeedGb trusts (an install from before .setup-root)
    public static bool Built(string dir)
    {
        try
        {
            string full = Path.GetFullPath(dir).TrimEnd('\\');
            string mark = Path.Combine(full, ".setup-root");
            return (File.Exists(mark) && string.Equals(Path.GetFullPath(File.ReadAllText(mark).Trim()).TrimEnd('\\'), full, StringComparison.OrdinalIgnoreCase))
                || File.Exists(Path.Combine(full, "tools", "llvm", ".toolchain.json"));
        }
        catch { return false; }
    }

    // Why a folder cannot take the install (null: it can): a local drive whose file system holds files over 4 GB
    // (the game's romfs.bin is 17 GB; FAT32 cannot). built: see Preflight.FolderProblem.
    public static string FolderProblem(string f, bool built = false)
    {
        try
        {
            if (!Path.IsPathRooted(f) || File.Exists(f)) return "Choose a folder.";
            if (built) return Preflight.FolderProblem(f, true);
            var d = new DriveInfo(Path.GetPathRoot(f));
            if (d.DriveType != DriveType.Fixed) return "Choose a folder on a local drive (not a removable or network one).";
            string format = Wine.FileSystem(f) ?? d.DriveFormat;
            if (format.StartsWith("FAT", StringComparison.OrdinalIgnoreCase))
                return string.Format("{0} is formatted {1}, which cannot hold files over 4 GB (the game's data needs one of 17 GB). Choose a folder on an NTFS drive.",
                                     Wine.Active ? f : d.Name, format);
            return Preflight.FolderProblem(f);
        }
        catch (Exception e) { return e.Message; }
    }

    static string DescribeFolder(string f)
    {
        long free = Wine.FreeBytes(f);
        string exfat = Preflight.NoHardLinks(f) ? "exFAT: each update rebuilds from scratch" : null;
        return free < 0 ? (exfat == null ? f : f + "   (" + exfat + ")")
             : string.Format("{0}   ({1:0} GB free{2})", f, free / 1073741824.0, exfat == null ? "" : ", " + exfat);
    }

    // The product's name as one word ("WeepingOfTheEmpire"): folder names without spaces
    internal static readonly string FolderName = string.Concat(Release.Product.Split(' ').Where(w => w.Length > 0).Select(w => char.ToUpperInvariant(w[0]) + w.Substring(1)));

    // The fixed drive with the most free space, in a folder named after the product (no spaces: fewer surprises)
    static string DefaultFolder()
    {
        // Wine: the Linux home (Z:\home\<user>), not C: (Proton's prefix, deleted with it) nor Z:\ (/, not writable)
        string home = Wine.Home();
        if (home != null && FolderProblem(home) == null) return Path.Combine(home, FolderName);
        DriveInfo best = null;
        foreach (var d in DriveInfo.GetDrives())
            try { if (d.IsReady && FolderProblem(d.RootDirectory.FullName) == null && (best == null || d.AvailableFreeSpace > best.AvailableFreeSpace)) best = d; }
            catch { }
        return Path.Combine(best != null ? best.RootDirectory.FullName : "C:\\", FolderName);
    }

    // A newer release on GitHub: said once in the log (its Setup updates this install; nothing is fetched here)
    void CheckNewer()
    {
        new Thread(() =>
        {
            try
            {
                ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
                var req = (HttpWebRequest)WebRequest.Create("https://api.github.com/repos/" + Release.Repo + "/releases/latest");
                req.UserAgent = "Setup"; req.Timeout = 8000;
                string body;
                using (var r = req.GetResponse()) using (var s = new StreamReader(r.GetResponseStream())) body = s.ReadToEnd();
                var m = Regex.Match(body, "\"tag_name\"\\s*:\\s*\"([^\"]+)\"");
                if (m.Success && m.Groups[1].Value != Release.Tag)
                    Post(() => status.Add(string.Format("A newer version is out ({0}): get its Setup from github.com/{1}/releases",
                                                                          m.Groups[1].Value, Release.Repo)));
            }
            catch { }   // (offline, or no release yet)
        }) { IsBackground = true }.Start();
    }

    // Setup remembers the four choices (paths only) so an update is one click
    void RestoreChoices()
    {
        using (var k = Registry.CurrentUser.OpenSubKey(RegKey))
        {
            Func<string, string> get = n => k == null ? null : k.GetValue(n) as string;
            string dir = get("InstallDir");
            // (an install made before a folder rule existed, e.g. in OneDrive, stays where it is: an update reuses its build)
            install.Set(dir != null && (IsFolderTarget(dir) || Built(dir)) ? dir : DefaultFolder());
            foreach (var r in new[] { new { Row = dump, Name = "Dump" }, new { Row = keys, Name = "Keys" }, new { Row = update, Name = "Update" },
                                      new { Row = firmware, Name = "Firmware" } })
            {
                string p = get(r.Name);
                if (p != null && (File.Exists(p) || Directory.Exists(p)) && (ShowUpdate || r.Row != update)) r.Row.Offer(p);
            }
            string tag = get("Tag");
            if (tag != null) build.Text = tag == Release.Tag ? "Rebuild" : "Update";
            object copied = k == null ? null : k.GetValue("CopyGameFiles");   // (the player's own choice, once made)
            if (copied is int) copy.Set((int)copied != 0, true);
        }
    }

    void SaveChoices(bool done)
    {
        try
        {
            using (var k = Registry.CurrentUser.CreateSubKey(RegKey))
            {
                k.SetValue("InstallDir", install.FilePath); k.SetValue("Dump", dump.FilePath);
                k.SetValue("Keys", keys.FilePath); if (firmware.FilePath != null) k.SetValue("Firmware", firmware.FilePath); else k.DeleteValue("Firmware", false);
                if (update.FilePath != null) k.SetValue("Update", update.FilePath); else k.DeleteValue("Update", false);
                if (copy.Touched && update.FilePath == null) k.SetValue("CopyGameFiles", copy.Checked ? 1 : 0, RegistryValueKind.DWord);
                if (done) k.SetValue("Tag", Release.Tag);
                if (done && launcher != null) k.SetValue("Launcher", launcher);   // (what --play starts)
            }
        }
        catch { }
    }

    // the folder's .setup-root names the folder itself: a junction that shows it leads to that folder
    static bool Marks(string link, string dir)
    {
        try { return File.ReadAllText(Path.Combine(link, ".setup-root")).Trim() == dir; } catch { return false; }
    }

    // A folder the compilers and the build scripts all take as is: ASCII letters, digits and - _ . only, and short
    // (the build nests up to ~190 characters of its own below it; Windows' default path limit is 260). Any other folder
    // (spaces, commas, accents, other scripts, deep) is reached through a junction with such a name, so no tool ever
    // sees the original path (the files still live in the chosen folder).
    const int MaxRoot = 48;
    static bool Plain(string p) { return p.Length <= MaxRoot && Regex.IsMatch(p, @"^[A-Za-z]:\\[A-Za-z0-9_.\\-]*$"); }

    string SafeRoot(string dir)
    {
        if (Plain(dir)) return dir;
        string hash;
        using (var h = SHA256.Create()) hash = BitConverter.ToString(h.ComputeHash(Encoding.UTF8.GetBytes(dir.ToLowerInvariant()))).Replace("-", "").Substring(0, 10).ToLowerInvariant();
        foreach (var root in new[] { Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), Path.GetPathRoot(Environment.SystemDirectory) })
        {
            string parent = Path.Combine(root, FolderName);
            string link = Path.Combine(parent, "j" + hash);
            if (!Plain(link)) continue;
            try
            {
                Directory.CreateDirectory(parent);
                if (Directory.Exists(link))
                {
                    if (Marks(link, dir)) return link;
                    Directory.Delete(link);   // (a junction to another folder: removes the link only, never its contents)
                }
                // (Wine: a Linux symbolic link instead, Wine.LinkFolder; Proton's cmd makes no junctions)
                if (Wine.Active) { if (Wine.LinkFolder(link, dir) && Marks(link, dir)) return link; continue; }
                if (Win32.Junction(link, dir) && Marks(link, dir)) return link;   // (as mklink /J, through the API)
            }
            catch { }
        }
        if (Wine.Active)   // (no Linux link either: the folder itself must be plain)
            throw new IOException("Could not reach " + dir + " through a plain-named folder (Wine could not make the Linux link Setup uses). Choose a folder whose path uses only English letters, digits and - _ . and has at most " + MaxRoot + " characters, for example " + (Wine.Home() != null ? Path.Combine(Wine.Home(), FolderName) : @"C:\" + FolderName) + ".");
        throw new IOException("Could not reach " + dir + " through a plain-named folder. Choose a folder whose name uses only English letters and digits.");
    }

    // ---- running ------------------------------------------------------------------------------------------------------

    // To the window's thread from a worker, a download or a process event. Those can still fire while the window
    // closes (Stop's taskkill ends the build: its Exited and last output lines arrive after): then they are dropped,
    // instead of an exception on a pool thread that would end the process with a crash report.
    void Post(Action a)
    {
        try { if (!IsDisposed && IsHandleCreated && !closing) BeginInvoke(a); }
        catch (InvalidOperationException) { }   // (ObjectDisposedException too)
    }
    volatile bool closing;

    void Append(string line)
    {
        if (InvokeRequired) { Post(() => Append(line)); return; }
        if (log != null) try { log.WriteLine(line); } catch { }
        var m = StepLine.Match(line);
        if (m.Success)
        {
            int i = int.Parse(m.Groups[1].Value), n = int.Parse(m.Groups[2].Value);
            var name = m.Groups[3].Value;
            status.Step = i == 0 ? "Getting the build tools" : string.Format("Step {0} of {1}: {2}", i, n, char.ToUpper(name[0]) + name.Substring(1));
            stepStart = Progress(name); stepSpan = Span(name);
            status.Value = stepStart;
            status.Add(string.Format("step {0}/{1} {2}", i, n, name));   // (the command line itself is noise here)
            if (name == "romfs" && !scanNote)   // (the first step that writes tens of GB: antivirus scanning shows here)
            {
                scanNote = true;
                Append("[Builder] Windows Security scans every new file: on some PCs that makes this step and the build slower. That is normal.");
            }
            return;
        }
        var p = Percent.Match(line);
        if (p.Success) status.Value = Math.Max(status.Value, stepStart + stepSpan * int.Parse(p.Groups[1].Value) / 100f);   // (never back: a step may fetch several things)
        if (line.StartsWith("[Builder] DONE: ")) launcher = line.Substring("[Builder] DONE: ".Length).Trim();
        status.Add(line.Replace("[Builder] ", ""));
    }

    float stepStart, stepSpan;
    static float Total() { float t = 0; foreach (var w in Weight.Values) t += w; return t; }
    static float Span(string name) { float w; return Weight.TryGetValue(name, out w) ? w / Total() : 0; }
    static float Progress(string name)
    {
        float t = 0;
        foreach (var kv in Weight) { if (kv.Key == name) return t / Total(); t += kv.Value; }
        return 0;
    }

    void Lock(bool on)
    {
        foreach (var r in rows) r.Locked = on; copy.Locked = on || update.FilePath != null; copy.Invalidate();
        if (uninstall != null) uninstall.Visible = !on;   // (never mid-build)
    }

    // The newest 1.0.0 build's launcher in an install folder (game\TotK-PC-<date>-<time>-<commit>: by name, not by
    // when a folder was last written, which a mod edited in an older kept folder changes; a 1.4.3 build, -v143, only
    // when there is no other), or null
    public static string NewestLauncher(string root)
    {
        try
        {
            var all = new DirectoryInfo(Path.Combine(root, "game")).GetDirectories("TotK-PC-*")
                .Where(d => !d.Name.EndsWith(".tmp") && File.Exists(Path.Combine(d.FullName, "TotK Launcher.exe")))
                .OrderByDescending(d => d.Name, StringComparer.Ordinal).ToList();
            var pick = all.FirstOrDefault(d => !Regex.IsMatch(d.Name, @"-v\d{3}(-beta)?$")) ?? all.FirstOrDefault();
            return pick == null ? null : Path.Combine(pick.FullName, "TotK Launcher.exe");
        }
        catch { return null; }
    }

    // Setup --play: the installed game starts and Setup ends (no window); nothing installed yet: Setup's window. One
    // Steam shortcut (one Proton prefix: the registry, saves and settings Setup and the game share) then runs both:
    // Setup itself the first time, the game with the launch option --play (README, Linux and Steam Deck)
    static bool PlayInstalled()
    {
        string root = InstalledDir(), exe = null;
        if (root == null) return false;
        try   // (the launcher the last install finished with, while it is still in this install folder)
        {
            using (var k = Registry.CurrentUser.OpenSubKey(RegKey)) exe = k == null ? null : k.GetValue("Launcher") as string;
            if (exe != null && (!File.Exists(exe) || !Path.GetFullPath(exe).StartsWith(Path.GetFullPath(root).TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase))) exe = null;
        }
        catch { exe = null; }
        exe = exe ?? NewestLauncher(root);
        if (exe == null) return false;
        try { Process.Start(new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe) }); return true; }
        catch { return false; }
    }

    // the install folder Setup remembers, when it still holds an install (its tools); else null
    static string InstalledDir()
    {
        try
        {
            using (var k = Registry.CurrentUser.OpenSubKey(RegKey))
            {
                string d = k == null ? null : k.GetValue("InstallDir") as string;
                return d != null && Directory.Exists(Path.Combine(d, "tools")) ? d : null;
            }
        }
        catch { return null; }
    }

    void Start()
    {
        if (repo == null && !Consent()) return;
        Environment.SetEnvironmentVariable("GIT_CONFIG_COUNT", "1");
        Environment.SetEnvironmentVariable("GIT_CONFIG_KEY_0", "safe.directory");
        Environment.SetEnvironmentVariable("GIT_CONFIG_VALUE_0", "*");
        build.Enabled = false; Lock(true); stopping = false; launcher = null; scanNote = false;
        status.Clear(); diskLine = null; status.Failed = false; status.Running = true; status.Value = 0; status.Step = "Starting"; status.Invalidate();
        if (repo != null) { RunBuilder(repo, "py", "-3 ", null, CopyArg()); return; }
        SaveChoices(false);
        string dir = install.FilePath.TrimEnd('\\');
        bool withUpdate = update.FilePath != null;
        string copyArg = CopyArg();   // (read here: the worker thread must not touch the controls)
        string dumpFile = dump.FilePath, keysFile = keys.FilePath, updateFile = update.FilePath;
        bool copyRomfs = copy.Checked;
        worker = new Thread(() =>
        {
            try
            {
                // the folder as when it is picked: one given by --install-to (the launcher's updates) or remembered from
                // an earlier run was never checked by this Setup (a game already built there: only what stops a build)
                string folder = noPreflight ? null : FolderProblem(dir, Built(dir));
                if (folder != null) throw new IOException(folder);
                Directory.CreateDirectory(dir);
                var w = new StreamWriter(Path.Combine(dir, "setup.log"), true, new UTF8Encoding(false)) { AutoFlush = true };
                w.WriteLine("---- " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " Setup " + Release.Tag);
                Post(() => log = w);
                if (!noPreflight)
                {
                    var problems = new List<string>(); var notes = new List<string>(); var details = new List<string>();
                    Preflight.Check(dir, problems, notes, details, withUpdate, copyRomfs);
                    string files = Inputs.Check(dumpFile, keysFile, updateFile);   // (a wrong dump or keys: before the downloads)
                    if (files != null) problems.Add(files);
                    Post(() => { if (log != null) foreach (var d in details) log.WriteLine(d); });   // (the log only: support)
                    foreach (var n in notes) Append("[Builder] " + n);
                    foreach (var p in problems) Append("[Builder] " + p);
                    if (problems.Count > 0)   // (the log, not the window: for support, when the check is wrong about a PC)
                        Post(() => { if (log != null) log.WriteLine("(support: starting Setup with --no-preflight skips this check)"); });
                    if (problems.Count > 0) throw new IOException("Setup stopped before downloading anything: fix what is said above, then press Install again.");
                }
                File.WriteAllText(Path.Combine(dir, ".setup-root"), dir);   // (SafeRoot checks its junction by it)
                string root = SafeRoot(dir);
                if (root != dir) Append("[Builder] working through " + root + " (a short plain path every compiler takes)");
                string tools = Path.Combine(root, "tools");
                toolsDir = tools;
                Ensure(Fetch.Python, tools); Ensure(Fetch.Git, tools);
                Ensure(Fetch.Vulkan, tools); Ensure(Fetch.Llvm, tools, ClangNames);
                string src = Source(root, tools);
                Post(() =>
                {
                    worker = null;
                    var env = new Dictionary<string, string> { { "PATH", Path.Combine(tools, "git", "cmd") + ";" + Environment.GetEnvironmentVariable("PATH") } };
                    RunBuilder(src, Path.Combine(tools, "python", "python.exe"), "-E -s -X utf8 ", env,
                               string.Format(" --install \"{0}\"{1}{2}", root, copyArg, builderArgs));
                });
            }
            catch (Exception ex)
            {
                Post(() =>
                {
                    if (!(ex is OperationCanceledException)) Append("[Builder] ERROR: " + ex.Message);
                    worker = null; Finish(false);
                });
            }
        }) { IsBackground = true };
        worker.Start();
    }

    // One pinned download into <tools>\<dir>, skipped when its stamp says it is already there and its key file is (an
    // antivirus that removed it: fetched again, so Install again repairs it)
    void Ensure(Fetch.Item it, string tools, Action<string> after = null)
    {
        if (it.Sha256.Length == 0) return;   // (a Builder.exe pins no release assets: the PC's own tools serve)
        string dir = Path.Combine(tools, it.Dir), stamp = Path.Combine(dir, ".setup.json");
        if (File.Exists(stamp) && File.ReadAllText(stamp).Contains(it.Sha256) && it.Keys.All(k => File.Exists(Path.Combine(dir, k)))) return;
        Directory.CreateDirectory(tools);
        string zip = Path.Combine(tools, it.Dir + ".zip");
        Append("[Builder] step 0/8 tools");
        string local = Fetch.Local(it, AppDomain.CurrentDomain.BaseDirectory);
        if (local != null) { Append("[Builder] " + it.Name + ": the copy next to Setup"); File.Copy(local, zip, true); }
        else
        {
            Append("[Builder] downloading " + it.Name);
            Fetch.Download(it, zip, (done, total, rate) => Append(total > 0
                ? string.Format("[Builder] download {0}: {1}% ({2:0}/{3:0} MB{4})", it.Name, done * 100 / total, done / 1048576.0, total / 1048576.0,
                                Fetch.RateText(done, total, rate))
                : string.Format("[Builder] download {0}: {1:0} MB{2}", it.Name, done / 1048576.0, Fetch.RateText(done, 0, rate))), () => stopping,
                m => Append("[Builder] download " + m));
        }
        Append("[Builder] unpacking " + it.Name);
        Fetch.Unzip(it, zip, dir);
        File.Delete(zip);
        if (after != null) after(dir);
        File.WriteAllText(stamp, "{\"sha256\": \"" + it.Sha256 + "\"}\n");
    }

    // After a failed run: a file Setup put in <tools> that is gone (an antivirus took it), or null. Install again
    // fetches it again (Ensure checks the same files), unless the antivirus takes it again.
    internal static string RemovedTool(string tools)
    {
        foreach (var it in new[] { Fetch.Python, Fetch.Git, Fetch.Vulkan, Fetch.Llvm })
            foreach (var k in it.Keys)
            {
                string dir = Path.Combine(tools, it.Dir);
                if (File.Exists(Path.Combine(dir, ".setup.json")) && !File.Exists(Path.Combine(dir, k))) return Path.Combine(dir, k);
            }
        return null;
    }

    // clang.exe is also clang-cl.exe and clang++.exe (the driver reads its own name): hard links, copies if refused
    static void ClangNames(string llvm)
    {
        string clang = Path.Combine(llvm, "bin", "clang.exe");
        foreach (var n in new[] { "clang-cl.exe", "clang++.exe" })
        {
            string p = Path.Combine(llvm, "bin", n);
            if (File.Exists(p)) File.Delete(p);
            if (!CreateHardLink(p, clang, IntPtr.Zero)) File.Copy(clang, p);
        }
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateHardLink(string link, string target, IntPtr security);

    // Before the first download: what comes from where, and Microsoft's terms for its C++ libraries
    bool Consent()
    {
        if (acceptTerms) return true;
        using (var k = Registry.CurrentUser.OpenSubKey(RegKey))
            if (k != null && k.GetValue("AcceptedTerms") as string == TermsVersion) return true;
        var r = MessageBox.Show(
            "Setup downloads the free tools the build needs into the install folder: Python, MinGit, the Python " +
            "packages the build uses, and LLVM's compiler and the Vulkan SDK files from this project's GitHub release. " +
            "When Visual Studio's C++ tools are not installed, it also downloads " +
            "Microsoft's C++ libraries and the Windows SDK from Microsoft's servers, under the Visual Studio Build " +
            "Tools license:\n\n" + MsLicense + "\n\nNothing is installed system-wide. Accept and continue?",
            Text, MessageBoxButtons.YesNo, MessageBoxIcon.Information);
        if (r != DialogResult.Yes) return false;
        try { using (var k = Registry.CurrentUser.CreateSubKey(RegKey)) k.SetValue("AcceptedTerms", TermsVersion); } catch { }
        return true;
    }
    const string MsLicense = "https://go.microsoft.com/fwlink/?LinkId=2179911";
    // The terms the player accepted (registry AcceptedTerms): raise it whenever the text above or the license changes, so
    // every player reads the new ones once, also in the launcher's automatic update (it runs Setup without --accept-terms)
    const string TermsVersion = "1";

    // The repository at this Setup's tag: cloned once, then moved to the new tag by a newer Setup (an update)
    string Source(string root, string tools)
    {
        string git = Path.Combine(tools, "git", "cmd", "git.exe"), src = Path.Combine(root, "src");
        string url = source ?? "https://github.com/" + Release.Repo + ".git";
        if (!Directory.Exists(Path.Combine(src, ".git")))
        {
            if (Directory.Exists(src)) Directory.Delete(src, true);   // (a first fetch cut short)
            Run(git, string.Format("init --quiet \"{0}\"", src), null);
            Run(git, string.Format("-C \"{0}\" config core.autocrlf false", src), null);
        }
        // only the tag's own files (no history); an update moves the same folder to the newer tag
        Append("[Builder] fetching the source (" + Release.Tag + ")");
        try { Run(git, string.Format("-C \"{0}\" fetch --progress --depth 1 --force \"{1}\" \"{2}\"", src, url, Release.Tag), null); }
        catch (IOException e)   // (git's own last lines are already in the log above)
        {
            throw new IOException(string.Format("Could not download the source code ({0}) from {1}. Check the internet connection; a firewall, " +
                "proxy or school / work network can block github.com. Then press Install again. ({2})", Release.Tag, source ?? "github.com", e.Message));
        }
        Run(git, string.Format("-C \"{0}\" checkout --quiet --force --detach FETCH_HEAD", src), null);
        return src;
    }

    // A helper process to its end; its output goes to the log (git's progress moves the bar); a failure throws
    void Run(string exe, string args, string cwd, Dictionary<string, string> env = null)
    {
        var psi = new ProcessStartInfo(exe, args)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
        };
        if (cwd != null) psi.WorkingDirectory = cwd;
        if (env != null) foreach (var kv in env) psi.EnvironmentVariables[kv.Key] = kv.Value;
        var tail = new List<string>();
        using (var p = new Process { StartInfo = psi })
        {
            DataReceivedEventHandler h = (s, e) =>
            {
                if (string.IsNullOrEmpty(e.Data)) return;
                foreach (var ln in e.Data.Split('\r'))
                {
                    if (ln.Trim().Length == 0) continue;
                    lock (tail) { tail.Add(ln); if (tail.Count > 4) tail.RemoveAt(0); }
                    var g = GitPercent.Match(ln);
                    if (g.Success) Append(string.Format("[Builder] {0}: {1}% (git)", g.Groups[1].Value, g.Groups[2].Value));
                }
            };
            p.OutputDataReceived += h; p.ErrorDataReceived += h;
            p.Start(); p.BeginOutputReadLine(); p.BeginErrorReadLine();
            while (!p.WaitForExit(500)) if (stopping) { try { p.Kill(); } catch { } throw new OperationCanceledException(); }
            p.WaitForExit();
            if (p.ExitCode != 0)
            {
                lock (tail) foreach (var ln in tail) Append(ln);
                throw new IOException(string.Format("{0} failed (exit code {1}); the last lines are below", Path.GetFileName(exe), p.ExitCode));
            }
        }
    }

    void RunBuilder(string src, string python, string pyArgs, Dictionary<string, string> env, string extra = "")
    {
        string fw = firmware.FilePath == null ? "" : " \"" + firmware.FilePath.TrimEnd('\\') + "\"";   // (optional)
        if (update.FilePath != null) fw += " --update \"" + update.FilePath + "\"";   // (optional: builds 1.4.3)
        var psi = new ProcessStartInfo(python, string.Format("{0}\"{1}\" \"{2}\" \"{3}\"{4} --gui-pid {5}{6}",   // (it ends with us)
            pyArgs, Path.Combine(src, "tools", "builder", "builder.py"), dump.FilePath, keys.FilePath, fw,
            Process.GetCurrentProcess().Id, extra))
        {
            WorkingDirectory = src, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
        };
        psi.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
        psi.EnvironmentVariables["PYTHONUTF8"] = "1";
        if (env != null) foreach (var kv in env) psi.EnvironmentVariables[kv.Key] = kv.Value;
        try
        {
            proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
            proc.OutputDataReceived += (s, e) => { if (e.Data != null) Append(e.Data); };
            proc.ErrorDataReceived += (s, e) => { if (e.Data != null) Append(e.Data); };
            proc.Exited += (s, e) => Post(() => { if (proc == null) return; bool ok = proc.ExitCode == 0; proc = null; Finish(ok); });
            proc.Start();
            BuildJob(proc);   // (Stop ends the whole build through it)
            proc.BeginOutputReadLine(); proc.BeginErrorReadLine();
        }
        catch (Exception ex)
        {
            proc = null; Finish(false);
            status.Step = "Python not found";
            status.Add("Could not start Python (" + python + "): " + ex.Message);
            if (repo != null) status.Add("Install Python 3.11 or newer from python.org, then try again.");
        }
    }

    void Finish(bool ok)
    {
        status.Running = false; status.Failed = !ok;
        Lock(false);
        if (ok && launcher != null && File.Exists(launcher))
        {
            if (repo == null) { SaveChoices(true); RegisterUninstall(); }
            // (Controlled folder access blocks an unknown program's shortcut on the desktop: then Play is the way in)
            // (Wine: a desktop shortcut lands in the prefix's own Desktop folder, which no Linux desktop shows: Steam's
            // "Add a Non-Steam Game" with the launcher is the way in, README)
            status.Step = Wine.Active ? "Done: press Play, or add " + Wine.LinuxPath(launcher) + " to Steam (README, Linux and Steam Deck)"
                        : noShortcut || Shortcut(launcher) ? "Done: the game is on your desktop"
                        : "Done: press Play (Windows did not let Setup put a shortcut on the desktop)";
            status.Value = 1f; build.Text = "Play";
        }
        else
        {
            launcher = null;
            status.Step = stopping ? "Stopped" : ok ? "Done" : "Stopped: see the last lines below";
            string gone = !ok && !stopping && toolsDir != null ? RemovedTool(toolsDir) : null;
            if (gone != null)
                Append("[Builder] Windows Security (or another antivirus) seems to have removed " + gone + ". Restore it in Windows Security > " +
                       "Virus & threat protection > Protection history (Allow), then press Install again: Setup fetches what is missing.");
            build.Text = "Try again";
        }
        status.Invalidate();
        build.Enabled = true;
        if (log != null) { log.WriteLine(status.Step); log.Dispose(); log = null; }
        if (hidden) { Environment.ExitCode = ok ? 0 : 1; Close(); }
    }

    // Windows' Settings > Apps entry: a copy of this Setup in the install folder (the downloaded one may be deleted)
    // runs as the uninstaller; written again by every install and update (no admin: the user's own registry)
    void RegisterUninstall()
    {
        try
        {
            string root = Path.GetFullPath(install.FilePath).TrimEnd('\\'), copyTo = Path.Combine(root, "Setup.exe");
            if (!string.Equals(Path.GetFullPath(Application.ExecutablePath), copyTo, StringComparison.OrdinalIgnoreCase))
                File.Copy(Application.ExecutablePath, copyTo, true);
            Uninstaller.Register(root, copyTo, launcher);
        }
        catch (Exception e) { Append("[Builder] Windows' Apps list entry not written (" + e.Message + "): Setup --uninstall still works"); }
    }

    void Play()
    {
        // outside this window's job (it allows breakaway), so closing the window leaves the game running; where a job above
        // ours forbids that, the game starts in ours and ours stops killing on close (the build is over by then).
        // (Wine: no job to leave, Main; the launcher starts directly)
        try
        {
            if (Wine.Active) Process.Start(new ProcessStartInfo(launcher) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(launcher) });
            else if (Win32.StartOutsideJob(launcher, null) == 0)
            {
                KeepChildrenOnClose();
                Process.Start(new ProcessStartInfo(launcher) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(launcher) });
            }
        }
        catch (Exception ex) { status.Add("Could not start the game: " + ex.Message); }
    }

    // The desktop shortcut, pointed at the newest build (an update re-points it; saves and settings live in the user's
    // profile, not in the build folder, so they carry over)
    static bool Shortcut(string target)
    {
        try
        {
            // (a 1.4.3 build, ...-v143 folder, gets its own shortcut: the 1.0.0 one keeps pointing at 1.0.0)
            var v = Regex.Match(Path.GetFileName(Path.GetDirectoryName(target)), @"-v(\d)(\d)(\d)(-beta)?$");
            string name = Release.Product + (v.Success ? string.Format(" {0}.{1}.{2}", v.Groups[1], v.Groups[2], v.Groups[3]) : "");
            string lnk = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), name + ".lnk");
            Win32.SaveShortcut(lnk, target, Path.GetDirectoryName(target));
            return File.Exists(lnk);
        }
        catch { return false; }
    }

    // Stop: the build is python (builder.py) -> build.py -> compilers, all below the process we started. They run in a
    // job of their own (BuildJob, nested in Setup's): ending it ends every one at once, so nothing between a snapshot
    // and the parent's end can start a next step that outlives the stop. Without that job (old Windows, Wine): the tree,
    // parent first (Win32.KillTree, as taskkill /T did). Setup's own job catches anything left at exit.
    void Stop()
    {
        stopping = true;
        try
        {
            if (proc == null || proc.HasExited) return;
            if (buildJob != IntPtr.Zero && TerminateJobObject(buildJob, 1)) return;
            Win32.KillTree(proc.Id);
        }
        catch { }
    }

    IntPtr buildJob;   // (one per build: closed with the next, or at exit, which ends whatever is still in it)
    [DllImport("kernel32.dll")] static extern bool TerminateJobObject(IntPtr job, uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    void BuildJob(Process p)
    {
        if (buildJob != IntPtr.Zero) { CloseHandle(buildJob); buildJob = IntPtr.Zero; }
        IntPtr j = CreateJobObject(IntPtr.Zero, null);
        if (j == IntPtr.Zero) return;
        var info = new ExtendedLimits();
        info.Basic.Flags = 0x2000;   // KILL_ON_JOB_CLOSE (python starts right away, its first child well after this)
        if (SetInformationJobObject(j, 9, ref info, Marshal.SizeOf(typeof(ExtendedLimits))) &&
            AssignProcessToJobObject(j, p.Handle))
            buildJob = j;
        else
            CloseHandle(j);
    }

    // The exe sits in a job object that kills every process in it when it exits (closed, stopped or crashed), so no
    // build keeps running hidden after the window is gone. Children inherit the job; a Microsoft Store Python starts
    // outside it, so builder.py also ends its own tree when we go (--gui-pid).
    [StructLayout(LayoutKind.Sequential)]
    struct BasicLimits { public long PerProcessTime, PerJobTime; public uint Flags; public UIntPtr MinWs, MaxWs; public uint ActiveProcs; public UIntPtr Affinity; public uint Priority, SchedClass; }
    [StructLayout(LayoutKind.Sequential)]
    struct IoCounters { public ulong R, W, O, RB, WB, OB; }
    [StructLayout(LayoutKind.Sequential)]
    struct ExtendedLimits { public BasicLimits Basic; public IoCounters Io; public UIntPtr ProcMem, JobMem, PeakProcMem, PeakJobMem; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObject(IntPtr attr, string name);
    [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr job, int cls, ref ExtendedLimits info, int size);
    [DllImport("kernel32.dll")] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    static IntPtr job;   // (held open for the life of the process; the system closes it when the exe exits)

    static void JoinKillOnCloseJob()
    {
        job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero) return;
        var info = new ExtendedLimits();
        info.Basic.Flags = 0x2000 | 0x800;   // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | _BREAKAWAY_OK (Play starts the game outside it)
        if (SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(typeof(ExtendedLimits))))   // JobObjectExtendedLimitInformation
            AssignProcessToJobObject(job, Process.GetCurrentProcess().Handle);   // (fails only on old Windows in a job: Stop's KillTree still works)
    }

    // the job no longer ends its processes when Setup exits (Play, where the game could not leave the job)
    static void KeepChildrenOnClose()
    {
        if (job == IntPtr.Zero) return;
        var info = new ExtendedLimits();
        info.Basic.Flags = 0x800;
        SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(typeof(ExtendedLimits)));
    }

    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [DllImport("shcore.dll")] static extern int SetProcessDpiAwareness(int value);

    // Setup --uninstall [--install-to=DIR] (Windows' Settings > Apps runs the install folder's Setup.exe so): asks, then
    // removes what Setup made (Uninstaller), the saves only on a second Yes (default No), and says how much was freed.
    // 0 done, 1 nothing there / failed, 2 the player said No
    static int UninstallMain(string[] args)
    {
        string root = args.Where(a => a.StartsWith("--install-to=")).Select(a => a.Substring("--install-to=".Length).Trim('"')).FirstOrDefault();
        if (root == null)
            using (var k = Registry.CurrentUser.OpenSubKey(RegKey)) root = k == null ? null : k.GetValue("InstallDir") as string;
        string title = Release.Product + " Setup";
        if (string.IsNullOrEmpty(root) || !Directory.Exists(root))
        {
            MessageBox.Show("Nothing to uninstall: no install folder was found" + (string.IsNullOrEmpty(root) ? "." : " at " + root + "."), title);
            try { Registry.CurrentUser.DeleteSubKeyTree(Uninstaller.AppsKey, false); } catch { }
            return 1;
        }
        root = Path.GetFullPath(root).TrimEnd('\\');
        // (run from the install folder's own Setup.exe, the copy Windows' Apps entry starts: everything else goes, that one
        //  running file stays and so does the folder, said at the end; no helper copy, no child process)
        string self = Path.GetFullPath(Application.ExecutablePath);
        string keepSelf = self.StartsWith(root + "\\", StringComparison.OrdinalIgnoreCase) ? self : null;
        string why = Uninstaller.NotOurs(root);
        if (why != null)
        {
            MessageBox.Show("Nothing was removed: " + why + ", so it does not look like a folder Setup installed into.", title,
                            MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return 1;
        }
        if (Process.GetProcessesByName("totk").Length > 0 || Process.GetProcessesByName("TotK Launcher").Length > 0)
        {
            MessageBox.Show("A game window is open (TotK Launcher or totk.exe, from any folder). Close it, then uninstall again.", title,
                            MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return 1;
        }
        if (MessageBox.Show(string.Format("Uninstall {0} from {1}?\n\nThis removes the game build, its tools, the desktop shortcut, and the " +
                                          "game's caches and logs (shared by every build of the game on this PC). Your dump and keys are not touched.", Release.Product, root), title,
                            MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
            return 2;   // (the player said No)
        string roaming = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "nxrecomp", "TotK-engine");
        string local = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "nxrecomp", "TotK-engine");
        bool saves = Directory.Exists(roaming) &&
            MessageBox.Show("Also delete your saves, settings and mods?\n\n" + roaming + "\n\nNo keeps them: a later install finds them again.",
                            title, MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2) == DialogResult.Yes;
        var parents = new[] { Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), Path.GetPathRoot(Environment.SystemDirectory) }
            .Select(x => Path.Combine(x, FolderName));
        var r = Uninstaller.Run(root, parents, Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), Release.Product,
                                local, saves ? roaming : null, Uninstaller.ShortcutTarget, keepSelf);
        try { Registry.CurrentUser.DeleteSubKeyTree(RegKey, false); } catch { }
        try { Registry.CurrentUser.DeleteSubKeyTree(Uninstaller.AppsKey, false); } catch { }
        if (r.Refused != null) { MessageBox.Show("Nothing was removed: " + r.Refused + ".", title); return 1; }
        string msg = string.Format("{0} is uninstalled: about {1:0.0} GB freed.", Release.Product, Math.Max(0, r.Freed) / 1e9);
        if (!saves && Directory.Exists(roaming)) msg += "\n\nYour saves and settings stay in " + roaming + ".";
        if (keepSelf != null) msg += "\n\nThis Setup (" + keepSelf + ") is still running, so it stays: delete " + root + " once this window is closed.";
        var others = r.Kept.Where(k => keepSelf == null || !string.Equals(k, keepSelf, StringComparison.OrdinalIgnoreCase)).ToList();
        if (others.Count > 0) msg += "\n\nLeft in place (not made by Setup):\n" + string.Join("\n", others.Take(6)) + (others.Count > 6 ? "\n..." : "");
        if (r.Failed.Count > 0) msg += "\n\nCould not remove (in use?):\n" + string.Join("\n", r.Failed.Take(4)) + (r.Failed.Count > 4 ? "\n..." : "");
        MessageBox.Show(msg, title, MessageBoxButtons.OK, r.Failed.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
        return r.Failed.Count == 0 ? 0 : 1;
    }

    [STAThread]   // (drag and drop needs a single-threaded apartment)
    static void Main()
    {
        if (Environment.GetCommandLineArgs().Skip(1).FirstOrDefault() == "--vkcheck") { Environment.Exit(Preflight.VulkanChild()); return; }
        try { SetProcessDpiAwareness(1); } catch { SetProcessDPIAware(); }   // DPI aware: drawn at the real resolution, sharp text
        if (Environment.GetCommandLineArgs().Contains("--uninstall")) { Environment.Exit(UninstallMain(Environment.GetCommandLineArgs())); return; }
        if (Environment.GetCommandLineArgs().Contains("--play") && PlayInstalled()) return;   // (before the job: the game outlives us)
        if (!Wine.Active) JoinKillOnCloseJob();   // (Wine: none; builder.py's --gui-pid still ends a build when we go)
        Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false); Application.Run(new BuilderForm());
    }
}
