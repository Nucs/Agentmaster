// Agentmaster: thumbprobe - the taskbar-thumbnail survey probe behind doc/agentmaster/NOTIFICATIONS.md section 9, run on a
// DISPOSABLE Windows 11 VM (2026-10-06), never on a desktop someone works on (it replaces window previews and logs to C:\t).
// Question: which per-window cues show on a window's preview in the Windows 11 taskbar hover flyout, and can an
// "iconic thumbnail" (our own bitmap, here: a capture with a red frame) replace the live preview?
// Build (C# 5, inbox csc): csc /nologo /target:winexe /out:C:\t\thumbprobe.exe /r:System.Windows.Forms.dll /r:System.Drawing.dll Z:\thumbprobe.cs
// Run:   thumbprobe.exe A [invMs]   -> 1 plain, 2 flash, 3 progress error, 4 overlay
//        thumbprobe.exe B [invMs]   -> 5 emoji title, 6 icon swap, 7 thumbbar, 8 iconic (self), 9 iconic (Windows Terminal)
//        thumbprobe.exe cap         -> PrintWindow captures of the Windows Terminal window -> C:\t\cap_<flags>.png
// Every DWM request and capture is logged to C:\t\probe.log.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

static class Log
{
    public static void W(string s)
    {
        try { File.AppendAllText(@"C:\t\probe.log", DateTime.Now.ToString("HH:mm:ss.fff") + " " + s + "\r\n"); }
        catch { }
    }
}

static class N
{
    [DllImport("shell32.dll")] public static extern int SetCurrentProcessExplicitAppUserModelID([MarshalAs(UnmanagedType.LPWStr)] string id);
    [DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
    [DllImport("dwmapi.dll")] public static extern int DwmSetIconicThumbnail(IntPtr hwnd, IntPtr hbmp, uint flags);
    [DllImport("dwmapi.dll")] public static extern int DwmSetIconicLivePreviewBitmap(IntPtr hwnd, IntPtr hbmp, IntPtr ppt, uint flags);
    [DllImport("dwmapi.dll")] public static extern int DwmInvalidateIconicBitmaps(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool FlashWindowEx(ref FLASHWINFO fi);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct FLASHWINFO { public uint cbSize; public IntPtr hwnd; public uint dwFlags; public uint uCount; public uint dwTimeout; }
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
struct THUMBBUTTON
{
    public uint dwMask;
    public uint iId;
    public uint iBitmap;
    public IntPtr hIcon;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szTip;
    public uint dwFlags;
}

[ComImport, Guid("ea1afb91-9e28-4b86-90e9-9e9f8a5eefaf"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface ITaskbarList3
{
    void HrInit();
    void AddTab(IntPtr hwnd);
    void DeleteTab(IntPtr hwnd);
    void ActivateTab(IntPtr hwnd);
    void SetActiveAlt(IntPtr hwnd);
    void MarkFullscreenWindow(IntPtr hwnd, [MarshalAs(UnmanagedType.Bool)] bool fFullscreen);
    void SetProgressValue(IntPtr hwnd, ulong ullCompleted, ulong ullTotal);
    void SetProgressState(IntPtr hwnd, int tbpFlags);
    void RegisterTab(IntPtr hwndTab, IntPtr hwndMDI);
    void UnregisterTab(IntPtr hwndTab);
    void SetTabOrder(IntPtr hwndTab, IntPtr hwndInsertBefore);
    void SetTabActive(IntPtr hwndTab, IntPtr hwndMDI, uint dwReserved);
    [PreserveSig] int ThumbBarAddButtons(IntPtr hwnd, uint cButtons, [MarshalAs(UnmanagedType.LPArray)] THUMBBUTTON[] pButtons);
    [PreserveSig] int ThumbBarUpdateButtons(IntPtr hwnd, uint cButtons, [MarshalAs(UnmanagedType.LPArray)] THUMBBUTTON[] pButtons);
    void ThumbBarSetImageList(IntPtr hwnd, IntPtr himl);
    void SetOverlayIcon(IntPtr hwnd, IntPtr hIcon, [MarshalAs(UnmanagedType.LPWStr)] string pszDescription);
    void SetThumbnailTooltip(IntPtr hwnd, [MarshalAs(UnmanagedType.LPWStr)] string pszTip);
    void SetThumbnailClip(IntPtr hwnd, IntPtr prcClip);
}

[ComImport, Guid("56FDF344-FD6D-11d0-958A-006097C9A090"), ClassInterface(ClassInterfaceType.None)]
class CTaskbarList { }

static class Img
{
    public static readonly Color Red = Color.FromArgb(255, 232, 17, 35);

    public static Icon RedDot(int size)
    {
        var b = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(b))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Color.Transparent);
            using (var br = new SolidBrush(Red)) { g.FillEllipse(br, 1, 1, size - 2, size - 2); }
        }
        return Icon.FromHandle(b.GetHicon());
    }

    public static Icon Ringed(int size)
    {
        var b = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(b))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Color.Transparent);
            g.DrawIcon(SystemIcons.Application, new Rectangle(size / 6, size / 6, size - size / 3, size - size / 3));
            using (var p = new Pen(Red, Math.Max(2, size / 8))) { g.DrawEllipse(p, 1, 1, size - 3, size - 3); }
        }
        return Icon.FromHandle(b.GetHicon());
    }

    public static Bitmap Capture(IntPtr hwnd, uint flags, out long ms)
    {
        var sw = Stopwatch.StartNew();
        N.RECT r;
        if ((flags & 1) != 0) { N.GetClientRect(hwnd, out r); } else { N.GetWindowRect(hwnd, out r); }
        int w = Math.Max(1, r.Right - r.Left), h = Math.Max(1, r.Bottom - r.Top);
        var bmp = new Bitmap(w, h, PixelFormat.Format32bppRgb);
        using (var g = Graphics.FromImage(bmp))
        {
            IntPtr hdc = g.GetHdc();
            bool ok = N.PrintWindow(hwnd, hdc, flags);
            g.ReleaseHdc(hdc);
            if (!ok) { Log.W("PrintWindow FAILED flags=" + flags + " err=" + Marshal.GetLastWin32Error()); }
        }
        ms = sw.ElapsedMilliseconds;
        return bmp;
    }

    public static double NonBlackPct(Bitmap b)
    {
        int hits = 0, total = 0;
        int sy = Math.Max(1, b.Height / 60), sx = Math.Max(1, b.Width / 80);
        for (int y = 0; y < b.Height; y += sy)
        {
            for (int x = 0; x < b.Width; x += sx)
            {
                var c = b.GetPixel(x, y);
                total++;
                if (c.R > 16 || c.G > 16 || c.B > 16) { hits++; }
            }
        }
        return total == 0 ? 0 : 100.0 * hits / total;
    }

    public static Bitmap FitAndFrame(Bitmap src, int maxW, int maxH, bool frame)
    {
        double s = Math.Min((double)maxW / src.Width, (double)maxH / src.Height);
        int w = Math.Max(1, (int)(src.Width * s)), h = Math.Max(1, (int)(src.Height * s));
        var dst = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(dst))
        {
            g.Clear(Color.Black);
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.DrawImage(src, new Rectangle(0, 0, w, h));
            if (frame)
            {
                int t = Math.Max(3, Math.Min(w, h) / 30);
                using (var p = new Pen(Red, t)) { p.Alignment = PenAlignment.Inset; g.DrawRectangle(p, 0, 0, w - 1, h - 1); }
            }
        }
        return dst;
    }
}

class ProbeForm : Form
{
    static readonly uint WM_TBC = N.RegisterWindowMessage("TaskbarButtonCreated");
    public static bool PreLive;
    public static bool NoPeek;
    public static bool Push;
    public static bool PushInvalidate;
    public static bool Worker;
    public static int PushW = 200, PushH = 108;
    readonly string _mode;
    readonly int _invMs;
    ITaskbarList3 _tb;
    bool _iconic;
    Timer _inv;
    int _requests;

    public ProbeForm(string mode, string title, Color back, int invMs)
    {
        _mode = mode;
        _invMs = invMs;
        Text = title;
        BackColor = back;
        Width = 720;
        Height = 420;
        KeyPreview = true;
        var lbl = new Label();
        lbl.Text = title + "\r\n(" + mode + ")";
        lbl.Dock = DockStyle.Fill;
        lbl.Font = new Font("Segoe UI", 30);
        lbl.ForeColor = Color.White;
        lbl.TextAlign = ContentAlignment.MiddleCenter;
        Controls.Add(lbl);
        KeyDown += (s, e) => { if (e.KeyCode == Keys.T && _mode.StartsWith("iconic")) { SetIconic(!_iconic); } };
        if (mode.StartsWith("iconic"))
        {
            // A visible seconds counter, so a stale preview (taskbar, Alt+Tab, peek) is obvious in a screenshot.
            int n = 0;
            var tick = new Timer();
            tick.Interval = 1000;
            tick.Tick += (s, e) => { n++; lbl.Text = title + "\r\nt=" + n; };
            tick.Start();
        }
    }

    void SetIconic(bool on)
    {
        _iconic = on;
        int v = on ? 1 : 0;
        int h1 = N.DwmSetWindowAttribute(Handle, 7, ref v, 4);   // DWMWA_FORCE_ICONIC_REPRESENTATION
        int h2 = N.DwmSetWindowAttribute(Handle, 10, ref v, 4);  // DWMWA_HAS_ICONIC_BITMAP
        int h3 = N.DwmInvalidateIconicBitmaps(Handle);
        int h4 = 0;
        if (NoPeek) { h4 = N.DwmSetWindowAttribute(Handle, 11, ref v, 4); } // DWMWA_DISALLOW_PEEK while iconic
        Log.W(Text + ": iconic=" + on + " hr=" + h1.ToString("X") + "/" + h2.ToString("X") + "/" + h3.ToString("X") + " nopeek=" + NoPeek + "/" + h4.ToString("X"));
        if (on && Push) { PushThumb("on"); }
        if (on && _invMs > 0)
        {
            if (_inv == null)
            {
                _inv = new Timer();
                _inv.Interval = _invMs;
                _inv.Tick += (s, e) =>
                {
                    if (Worker)
                    {
                        // Experiment: capture + invalidate + push on a pool thread (the UI thread only arms it).
                        IntPtr me = Handle, target = CaptureTarget();
                        if (N.IsIconic(target)) { return; } // nothing to capture; keep the cached preview
                        int w = _lastMaxW, h = _lastMaxH;
                        string name = Text;
                        System.Threading.ThreadPool.QueueUserWorkItem(_ =>
                        {
                            long ms;
                            using (var cap = Img.Capture(target, 1 | 2, out ms))
                            using (var th = Img.FitAndFrame(cap, w, h, true))
                            {
                                IntPtr hb = th.GetHbitmap(Color.FromArgb(0, 0, 0, 0));
                                int hi = N.DwmInvalidateIconicBitmaps(me);
                                int hr = N.DwmSetIconicThumbnail(me, hb, 0);
                                N.DeleteObject(hb);
                                Log.W(string.Format("{0}: WORKER push tid={1} cap={2}x{3} {4}ms nonblack={5:F1}% inv={6:X} hr={7:X}",
                                    name, System.Threading.Thread.CurrentThread.ManagedThreadId, cap.Width, cap.Height, ms, Img.NonBlackPct(cap), hi, hr));
                            }
                        });
                        return;
                    }
                    if (Push)
                    {
                        if (PushInvalidate) { N.DwmInvalidateIconicBitmaps(Handle); } // drop the stale peek bitmap too...
                        PushThumb("timer");                                           // ...and refill the thumbnail at once
                    }
                    else { N.DwmInvalidateIconicBitmaps(Handle); }
                };
            }
            _inv.Start();
        }
        else if (_inv != null) { _inv.Stop(); }
    }

    // Experiment: hand DWM a thumbnail WITHOUT a pending WM_DWMSENDICONICTHUMBNAIL, at the last requested size.
    int _lastMaxW = PushW, _lastMaxH = PushH;
    void PushThumb(string why)
    {
        if (N.IsIconic(Handle)) { Log.W(Text + ": push(" + why + ") skipped (minimized)"); return; }
        long ms;
        using (var cap = Img.Capture(CaptureTarget(), 1 | 2, out ms))
        using (var th = Img.FitAndFrame(cap, _lastMaxW, _lastMaxH, true))
        {
            IntPtr hb = th.GetHbitmap(Color.FromArgb(0, 0, 0, 0));
            int hr = N.DwmSetIconicThumbnail(Handle, hb, 0);
            N.DeleteObject(hb);
            Log.W(string.Format("{0}: PUSH({1}) {2}x{3} {4}ms hr={5:X}", Text, why, th.Width, th.Height, ms, hr));
        }
    }

    IntPtr CaptureTarget()
    {
        if (_mode == "iconic-wt")
        {
            IntPtr wt = N.FindWindow("CASCADIA_HOSTING_WINDOW_CLASS", null);
            if (wt != IntPtr.Zero) { return wt; }
        }
        return Handle;
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        if (_mode.StartsWith("iconic")) { SetIconic(true); }
        if (_mode == "icon")
        {
            N.SendMessage(Handle, 0x80, (IntPtr)0, Img.Ringed(16).Handle); // WM_SETICON ICON_SMALL
            N.SendMessage(Handle, 0x80, (IntPtr)1, Img.Ringed(32).Handle); // WM_SETICON ICON_BIG
            Log.W(Text + ": icon swapped");
        }
        if (_mode == "flash")
        {
            var t = new Timer();
            t.Interval = 2500;
            t.Tick += (s, a) =>
            {
                t.Stop();
                var fi = new N.FLASHWINFO();
                fi.cbSize = (uint)Marshal.SizeOf(typeof(N.FLASHWINFO));
                fi.hwnd = Handle;
                fi.dwFlags = 0x2 | 0xC; // FLASHW_TRAY | FLASHW_TIMERNOFG
                bool r = N.FlashWindowEx(ref fi);
                Log.W(Text + ": FlashWindowEx -> " + r);
            };
            t.Start();
        }
    }

    void OnButtonCreated()
    {
        try
        {
            if (_tb == null)
            {
                _tb = (ITaskbarList3)new CTaskbarList();
                _tb.HrInit();
            }
            if (_mode == "progress")
            {
                _tb.SetProgressState(Handle, 4); // TBPF_ERROR
                _tb.SetProgressValue(Handle, 100, 100);
                Log.W(Text + ": progress error set");
            }
            if (_mode == "overlay")
            {
                _tb.SetOverlayIcon(Handle, Img.RedDot(16).Handle, "unread");
                Log.W(Text + ": overlay set");
            }
            if (_mode == "thumbbar")
            {
                var b = new THUMBBUTTON[2];
                b[0].dwMask = 2 | 4 | 8; b[0].iId = 1; b[0].hIcon = Img.RedDot(16).Handle; b[0].szTip = "unread (normal)"; b[0].dwFlags = 0;
                b[1].dwMask = 2 | 4 | 8; b[1].iId = 2; b[1].hIcon = Img.RedDot(16).Handle; b[1].szTip = "unread (noninteractive)"; b[1].dwFlags = 0x10 | 0x4;
                int hr = _tb.ThumbBarAddButtons(Handle, 2, b);
                Log.W(Text + ": thumbbar hr=" + hr.ToString("X"));
            }
        }
        catch (Exception ex) { Log.W(Text + ": taskbar call threw " + ex.Message); }
    }

    protected override void WndProc(ref Message m)
    {
        if (WM_TBC != 0 && m.Msg == (int)WM_TBC) { OnButtonCreated(); }
        if (_iconic && m.Msg == 0x0323) // WM_DWMSENDICONICTHUMBNAIL
        {
            long lp = m.LParam.ToInt64();
            int maxW = (int)((lp >> 16) & 0xFFFF), maxH = (int)(lp & 0xFFFF);
            _lastMaxW = maxW;
            _lastMaxH = maxH;
            _requests++;
            IntPtr target = CaptureTarget();
            if (N.IsIconic(target))
            {
                Log.W(Text + ": THUMB req while minimized -> not answered");
                m.Result = IntPtr.Zero;
                return;
            }
            long ms;
            using (var cap = Img.Capture(target, 1 | 2, out ms))
            {
                double nb = Img.NonBlackPct(cap);
                using (var th = Img.FitAndFrame(cap, maxW, maxH, true))
                {
                    IntPtr hb = th.GetHbitmap(Color.FromArgb(0, 0, 0, 0));
                    int hr = N.DwmSetIconicThumbnail(Handle, hb, 0);
                    N.DeleteObject(hb);
                    Log.W(string.Format("{0}: THUMB req#{1} max={2}x{3} target={4} cap={5}x{6} {7}ms nonblack={8:F1}% sent={9}x{10} hr={11:X} minimized={12}",
                        Text, _requests, maxW, maxH, target == Handle ? "self" : "wt", cap.Width, cap.Height, ms, nb, th.Width, th.Height, hr, N.IsIconic(Handle)));
                }
                if (PreLive)
                {
                    // Experiment: also hand DWM the full-size live-preview bitmap with every thumbnail answer.
                    using (var self = Img.Capture(Handle, 1 | 2, out ms))
                    using (var full = Img.FitAndFrame(self, self.Width, self.Height, false))
                    {
                        IntPtr hb2 = full.GetHbitmap(Color.FromArgb(0, 0, 0, 0));
                        int hr2 = N.DwmSetIconicLivePreviewBitmap(Handle, hb2, IntPtr.Zero, 0);
                        N.DeleteObject(hb2);
                        Log.W(string.Format("{0}: prelive set {1}x{2} hr={3:X}", Text, self.Width, self.Height, hr2));
                    }
                }
            }
            m.Result = IntPtr.Zero;
            return;
        }
        if (_iconic && m.Msg == 0x0326 && N.IsIconic(Handle))
        {
            Log.W(Text + ": PEEK req while minimized -> not answered (DefWindowProc)");
            base.WndProc(ref m);
            return;
        }
        if (_iconic && m.Msg == 0x0326) // WM_DWMSENDICONICLIVEPREVIEWBITMAP
        {
            long ms;
            using (var cap = Img.Capture(Handle, 1 | 2, out ms))
            using (var full = Img.FitAndFrame(cap, cap.Width, cap.Height, false))
            {
                IntPtr hb = full.GetHbitmap(Color.FromArgb(0, 0, 0, 0));
                int hr = N.DwmSetIconicLivePreviewBitmap(Handle, hb, IntPtr.Zero, 0);
                N.DeleteObject(hb);
                Log.W(string.Format("{0}: PEEK req cap={1}x{2} {3}ms hr={4:X}", Text, cap.Width, cap.Height, ms, hr));
            }
            m.Result = IntPtr.Zero;
            return;
        }
        base.WndProc(ref m);
    }
}

static class Program
{
    static void CapTest()
    {
        IntPtr wt = N.FindWindow("CASCADIA_HOSTING_WINDOW_CLASS", null);
        Log.W("cap: wt hwnd=0x" + wt.ToInt64().ToString("X") + " minimized=" + (wt != IntPtr.Zero && N.IsIconic(wt)));
        if (wt == IntPtr.Zero) { return; }
        foreach (uint f in new uint[] { 0, 1, 2, 3 })
        {
            long ms;
            using (var b = Img.Capture(wt, f, out ms))
            {
                b.Save(@"C:\t\cap_" + f + ".png", ImageFormat.Png);
                Log.W(string.Format("cap flags={0} size={1}x{2} {3}ms nonblack={4:F1}%", f, b.Width, b.Height, ms, Img.NonBlackPct(b)));
            }
        }
    }

    [STAThread]
    static void Main(string[] args)
    {
        Directory.CreateDirectory(@"C:\t");
        string group = args.Length > 0 ? args[0] : "A";
        int invMs = args.Length > 1 ? int.Parse(args[1]) : 1000;
        ProbeForm.PreLive = args.Length > 2 && args[2] == "prelive";
        ProbeForm.NoPeek = args.Length > 2 && args[2] == "nopeek";
        ProbeForm.Push = args.Length > 2 && (args[2] == "push" || args[2] == "pushinv" || args[2] == "worker");
        ProbeForm.Worker = args.Length > 2 && args[2] == "worker";
        ProbeForm.PushInvalidate = args.Length > 2 && args[2] == "pushinv";
        if (args.Length > 3)
        {
            var wh = args[3].Split('x');
            ProbeForm.PushW = int.Parse(wh[0]);
            ProbeForm.PushH = int.Parse(wh[1]);
        }
        if (group == "cap") { CapTest(); return; }
        N.SetCurrentProcessExplicitAppUserModelID("ClaudeProbe.Thumbs." + group);
        Application.EnableVisualStyles();
        var forms = new List<ProbeForm>();
        if (group == "A")
        {
            forms.Add(new ProbeForm("plain", "1 plain", Color.FromArgb(45, 45, 48), invMs));
            forms.Add(new ProbeForm("flash", "2 flash", Color.FromArgb(30, 60, 90), invMs));
            forms.Add(new ProbeForm("progress", "3 progress error", Color.FromArgb(40, 80, 40), invMs));
            forms.Add(new ProbeForm("overlay", "4 overlay", Color.FromArgb(90, 60, 20), invMs));
        }
        else
        {
            forms.Add(new ProbeForm("emoji", "5 \U0001F534 emoji title", Color.FromArgb(45, 45, 48), invMs));
            forms.Add(new ProbeForm("icon", "6 icon swap", Color.FromArgb(30, 60, 90), invMs));
            forms.Add(new ProbeForm("thumbbar", "7 thumbbar", Color.FromArgb(40, 80, 40), invMs));
            forms.Add(new ProbeForm("iconic", "8 iconic self", Color.FromArgb(90, 60, 20), invMs));
            forms.Add(new ProbeForm("iconic-wt", "9 iconic WT", Color.FromArgb(70, 30, 70), invMs));
        }
        int baseX = group == "A" ? 40 : 700;
        for (int i = 0; i < forms.Count; i++)
        {
            forms[i].StartPosition = FormStartPosition.Manual;
            forms[i].Location = new Point(baseX + i * 50, 40 + i * 50);
            forms[i].Show();
        }
        Application.Run();
    }
}
