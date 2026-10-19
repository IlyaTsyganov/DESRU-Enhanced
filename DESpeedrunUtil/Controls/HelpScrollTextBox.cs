using System.Runtime.InteropServices;

namespace DESpeedrunUtil.Controls {
    internal sealed class HelpScrollTextBox : TextBox {
        private const int GetLineCount=0xba, GetFirstVisibleLine=0xce, LineScroll=0xb6, MouseWheel=0x20a;
        private int _wheelRemainder;
        internal event EventHandler? ViewportChanged;
        [DllImport("user32.dll",ExactSpelling=true)]
        private static extern IntPtr SendMessageW(IntPtr window,int message,IntPtr wParam,IntPtr lParam);

        internal int FirstLine => IsHandleCreated?(int)SendMessageW(Handle,GetFirstVisibleLine,IntPtr.Zero,IntPtr.Zero):0;
        internal int TotalLines => IsHandleCreated?Math.Max(1,(int)SendMessageW(Handle,GetLineCount,IntPtr.Zero,IntPtr.Zero)):1;
        internal int PageLines {
            get {
                int first=FirstLine, start=GetFirstCharIndexFromLine(first), next=GetFirstCharIndexFromLine(first+1);
                int height=next>=0&&start>=0?GetPositionFromCharIndex(next).Y-GetPositionFromCharIndex(start).Y:Font.Height;
                return Math.Max(1,ClientSize.Height/Math.Max(1,height));
            }
        }
        internal int LastFirstLine => Math.Max(0,TotalLines-PageLines);
        internal void ScrollToLine(int line) {
            if(!IsHandleCreated) return;
            int delta=Math.Clamp(line,0,LastFirstLine)-FirstLine;
            if(delta!=0) SendMessageW(Handle,LineScroll,IntPtr.Zero,new IntPtr(delta));
        }
        internal void ScrollByWheel(int delta) {
            _wheelRemainder+=delta;
            int steps=_wheelRemainder/120;_wheelRemainder%=120;
            int lines=SystemInformation.MouseWheelScrollLines;
            if(steps!=0&&lines!=0) ScrollToLine(FirstLine-steps*(lines<0?PageLines:lines));
        }
        protected override void WndProc(ref Message message) {
            if(message.Msg==MouseWheel) {
                ScrollByWheel((short)((message.WParam.ToInt64()>>16)&0xffff));
                message.Result=IntPtr.Zero;return;
            }
            base.WndProc(ref message);
            if(message.Msg is 0x5 or 0x30 or 0x100 or 0x101 or 0x115 or 0x202 or 0xb5 or LineScroll or 0xb7 or 0xc2 or 0xc)
                ViewportChanged?.Invoke(this,EventArgs.Empty);
        }
        protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e);_wheelRemainder=0;ViewportChanged?.Invoke(this,EventArgs.Empty); }
    }
}
