using DESpeedrunUtil;
using DESpeedrunUtil.Memory;
using DESpeedrunUtil.Overlay;
using System.Reflection;
using System.Runtime.InteropServices;

internal static class VisualFeatureTests {
    internal static void Run(Action<bool,string> check) {
        string order="MFRSLX";
        for(int mask=0;mask<64;mask++) {
            bool Flag(int bit)=>(mask&(1<<bit))!=0;
            string flags=OsdModifiers.Flags(Flag(0),Flag(1),Flag(2),Flag(3),Flag(4),Flag(5));
            string expected=new(order.Where((c,i)=>Flag(i)).ToArray());
            string row=OsdModifiers.Version("6.66 Rev 3.2",flags);
            check(flags==expected&&row.Length<=17&&(flags.Length==0||row.EndsWith("("+flags+")")),"All modifier combinations keep order and fit the real native string buffer");
        }
        foreach(string version in new[]{"1.0 (Release)","1.1","May Hotfix Steam","6.66 Rev 3.2 (Gamepass)","Unrecognized game build"})
            check(OsdModifiers.Version(version,order).EndsWith("(MFRSLX)")&&OsdModifiers.Version(version,order).Length<=17,"Full flags survive long game version labels");
        for(int mask=0;mask<128;mask++) {
            bool Flag(int bit)=>(mask&(1<<bit))!=0;
            int request=LiveVisualFeatures.Request(Flag(0),Flag(1),Flag(2),Flag(3),Flag(4),Flag(5),Flag(6));
            check(((request&2)!=0)==(Flag(1)&&Flag(2))&&((request&64)!=0)==Flag(1)&&
                ((request&32)!=0)==Flag(3)&&((request&1)!=0)==Flag(0),"Cheats gate viewer visibility independently of lighting and fill");
        }
        var exports=LiveVisualFeatures.ReadExports("artifacts/visuals-build/bin/DESRU.Visuals.dll");
        check(exports["DESRUVisualControl"]>0&&exports["DesruVisualsRegister"]!=exports["DesruVisualsStart"],"The built native module exposes the required separate protocol and entry points");
        IntPtr module=NativeLibrary.Load(Path.GetFullPath("artifacts/visuals-build/bin/DESRU.Visuals.dll"));
        try {
            IntPtr control=NativeLibrary.GetExport(module,"DESRUVisualControl");
            check(Marshal.ReadInt32(control+8)==2&&BitConverter.Int32BitsToSingle(Marshal.ReadInt32(control+80))==40,"The native distance field has the expected protocol offset and 40-metre default");
            check(Marshal.ReadInt32(NativeLibrary.GetExport(module,"DESRUVisualDiagnosticSkipDepth"))==0,"The session-only depth diagnostic starts with normal occlusion enabled");
        } finally { NativeLibrary.Free(module); }
        using var window=new MainWindow();
        var lighting=(CheckBox)window.Controls.Find("lightingFixCheckbox",true).Single();
        var lightStatus=(Label)window.Controls.Find("lightingFixStatus",true).Single();
        check(lighting.Parent!.ClientRectangle.Contains(lighting.Bounds)&&lightStatus.Parent!.ClientRectangle.Contains(lightStatus.Bounds),"Lighting controls fit the compact panels");
        check(window.Controls.Find("collisionViewerCheckbox",true).Length==0&&window.Controls.Find("collisionViewerStatus",true).Length==0&&window.Controls.Find("collisionViewerStatusLabel",true).Length==0,"Viewer checkbox and Info Panel status are removed");
        var firewall=window.Controls.Find("firewallToggleButton",true).Single();
        var cheats=window.Controls.Find("cheatsToggleButton",true).Single();
        var trainer=window.Controls.Find("trainerPanel",true).Single();
        check(lighting.Bottom<=firewall.Top&&firewall.Bottom<=cheats.Top&&lighting.Parent.Bottom==trainer.Bottom,"Lighting remains above the buttons and Options aligns with Trainer");
        string other = DESpeedrunUtil.Properties.Resources.HelpPage_Other;
        check(!DESpeedrunUtil.Properties.Resources.HelpPage_OSD.Contains("RESTART GAME")&&other.Contains("desru_invisible_walls_show")&&!other.Contains("desru_entity_walls_show"),"Help contains the final commands and removes obsolete restart guidance");
        check(other.Contains("desru_volumes_render_distance")&&other.Contains("metres"),"Other explains the configurable rendering distance and units");
        check(other.Contains("desru_volumes_show")&&other.Contains("desru_volumes_fill")&&!DESpeedrunUtil.Properties.Resources.HelpPage_OSD.Contains("MFRSLXV")&&!other.Contains("Collision Viewer checkbox"),"Other describes cheat-gated console controls without the obsolete V indicator");
    }
    internal static void Preview(string output) {
        string previous=Environment.CurrentDirectory;
        using var window=new MainWindow();
        try {
            Environment.CurrentDirectory=Path.GetFullPath("output");
            const BindingFlags flags=BindingFlags.Instance|BindingFlags.NonPublic;
            typeof(MainWindow).GetMethod("InitializeFonts",flags)!.Invoke(window,null);
            window.Size=new Size(641,DESpeedrunUtil.Define.Constants.WINDOW_HEIGHT_DEFAULT);
            window.Load -= (EventHandler)Delegate.CreateDelegate(typeof(EventHandler),window,"MainWindow_Load");
            window.Shown -= (EventHandler)Delegate.CreateDelegate(typeof(EventHandler),window,"MainWindow_Shown");
            window.StartPosition=FormStartPosition.Manual; window.Location=new Point(-20000,-20000); window.ShowInTaskbar=false;
            var panel=window.Controls.Find("collapsiblePanel",true).Single();panel.Height=DESpeedrunUtil.Define.Constants.PANEL_HEIGHT_DEFAULT;
            foreach(string name in new[]{"hotkeysTitle","versionTitle","optionsTitle","infoPanelTitle","resTitle","trainerTitle"}) window.Controls.Find(name,true).Single().Visible=true;
            window.Show(); Application.DoEvents();
            using var bitmap=new Bitmap(window.Width,window.Height);
            window.DrawToBitmap(bitmap,new Rectangle(Point.Empty,bitmap.Size));
            bitmap.Save(output);
            window.Hide();
        } finally { Environment.CurrentDirectory=previous; }
    }
}
