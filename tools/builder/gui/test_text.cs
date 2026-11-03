// L (Builder.cs): the text files built into the exe are read whole, every sentence formats, and the language is
// picked as Main picks it; StatusView wraps its lines. Compiled and run by test_text.py (args: the output file, then the
// language codes).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Forms;

static class TextTest
{
    static int fails;
    static void Expect(string what, bool ok, string got = "")
    {
        if (!ok) { fails++; Console.WriteLine("FAIL " + what + (got.Length > 0 ? ": " + got : "")); }
    }

    static string Json(string s)
    {
        var b = new StringBuilder("\"");
        foreach (char c in s)
            if (c == '"' || c == '\\') b.Append('\\').Append(c);
            else if (c < ' ') b.AppendFormat("\\u{0:x4}", (int)c);
            else b.Append(c);
        return b.Append('"').ToString();
    }

    // The window's one-line texts fit their place, in every language (they end in "..." when they do not): the row
    // hints, the copy switch's line, the status line, the row titles and the button (Builder.cs's layout at 96 dpi:
    // a 560 wide window, 32 margins, the rows' text from 48; T's fonts, Microsoft YaHei UI for Chinese)
    static void Fits(string code)
    {
        var places = new[]
        {
            Tuple.Create("Segoe UI", 9f, 392, new[] { "Drop it here or click to choose", "The keys file from the same console",
                "Your {0} update dump: builds version {0}", "Recommended: Korean and Chinese text looks right with its fonts",
                "Only Korean and Chinese text needs it: its folder or .zip", "A folder with about {0} GB free" }),
            Tuple.Create("Segoe UI", 9f, 432, new[] { "Kept from your earlier install, which runs without the dump",
                "Your dump is in OneDrive, which can make it online-only, so they are copied",
                "Your dump is on a USB drive, which can be unplugged, so they are copied",
                "Your dump is on a removable drive, which can be unplugged, so they are copied",
                "Your dump is on a network drive, which can be unplugged, so they are copied",
                "Your dump is on a disc drive, which can be unplugged, so they are copied", "Version {0} always copies them",
                "Uses 17 GB more; afterwards the dump can be moved or deleted", "The game reads them from your dump: keep it where it is" }),
            Tuple.Create("Bahnschrift", 11.5f, 496, new[] { "Ready: Korean and Chinese use a Windows font (no firmware)",
                "Done: press Play (Windows refused the desktop shortcut)", "Done: the game is on your desktop", "Getting the build tools",
                "Step {0} of {1}: {2}", "Stopped: see the last lines below", "{0} of {1} ready", "Python not found" }),
            Tuple.Create("Segoe UI Semibold", 10.5f, 448, new[] { "Dump", "Keys", "Update (optional)", "Firmware (optional)", "Install to",
                "Copy the game files (17 GB)", "Install", "Play", "Try again", "Rebuild", "Update", "Build" }),
        };
        float k96;
        using (var g = Graphics.FromHwnd(IntPtr.Zero)) k96 = 96f / g.DpiX;
        foreach (var p in places)
            using (var f = new Font(L.Cjk ? "Microsoft YaHei UI" : p.Item1, p.Item2))
                foreach (var key in p.Item4)
                {
                    string s = L.S(key).Replace("{0}", "1.4.3").Replace("{1}", "10").Replace("{2}", "Shaders");
                    int w = (int)(TextRenderer.MeasureText(s, f, Size.Empty, TextFormatFlags.NoPadding).Width * k96);
                    Expect(code + ": fits in " + p.Item3 + " px", w <= p.Item3, w + " px: " + key);
                }
    }

    // StatusView's rows: a long sentence wraps whole at spaces, a path or Chinese (no spaces) where it meets the edge
    static void Wraps()
    {
        using (var f = new Font("Cascadia Mono", 8.5f))
        using (var zh = new Font("Microsoft YaHei UI", 8.5f))
        {
            Func<Font, string, int> width = (font, t) => TextRenderer.MeasureText(t, font, Size.Empty, TextFormatFlags.NoPadding).Width;
            string en = "Drive D: has 3 GB free; this install needs about 30 GB. Free some space or choose a folder on another drive.";
            int w = width(f, en) / 2 + 7;
            var rows = StatusView.Wrap(en, f, w);
            Expect("wrap: two or three rows", rows.Count >= 2 && rows.Count <= 3, rows.Count.ToString());
            Expect("wrap: each row fits", rows.All(r => width(f, r) <= w));
            Expect("wrap: the words, in order", string.Join(" ", rows) == en, string.Join("|", rows));
            Expect("wrap: a short line is one row", StatusView.Wrap("step 3/9 romfs", f, w).Count == 1);
            Expect("wrap: an empty line is one row", StatusView.Wrap("", f, w).SequenceEqual(new[] { "" }));
            string path = @"C:\Users\Someone\AppData\Local\Temp\a-very-long-folder-name-without-any-space\and-another-one\file.zip";
            var p = StatusView.Wrap("removed " + path, f, w);
            Expect("wrap: a long path breaks at the edge", p.Count >= 2 && p.All(r => width(f, r) <= w)
                   && ("removed " + path).Replace(" ", "") == string.Concat(p).Replace(" ", ""), string.Join("|", p));
            string cn = "游戏转储不是《王国之泪》本体 1.0.0 版：请选择本体的 .nsp / .xci。更新放在“更新”一行，DLC 不会被使用。";
            var c = StatusView.Wrap(cn, zh, width(zh, cn) / 3 + 5);
            Expect("wrap: Chinese breaks where it meets the edge", c.Count >= 3 && c.All(r => width(zh, r) <= width(zh, cn) / 3 + 5)
                   && cn.Replace(" ", "") == string.Concat(c).Replace(" ", ""), string.Join("|", c));
        }
        using (var zh = new Font("Microsoft YaHei UI", 8.5f))
        {
            string ext = string.Concat(Enumerable.Repeat("\U00020000\U0002A700", 30));   // (CJK Ext-B / Ext-C: surrogate pairs)
            var x = StatusView.Wrap(ext, zh, 61);
            Expect("wrap: no surrogate pair split", x.Count >= 2 && string.Concat(x) == ext
                   && x.All(r => !char.IsHighSurrogate(r[r.Length - 1]) && !char.IsLowSurrogate(r[0])), x.Count.ToString());
        }
        Func<string, List<string>> two =s => s.StartsWith("long") ? new List<string> { s + "1", s + "2", s + "3" } : new List<string> { s };
        var shown = StatusView.Shown(new List<string> { "a", "longA", "b", "longB" }, two, 5);
        Expect("shown: the newest lines that fit whole", shown.SequenceEqual(new[] { "b", "longB1", "longB2", "longB3" }), string.Join("|", shown));
        shown = StatusView.Shown(new List<string> { "a", "b" }, two, 5);
        Expect("shown: all when they fit", shown.SequenceEqual(new[] { "a", "b" }), string.Join("|", shown));
        shown = StatusView.Shown(new List<string> { "a", "longC" }, two, 2);
        Expect("shown: a newest line over the rows shows its start", shown.SequenceEqual(new[] { "longC1", "longC2 longC3" }), string.Join("|", shown));
    }

    static int Main(string[] args)
    {
        Wraps();
        Expect("no Use: English", L.Code == "en" && L.S("Install") == "Install");
        var objs = new List<string>();
        foreach (var code in args.Skip(1))
        {
            Dictionary<string, string> table;
            using (var s = typeof(L).Assembly.GetManifestResourceStream("text." + code + ".json"))
            using (var r = new StreamReader(s, Encoding.UTF8)) table = L.Parse(r.ReadToEnd());
            L.Use(code);
            Expect(code + ": Use", L.Code == code, L.Code);
            var vals = new object[] { "A", "B", "C", "D", "E", "F" };
            foreach (var kv in table)
            {
                Expect(code + ": S", L.S(kv.Key) == kv.Value, kv.Key);
                try { L.S(kv.Key, vals); } catch (FormatException) { Expect(code + ": formats", false, kv.Value); }
            }
            objs.Add(Json(code) + ": {" + string.Join(", ", table.Select(kv => Json(kv.Key) + ": " + Json(kv.Value))) + "}");
        }
        foreach (var code in new[] { "en" }.Concat(args.Skip(1))) { L.Use(code); Fits(code); }
        L.Use("en");
        Expect("Use en", L.Code == "en" && L.S("Install") == "Install");
        L.Use("xx");
        Expect("Use of a language without a file: English", L.Code == "en" && L.S("Install") == "Install");
        File.WriteAllText(args[0], "{" + string.Join(", ", objs) + "}", new UTF8Encoding(false));

        var none = new string[0];
        foreach (var c in new[] { new[] { "ru-RU", "ru" }, new[] { "pt-BR", "pt-BR" }, new[] { "pt-PT", "pt-BR" }, new[] { "zh-CN", "zh-Hans" },
                                  new[] { "zh-TW", "zh-Hans" }, new[] { "es-ES", "es" }, new[] { "es-MX", "es" }, new[] { "en-US", "en" },
                                  new[] { "de-DE", "en" }, new[] { "ja-JP", "en" } })
            Expect("Pick " + c[0], L.Pick(none, new CultureInfo(c[0])) == c[1], L.Pick(none, new CultureInfo(c[0])));
        Expect("Pick --lang=ru", L.Pick(new[] { "x.nsp", "--lang=ru" }, new CultureInfo("en-US")) == "ru");
        Expect("Pick --lang=zh-hans", L.Pick(new[] { "--lang=zh-hans" }, new CultureInfo("en-US")) == "zh-Hans");
        Expect("Pick --lang=en", L.Pick(new[] { "--lang=en" }, new CultureInfo("ru-RU")) == "en");
        Expect("Pick --lang=xx", L.Pick(new[] { "--lang=xx" }, new CultureInfo("ru-RU")) == "en");
        Expect("Parse escapes", L.Parse("{ \"a\\\"b\" : \"c\\nd\\u00e9\\\\\" , \"e\":\"\" }")["a\"b"] == "c\ndé\\");
        Console.WriteLine(fails == 0 ? "text: languages read, format and pick" : "text: " + fails + " FAIL");
        return fails == 0 ? 0 : 1;
    }
}
