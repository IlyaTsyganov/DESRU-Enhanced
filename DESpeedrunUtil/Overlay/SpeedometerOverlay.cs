using System.Runtime.InteropServices;

namespace DESpeedrunUtil.Overlay {
    internal readonly record struct SpeedometerReading(bool Enabled, int X, int Y, float? Speed, bool Supported = true, double Scale = 1, Color TextColor = default) {
        internal Color DisplayColor => TextColor.IsEmpty ? Color.White : Color.FromArgb(TextColor.R, TextColor.G, TextColor.B);
    }

    internal sealed class SpeedometerOverlay : Form {
        private readonly Font _font = new("Segoe UI", 22, FontStyle.Bold);
        private SpeedometerReading _current, _landing;

        public SpeedometerOverlay() {
            FormBorderStyle = FormBorderStyle.None;
            ShowInTaskbar = false;
            StartPosition = FormStartPosition.Manual;
            TopMost = true;
            BackColor = TransparencyKey = Color.Magenta;
            ClientSize = new Size(420, 48);
            AutoScaleMode = AutoScaleMode.None;
            DoubleBuffered = true;
            AccessibleName = "Horizontal speed and last landing";
        }

        protected override bool ShowWithoutActivation => true;
        protected override CreateParams CreateParams {
            get {
                var cp = base.CreateParams;
                // Layered, click-through, tool window, and never activate.
                cp.ExStyle |= 0x00080000 | 0x00000020 | 0x00000080 | 0x08000000;
                return cp;
            }
        }

        public void UpdateDisplay(Rectangle viewport, SpeedometerReading current, SpeedometerReading landing) {
            // One layered surface keeps both meters in the same desktop composition layer.
            // Each number has its own position and dirty rectangle.
            bool resized = Bounds != viewport;
            Color key = new[] { Color.Magenta, Color.Cyan, Color.Lime }.First(c => c.ToArgb() != current.DisplayColor.ToArgb() && c.ToArgb() != landing.DisplayColor.ToArgb());
            if(TransparencyKey != key) { BackColor = TransparencyKey = key; resized = true; }
            Bounds = viewport;
            if(resized || current != _current || landing != _landing) {
                var dirty = Rectangle.Union(ReadingBounds(ClientSize, _current), ReadingBounds(ClientSize, _landing));
                dirty = Rectangle.Union(dirty, ReadingBounds(ClientSize, current));
                dirty = Rectangle.Union(dirty, ReadingBounds(ClientSize, landing));
                _current = current;
                _landing = landing;
                if(resized) Invalidate();
                else Invalidate(dirty);
            }
            if(!current.Enabled && !landing.Enabled) {
                Hide();
                return;
            }
            if(!Visible) {
                Show();
                Invalidate();
            }
        }

        internal static Rectangle ReadingBounds(Size viewport, SpeedometerReading reading, string? text = null) {
            using var font = new Font("Segoe UI", (float) (22 * Math.Clamp(reading.Scale, 0.1, 10)), FontStyle.Bold);
            var size = TextRenderer.MeasureText(text ?? ReadingText("", reading), font);
            int width = Math.Min(size.Width + 4, viewport.Width), height = Math.Min(size.Height + 4, viewport.Height);
            return new Rectangle(Math.Clamp(reading.X, 0, Math.Max(0, viewport.Width - width)),
                Math.Clamp(reading.Y, 0, Math.Max(0, viewport.Height - height)), width, height);
        }

        internal static string ReadingText(string caption, SpeedometerReading reading) {
            return SpeedometerText.Format(reading);
        }

        protected override void OnPaint(PaintEventArgs e) {
            base.OnPaint(e);
            DrawReading(e.Graphics, _current, SpeedometerText.Format(_current));
            DrawReading(e.Graphics, _landing, SpeedometerText.Format(_landing));
        }

        private void DrawReading(Graphics graphics, SpeedometerReading reading, string text) {
            if(!reading.Enabled) return;
            var bounds = ReadingBounds(ClientSize, reading, text);
            using var font = new Font(_font.FontFamily, (float) (_font.Size * Math.Clamp(reading.Scale, 0.1, 10)), FontStyle.Bold);
            TextRenderer.DrawText(graphics, text, font, new Point(bounds.X + 3, bounds.Y + 3), Color.Black, TextFormatFlags.NoPadding);
            TextRenderer.DrawText(graphics, text, font, new Point(bounds.X + 1, bounds.Y + 1), reading.DisplayColor, TextFormatFlags.NoPadding);
        }

        protected override void Dispose(bool disposing) {
            if(disposing) _font.Dispose();
            base.Dispose(disposing);
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeRect { public int Left, Top, Right, Bottom; }
        [StructLayout(LayoutKind.Sequential)]
        private struct NativePoint { public int X, Y; }
        [DllImport("user32.dll")]
        private static extern bool GetClientRect(IntPtr window, out NativeRect rect);
        [DllImport("user32.dll")]
        private static extern bool ClientToScreen(IntPtr window, ref NativePoint point);
        [DllImport("user32.dll")]
        private static extern bool IsIconic(IntPtr window);
        [DllImport("user32.dll")]
        private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);

        internal static bool TryGetPhysicalViewport(IntPtr window, out Rectangle viewport) {
            // DESRU's existing UI is DPI-unaware. Scope awareness to these API calls so
            // Windows cannot virtualize the pixel dimensions used by the native HUD.
            IntPtr previous = SetThreadDpiAwarenessContext(new IntPtr(-4));
            try { return TryGetViewport(window, out viewport); }
            finally { if(previous != IntPtr.Zero) SetThreadDpiAwarenessContext(previous); }
        }

        internal static bool TryGetViewport(IntPtr window, out Rectangle viewport) {
            viewport = Rectangle.Empty;
            if(window == IntPtr.Zero || IsIconic(window) || !GetClientRect(window, out var rect)) return false;
            var origin = new NativePoint();
            if(!ClientToScreen(window, ref origin)) return false;
            viewport = new Rectangle(origin.X, origin.Y, rect.Right - rect.Left, rect.Bottom - rect.Top);
            return viewport.Width > 0 && viewport.Height > 0;
        }
    }
}
