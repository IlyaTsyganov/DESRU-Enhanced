using static DESpeedrunUtil.Define.Constants;

namespace DESpeedrunUtil.Controls {
    internal sealed class DesruTextScrollBar : Control {
        private const int ArrowHeight=18;
        private readonly HelpScrollTextBox _text;
        private readonly System.Windows.Forms.Timer _repeat=new();
        private int _first,_total=1,_page=1,_dragOffset=-1,_pressedZone;
        private Point _mouse=new(-1,-1);
        internal int Maximum => Math.Max(0,_total-_page);
        internal int Value => _first;
        private Rectangle Track => new(1,ArrowHeight,Math.Max(1,Width-2),Math.Max(1,Height-2*ArrowHeight));
        internal Rectangle Thumb {
            get {
                Rectangle track=Track;
                int height=Math.Min(track.Height,Math.Max(24,(int)((long)track.Height*_page/Math.Max(_page,_total))));
                int y=track.Top+(Maximum==0?0:(int)Math.Round((double)_first*(track.Height-height)/Maximum));
                return new Rectangle(track.Left,y,track.Width,height);
            }
        }
        internal DesruTextScrollBar(HelpScrollTextBox text) {
            _text=text;Name="helpScrollBar";Width=18;TabStop=true;AccessibleRole=AccessibleRole.ScrollBar;
            AccessibleName="Help text scroll position";BackColor=COLOR_FORM_BACK;ForeColor=COLOR_TEXT_FORE;
            SetStyle(ControlStyles.UserPaint|ControlStyles.AllPaintingInWmPaint|ControlStyles.OptimizedDoubleBuffer|ControlStyles.ResizeRedraw|ControlStyles.Selectable,true);
            _text.ViewportChanged+=TextViewportChanged;
            _repeat.Tick+=(_,_)=> { _repeat.Interval=45;ScrollPressedZone(); };
        }
        internal void UpdateViewport() {
            _total=_text.TotalLines;_page=_text.PageLines;_first=Math.Clamp(_text.FirstLine,0,Maximum);
            Enabled=Maximum>0;Invalidate();
        }
        private void TextViewportChanged(object? sender,EventArgs e) => UpdateViewport();
        private void ScrollPressedZone() {
            if(_mouse.X<0||_mouse.X>=Width||_mouse.Y<0||_mouse.Y>=Height) return;
            if(_pressedZone==-1&&_mouse.Y<ArrowHeight) _text.ScrollToLine(_first-1);
            else if(_pressedZone==1&&_mouse.Y>=Height-ArrowHeight) _text.ScrollToLine(_first+1);
            else if(_pressedZone==-2&&_mouse.Y<Thumb.Top) _text.ScrollToLine(_first-_page);
            else if(_pressedZone==2&&_mouse.Y>=Thumb.Bottom) _text.ScrollToLine(_first+_page);
        }
        protected override void OnPaint(PaintEventArgs e) {
            base.OnPaint(e);
            using var frame=new Pen(SystemColors.WindowFrame);
            using var surface=new SolidBrush(COLOR_TEXT_BACK);
            using var hover=new SolidBrush(Color.FromArgb(_dragOffset>=0?105:90,_dragOffset>=0?105:90,_dragOffset>=0?105:90));
            using var glyph=new SolidBrush(Enabled?COLOR_TEXT_FORE:SystemColors.WindowFrame);
            e.Graphics.FillRectangle(surface,1,1,Width-2,ArrowHeight-1);
            e.Graphics.FillRectangle(surface,1,Height-ArrowHeight,Width-2,ArrowHeight-1);
            if(Enabled&&_mouse.Y>=0&&_mouse.Y<ArrowHeight) e.Graphics.FillRectangle(hover,1,1,Width-2,ArrowHeight-1);
            if(Enabled&&_mouse.Y>=Height-ArrowHeight&&_mouse.Y<Height) e.Graphics.FillRectangle(hover,1,Height-ArrowHeight,Width-2,ArrowHeight-1);
            if(Enabled) {
                Rectangle thumb=Thumb;
                e.Graphics.FillRectangle(_dragOffset>=0||thumb.Contains(_mouse)?hover:surface,thumb);
                e.Graphics.DrawRectangle(frame,thumb.X,thumb.Y,thumb.Width-1,thumb.Height-1);
                for(int y=-3;y<=3;y+=3) e.Graphics.DrawLine(frame,thumb.Left+4,thumb.Top+thumb.Height/2+y,thumb.Right-4,thumb.Top+thumb.Height/2+y);
            }
            int cx=Width/2;
            e.Graphics.FillPolygon(glyph,new[]{new Point(cx,6),new Point(cx-4,11),new Point(cx+4,11)});
            e.Graphics.FillPolygon(glyph,new[]{new Point(cx,Height-6),new Point(cx-4,Height-11),new Point(cx+4,Height-11)});
            e.Graphics.DrawRectangle(frame,0,0,Width-1,Height-1);
            e.Graphics.DrawLine(frame,0,ArrowHeight-1,Width-1,ArrowHeight-1);
            e.Graphics.DrawLine(frame,0,Height-ArrowHeight,Width-1,Height-ArrowHeight);
            if(Focused) ControlPaint.DrawFocusRectangle(e.Graphics,new Rectangle(2,2,Width-4,Height-4),ForeColor,BackColor);
        }
        protected override void OnMouseDown(MouseEventArgs e) {
            base.OnMouseDown(e);if(e.Button!=MouseButtons.Left||!Enabled) return;
            Focus();_mouse=e.Location;Capture=true;
            Rectangle thumb=Thumb;
            if(thumb.Contains(e.Location)) _dragOffset=e.Y-thumb.Top;
            else {
                _pressedZone=e.Y<ArrowHeight?-1:e.Y>=Height-ArrowHeight?1:e.Y<thumb.Top?-2:2;
                ScrollPressedZone();_repeat.Interval=400;_repeat.Start();
            }
            Invalidate();
        }
        protected override void OnMouseMove(MouseEventArgs e) {
            base.OnMouseMove(e);_mouse=e.Location;
            if(_dragOffset>=0&&Capture) {
                int travel=Track.Height-Thumb.Height;
                if(travel>0) _text.ScrollToLine((int)Math.Round((double)(e.Y-Track.Top-_dragOffset)*Maximum/travel));
            }
            Invalidate();
        }
        protected override void OnMouseUp(MouseEventArgs e) { base.OnMouseUp(e);Capture=false;StopDrag(); }
        protected override void OnMouseCaptureChanged(EventArgs e) { base.OnMouseCaptureChanged(e);if(!Capture) StopDrag(); }
        private void StopDrag() { _repeat.Stop();_dragOffset=-1;_pressedZone=0;Invalidate(); }
        protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e);_mouse=new Point(-1,-1);Invalidate(); }
        protected override void OnMouseWheel(MouseEventArgs e) {
            _text.ScrollByWheel(e.Delta);if(e is HandledMouseEventArgs handled) handled.Handled=true;base.OnMouseWheel(e);
        }
        protected override bool IsInputKey(Keys keyData) => keyData is Keys.Up or Keys.Down or Keys.PageUp or Keys.PageDown or Keys.Home or Keys.End || base.IsInputKey(keyData);
        protected override void OnKeyDown(KeyEventArgs e) {
            int? target=e.KeyCode switch { Keys.Up=>_first-1,Keys.Down=>_first+1,Keys.PageUp=>_first-_page,Keys.PageDown=>_first+_page,Keys.Home=>0,Keys.End=>Maximum,_=>null };
            if(target.HasValue) { _text.ScrollToLine(target.Value);e.Handled=true;e.SuppressKeyPress=true; }base.OnKeyDown(e);
        }
        protected override void Dispose(bool disposing) {
            if(disposing) { _text.ViewportChanged-=TextViewportChanged;_repeat.Dispose(); }base.Dispose(disposing);
        }
    }
}
