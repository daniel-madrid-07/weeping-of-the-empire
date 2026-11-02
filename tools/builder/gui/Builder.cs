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

    // Download url to path with progress(done, total); resumes a .part file, retries with a pause, checks the sha256.
    public static void Download(Item it, string path, Action<long, long> progress, Func<bool> cancelled)
    {
        ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;   // TLS 1.2 (.NET 4.x defaults lower)
        string part = path + ".part";
        for (int attempt = 1; ; attempt++)
        {
            try
            {
                long have = File.Exists(part) ? new FileInfo(part).Length : 0;
                var req = (HttpWebRequest)WebRequest.Create(it.Url);
                req.UserAgent = "Setup"; req.Timeout = 30000; req.ReadWriteTimeout = 60000;
                if (have > 0) req.AddRange(have);
                using (var resp = (HttpWebResponse)req.GetResponse())
                {
                    bool partial = resp.StatusCode == HttpStatusCode.PartialContent;
                    if (!partial) have = 0;   // (the server ignored the range: start over)
                    long total = resp.ContentLength < 0 ? -1 : resp.ContentLength + have;
                    using (var src = resp.GetResponseStream())
                    using (var dst = new FileStream(part, partial ? FileMode.Append : FileMode.Create, FileAccess.Write))
                    {
                        var buf = new byte[1 << 16]; int n; long done = have, last = 0;
                        while ((n = src.Read(buf, 0, buf.Length)) > 0)
                        {
                            if (cancelled()) throw new OperationCanceledException();
                            dst.Write(buf, 0, n); done += n;
                            if (done - last > (1 << 20)) { progress(done, total); last = done; }
                        }
                    }
                }
                string sum = Sha256(part);
                if (!string.Equals(sum, it.Sha256, StringComparison.OrdinalIgnoreCase))
                {
                    File.Delete(part);
                    throw new InvalidDataException(string.Format("{0}: the download does not match its checksum (got {1}); press Install again", it.Name, sum));
                }
                if (File.Exists(path)) File.Delete(path);
                File.Move(part, path);
                return;
            }
            catch (Exception e)
            {
                // a refused request (WebException) or a connection cut mid-body (IOException from Read): retry from the
                // .part; a bad checksum or a cancel are final
                if (!(e is WebException || e is IOException)) throw;
                var r = (e is WebException) ? ((WebException)e).Response as HttpWebResponse : null;
                if (r != null && (int)r.StatusCode == 416) { File.Delete(part); }   // (a stale .part longer than the file)
                if (attempt >= 6)
                {
                    string host = new Uri(it.Url).Host;
                    throw new IOException(string.Format("Could not download {0} from {1} ({2}). Check the internet connection; a firewall, proxy or " +
                        "school / work network can block {1}. Then press Install again: it continues where it stopped.", it.Name, host, e.Message));
                }
                Thread.Sleep(2000 * attempt);
            }
        }
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

// What the PC must have before Setup downloads anything (the first seconds, not after 30 minutes of building): each
// problem is one sentence saying what to do. builder.py checks the same again (and the keys, the dump) once it runs.
static class Preflight
{
    const double MinRamGb = 7.5;                       // (builder.py MIN_RAM_GB: an "8 GB" PC reports ~7.8)
    const int FirstRunGb = 25, ResumeGb = 9, ToolsGb = 5, UpdateUnpackGb = 18;   // (builder.py FIRST_RUN_GB, RESUME_GB, TOOLS_GB, UPDATE_UNPACK_GB)

    // What a first install asks for, as the window says it (Builder mode: no tools; the 1.4.3 update: its RomFS too)
    public static int FirstInstallGb(bool tools, bool update) { return FirstRunGb + (tools ? ToolsGb : 0) + (update ? UpdateUnpackGb : 0); }
    const uint Vk13 = (1u << 22) | (3u << 12);

    [DllImport("kernel32.dll")] static extern bool IsProcessorFeaturePresent(int feature);
    [StructLayout(LayoutKind.Sequential)]
    struct MemoryStatus { public uint Length, Load; public ulong Total, Avail, PageTotal, PageAvail, VirtTotal, VirtAvail, ExtAvail; }
    [DllImport("kernel32.dll")] static extern bool GlobalMemoryStatusEx(ref MemoryStatus m);

    // Problems (the install cannot work: Setup stops) and notes (said once, the install goes on); details: for setup.log
    // only (each GPU the check saw, for support). dir: the install folder (null in Builder mode: builder.py checks it).
    public static void Check(string dir, List<string> problems, List<string> notes, List<string> details = null, bool update = false)
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
        if (dir != null) Disk(dir, update, problems);
        Vulkan(problems, notes, details ?? new List<string>());
        notes.Add(Vp9Note(Vp9Mft()));
        notes.Add("Visual C++ runtime in Windows: " + (CrtVersion() ?? "none"));
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

    // The same sum as builder.py's, a run whose game files (of the version built: v100, or v143 with the update dump)
    // are already extracted 15, the update's unpacked RomFS 18 more until it is, the tools 5
    public static int NeedGb(string dir, bool update = false)
    {
        bool resume = File.Exists(Path.Combine(dir, "work", "extracted", update ? "v143" : "v100", "READY.json"));
        return (resume ? ResumeGb : FirstRunGb) + (update && !resume ? UpdateUnpackGb : 0)
             + (File.Exists(Path.Combine(dir, "tools", "llvm", ".toolchain.json")) ? 0 : ToolsGb);
    }

    static void Disk(string dir, bool update, List<string> problems)
    {
        try
        {
            int need = NeedGb(dir, update);
            var d = new DriveInfo(Path.GetPathRoot(dir));
            double free = d.AvailableFreeSpace / 1073741824.0;
            if (free < need)
                problems.Add(string.Format("Drive {0} has {1:0} GB free; this install needs about {2} GB. Free some space or choose a folder on another drive.",
                                           d.Name.TrimEnd('\\'), free, need));
        }
        catch { }   // (builder.py checks again)
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
    // unknown programs (our compilers) from writing into Documents, Desktop, Pictures, Videos and Music.
    public static string FolderProblem(string f)
    {
        string full;
        try { full = Path.GetFullPath(f).TrimEnd('\\') + "\\"; } catch { return null; }
        foreach (var v in new[] { "OneDrive", "OneDriveConsumer", "OneDriveCommercial" })
        {
            string od = Environment.GetEnvironmentVariable(v);
            if (!string.IsNullOrEmpty(od) && full.StartsWith(od.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase))
                return "This folder is inside OneDrive, which would upload the " + FirstRunGb + " GB of build files. Choose a folder outside OneDrive.";
        }
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

class BuilderForm : Form
{
    readonly Row dump, keys, update, firmware, install;
    readonly Row[] rows;
    readonly PrimaryButton build;
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
            (File.Exists(f) && f.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)));
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

        build = new PrimaryButton { Text = repo != null ? "Build" : "Install", Bounds = new Rectangle(pad, rowY + rows.Length * rowH + T.S6, inner, T.D(42)), Enabled = false };
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
        Controls.AddRange(rows);

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
        //   --no-preflight: skip the PC check (processor, memory, disk, graphics driver) that runs before any download
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
        string dir = install.FilePath;
        bool withUpdate = update.FilePath != null;   // (the 1.4.3 update unpacks 18 GB more)
        new Thread(() =>
        {
            var problems = new List<string>();
            try { Preflight.Check(dir, problems, new List<string>(), null, withUpdate); } catch { return; }
            if (problems.Count > 0) Post(() => { if (!Busy) foreach (var p in problems) status.Add(p); });
        }) { IsBackground = true }.Start();
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

    const string UpdateVersion = "1.4.3";   // (builder.py's UPDATE_VERSION)
    // v0.1.7 ships without the 1.4.3 row: the row is not shown, a dropped or remembered update dump is not taken;
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

    // Why a folder cannot take the install (null: it can): a local drive whose file system holds files over 4 GB
    // (the game's romfs.bin is 17 GB; FAT32 cannot)
    public static string FolderProblem(string f)
    {
        try
        {
            if (!Path.IsPathRooted(f) || File.Exists(f)) return "Choose a folder.";
            var d = new DriveInfo(Path.GetPathRoot(f));
            if (d.DriveType != DriveType.Fixed) return "Choose a folder on a local drive (not a removable or network one).";
            if (d.DriveFormat.StartsWith("FAT", StringComparison.OrdinalIgnoreCase))
                return string.Format("{0} is formatted {1}, which cannot hold files over 4 GB (the game's data needs one of 17 GB). Choose a folder on an NTFS drive.", d.Name, d.DriveFormat);
            return Preflight.FolderProblem(f);
        }
        catch (Exception e) { return e.Message; }
    }

    static string DescribeFolder(string f)
    {
        try { return string.Format("{0}   ({1:0} GB free)", f, new DriveInfo(Path.GetPathRoot(f)).AvailableFreeSpace / 1073741824.0); }
        catch { return f; }
    }

    // The product's name as one word ("WeepingOfTheEmpire"): folder names without spaces
    static readonly string FolderName = string.Concat(Release.Product.Split(' ').Where(w => w.Length > 0).Select(w => char.ToUpperInvariant(w[0]) + w.Substring(1)));

    // The fixed drive with the most free space, in a folder named after the product (no spaces: fewer surprises)
    static string DefaultFolder()
    {
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
            install.Set(dir != null && (IsFolderTarget(dir) || Directory.Exists(Path.Combine(dir, "tools"))) ? dir : DefaultFolder());
            foreach (var r in new[] { new { Row = dump, Name = "Dump" }, new { Row = keys, Name = "Keys" }, new { Row = update, Name = "Update" },
                                      new { Row = firmware, Name = "Firmware" } })
            {
                string p = get(r.Name);
                if (p != null && (File.Exists(p) || Directory.Exists(p)) && (ShowUpdate || r.Row != update)) r.Row.Offer(p);
            }
            string tag = get("Tag");
            if (tag != null) build.Text = tag == Release.Tag ? "Rebuild" : "Update";
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
                if (done) k.SetValue("Tag", Release.Tag);
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
                // (the paths through variables: cmd expands them once, so a % in a folder name stays literal)
                Run("cmd.exe", "/d /c mklink /J \"%SETUP_LINK%\" \"%SETUP_DIR%\"", null,
                    new Dictionary<string, string> { { "SETUP_LINK", link }, { "SETUP_DIR", dir } });
                if (Marks(link, dir)) return link;
            }
            catch { }
        }
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

    void Lock(bool on) { foreach (var r in rows) r.Locked = on; }

    void Start()
    {
        if (repo == null && !Consent()) return;
        Environment.SetEnvironmentVariable("GIT_CONFIG_COUNT", "1");
        Environment.SetEnvironmentVariable("GIT_CONFIG_KEY_0", "safe.directory");
        Environment.SetEnvironmentVariable("GIT_CONFIG_VALUE_0", "*");
        build.Enabled = false; Lock(true); stopping = false; launcher = null; scanNote = false;
        status.Clear(); status.Failed = false; status.Running = true; status.Value = 0; status.Step = "Starting"; status.Invalidate();
        if (repo != null) { RunBuilder(repo, "py", "-3 ", null); return; }
        SaveChoices(false);
        string dir = install.FilePath.TrimEnd('\\');
        bool withUpdate = update.FilePath != null;
        worker = new Thread(() =>
        {
            try
            {
                Directory.CreateDirectory(dir);
                var w = new StreamWriter(Path.Combine(dir, "setup.log"), true, new UTF8Encoding(false)) { AutoFlush = true };
                w.WriteLine("---- " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " Setup " + Release.Tag);
                Post(() => log = w);
                if (!noPreflight)
                {
                    var problems = new List<string>(); var notes = new List<string>(); var details = new List<string>();
                    Preflight.Check(dir, problems, notes, details, withUpdate);
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
                               string.Format(" --install \"{0}\"{1}", root, builderArgs));
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
            Fetch.Download(it, zip, (done, total) => Append(total > 0
                ? string.Format("[Builder] download {0}: {1}% ({2:0}/{3:0} MB)", it.Name, done * 100 / total, done / 1048576.0, total / 1048576.0)
                : string.Format("[Builder] download {0}: {1:0} MB", it.Name, done / 1048576.0)), () => stopping);
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
            if (repo == null) SaveChoices(true);
            // (Controlled folder access blocks an unknown program's shortcut on the desktop: then Play is the way in)
            status.Step = noShortcut || Shortcut(launcher) ? "Done: the game is on your desktop"
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

    void Play()
    {
        // through Explorer: the game is then its child, not ours, so closing this window (and its job) leaves it running
        try { Process.Start(new ProcessStartInfo("explorer.exe", "\"" + launcher + "\"") { UseShellExecute = false }); }
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
            var shellType = Type.GetTypeFromProgID("WScript.Shell");
            object shell = Activator.CreateInstance(shellType);
            object sc = shellType.InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, shell, new object[] { lnk });
            var t = sc.GetType();
            t.InvokeMember("TargetPath", System.Reflection.BindingFlags.SetProperty, null, sc, new object[] { target });
            t.InvokeMember("WorkingDirectory", System.Reflection.BindingFlags.SetProperty, null, sc, new object[] { Path.GetDirectoryName(target) });
            t.InvokeMember("IconLocation", System.Reflection.BindingFlags.SetProperty, null, sc, new object[] { target + ",0" });
            t.InvokeMember("Save", System.Reflection.BindingFlags.InvokeMethod, null, sc, null);
            Marshal.FinalReleaseComObject(sc); Marshal.FinalReleaseComObject(shell);
            return File.Exists(lnk);
        }
        catch { return false; }
    }

    // Stop: the build is python (builder.py) -> build.py -> compilers, all below the process we started, so the whole
    // tree goes (taskkill /T). The job object below catches whatever outlives its parent.
    void Stop()
    {
        stopping = true;
        try
        {
            if (proc == null || proc.HasExited) return;
            using (var k = Process.Start(new ProcessStartInfo("taskkill", "/T /F /PID " + proc.Id) { UseShellExecute = false, CreateNoWindow = true }))
                k.WaitForExit(15000);
        }
        catch { }
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
        info.Basic.Flags = 0x2000;   // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if (SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(typeof(ExtendedLimits))))   // JobObjectExtendedLimitInformation
            AssignProcessToJobObject(job, Process.GetCurrentProcess().Handle);   // (fails only on old Windows in a job: Stop's taskkill still works)
    }

    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [DllImport("shcore.dll")] static extern int SetProcessDpiAwareness(int value);

    [STAThread]   // (drag and drop needs a single-threaded apartment)
    static void Main()
    {
        if (Environment.GetCommandLineArgs().Skip(1).FirstOrDefault() == "--vkcheck") { Environment.Exit(Preflight.VulkanChild()); return; }
        try { SetProcessDpiAwareness(1); } catch { SetProcessDPIAware(); }   // DPI aware: drawn at the real resolution, sharp text
        JoinKillOnCloseJob();
        Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false); Application.Run(new BuilderForm());
    }
}
