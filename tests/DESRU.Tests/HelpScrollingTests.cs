using DESpeedrunUtil;
using DESpeedrunUtil.Controls;
using System.Reflection;
using System.Runtime.InteropServices;

internal static class HelpScrollingTests {
    [DllImport("user32.dll",ExactSpelling=true)] private static extern IntPtr SendMessageW(IntPtr window,int message,IntPtr first,IntPtr second);
    private static void Input(Control control,string name,EventArgs args) => control.GetType().GetMethod(name,BindingFlags.NonPublic|BindingFlags.Instance)!.Invoke(control,new object[]{args});
    internal static void Run(Action<bool,string> check) {
        using var host=new Form { ClientSize=new Size(400,250),ShowInTaskbar=false };
        using var text=new HelpScrollTextBox { Multiline=true,ReadOnly=true,BorderStyle=BorderStyle.None,Bounds=new Rectangle(8,8,340,220) };
        using var bar=new DesruTextScrollBar(text) { Bounds=new Rectangle(352,8,18,220) };
        host.Controls.Add(text);host.Controls.Add(bar);
        _=host.Handle;_=text.Handle;_=bar.Handle;
        text.Text=string.Join("\r\n",Enumerable.Range(0,100).Select(i=>$"Line {i}: a complete help paragraph that wraps across several display lines."));
        bar.UpdateViewport();
        check(bar.Maximum>0&&text.FirstLine==0,"Long help starts at the top with a usable custom scroll range");
        SendMessageW(text.Handle,0x20a,new IntPtr(-120<<16),IntPtr.Zero);
        check((SystemInformation.MouseWheelScrollLines==0||text.FirstLine>0)&&bar.Value==text.FirstLine,"A real mouse-wheel message scrolls the native text and synchronizes the custom thumb");
        text.ScrollToLine(int.MaxValue);
        check(text.FirstLine==text.LastFirstLine&&bar.Value==bar.Maximum&&bar.Thumb.Bottom==bar.Height-18,"Scrolling to the end reaches the final help lines and the track bottom");
        Input(bar,"OnKeyDown",new KeyEventArgs(Keys.Home));
        check(text.FirstLine==0&&bar.Value==0,"Home on the custom scrollbar returns to the beginning");
        Rectangle thumb=bar.Thumb;
        Input(bar,"OnMouseDown",new MouseEventArgs(MouseButtons.Left,1,bar.Width/2,thumb.Top+thumb.Height/2,0));
        Input(bar,"OnMouseMove",new MouseEventArgs(MouseButtons.Left,0,bar.Width/2,bar.Height-18-thumb.Height/2+1,0));
        Input(bar,"OnMouseUp",new MouseEventArgs(MouseButtons.Left,1,bar.Width/2,bar.Height-18,0));
        check(bar.Value==bar.Maximum&&text.FirstLine==bar.Maximum,"Dragging the custom thumb reaches the last text page");
        int before=text.FirstLine;
        Input(bar,"OnMouseDown",new MouseEventArgs(MouseButtons.Left,1,bar.Width/2,8,0));
        Input(bar,"OnMouseUp",new MouseEventArgs(MouseButtons.Left,1,bar.Width/2,8,0));
        check(text.FirstLine==before-1,"The upper arrow scrolls one display line");
        text.ScrollToLine(0);int expected=Math.Min(text.PageLines,text.LastFirstLine);
        Input(bar,"OnMouseDown",new MouseEventArgs(MouseButtons.Left,1,bar.Width/2,bar.Thumb.Bottom+20,0));
        Input(bar,"OnMouseUp",new MouseEventArgs(MouseButtons.Left,1,bar.Width/2,bar.Thumb.Bottom+20,0));
        check(text.FirstLine==expected,"Clicking below the thumb scrolls by one page");
        int wideLines=text.TotalLines;text.Width=160;bar.UpdateViewport();
        check(text.TotalLines>wideLines&&bar.Value==text.FirstLine,"Word wrapping and scrollbar range update together when the text width changes");
        text.Text="Short help";bar.UpdateViewport();
        check(!bar.Enabled&&bar.Value==0&&bar.Maximum==0,"Short text resets the scroll position and disables the thumb");
        using var help=new HelpPage();
        var button=(Button)help.Controls.Find("helpTabButton6",true).Single();
        typeof(HelpPage).GetMethod("TabButton_Click",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(help,new object[]{button,EventArgs.Empty});
        var rendered=(TextBox)help.Controls.Find("helpTextbox",true).Single();
        check(rendered.Lines.Contains("X - Lighting Fix Enabled")&&!rendered.Text.Contains("desru_")&&rendered.Lines.Any(string.IsNullOrEmpty),"Approved OSD text preserves its flags and paragraphs without the moved commands");
        var otherButton=(Button)help.Controls.Find("helpTabButton8",true).Single();
        typeof(HelpPage).GetMethod("TabButton_Click",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(help,new object[]{otherButton,EventArgs.Empty});
        check(rendered.Lines.Where(line=>line.StartsWith("desru_")).Select(line=>line.Split(' ')[0]).Distinct().Count()==13&&rendered.Text.EndsWith("(set low-health tint opacity; default 100)"),"Other contains all 13 command names and preserves the user's final edited ending");
        check(help.ClientRectangle.Contains(otherButton.Bounds)&&help.Controls.OfType<Button>().Where(b=>b!=otherButton).All(b=>!b.Bounds.IntersectsWith(otherButton.Bounds)),"The Other navigation button fits without overlapping any existing Help button");
    }
    internal static void Preview(string output,string tab="osd") {
        string previous=Environment.CurrentDirectory;
        using var main=new MainWindow();
        try {
            Environment.CurrentDirectory=Path.GetFullPath("output");
            typeof(MainWindow).GetMethod("InitializeFonts",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(main,null);
            using var help=new HelpPage();
            help.Load-=(EventHandler)Delegate.CreateDelegate(typeof(EventHandler),help,"HelpPage_Load");
            help.StartPosition=FormStartPosition.Manual;help.Location=new Point(-20000,-20000);help.ShowInTaskbar=false;
            help.Show();
            ((Button)help.Controls.Find(tab=="other"?"helpTabButton8":tab=="info"?"helpTabButton5":"helpTabButton6",true).Single()).PerformClick();
            Application.DoEvents();
            using var bitmap=new Bitmap(help.Width,help.Height);help.DrawToBitmap(bitmap,new Rectangle(Point.Empty,bitmap.Size));bitmap.Save(output);
            help.Hide();
        } finally { Environment.CurrentDirectory=previous; }
    }
}
