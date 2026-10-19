using DESpeedrunUtil.Memory;
using DESpeedrunUtil.Overlay;
using System.Reflection;
using System.Diagnostics;
using Settings = DESpeedrunUtil.Properties.Settings;

internal static class Program {
    private static int _checks;
    private static void Check(bool condition, string message) {
        if(!condition) throw new Exception(message);
        _checks++;
    }

    [STAThread]
    private static int Main(string[] args) {
        try {
            if(args.Length==2&&args[0]=="--live-modbot-guard") {
                LiveModbotInputTests.Run(Math.Clamp(int.Parse(args[1]),10,600));return 0;
            }
            if(args.Length==2&&args[0]=="--visual-depth-test") {
                VisualDepthDiagnostic.Run(args[1]);return 0;
            }
            if(args.Contains("--live-visual-lighting")||args.Contains("--live-visual-viewer")) {
                LiveVisualTests.Run(args.Contains("--live-visual-viewer"),Check).GetAwaiter().GetResult();
                Console.WriteLine($"PASS: {_checks} live visual feature checks");return 0;
            }
            if(args.Length==2&&args[0]=="--preview-visual-options") {
                ApplicationConfiguration.Initialize();
                VisualFeatureTests.Preview(Path.GetFullPath(args[1]));return 0;
            }
            if(args.Length==2&&args[0] is "--preview-help-osd" or "--preview-help-other" or "--preview-help-info") {
                ApplicationConfiguration.Initialize();HelpScrollingTests.Preview(Path.GetFullPath(args[1]),args[0].Replace("--preview-help-",""));return 0;
            }
            if(args.Contains("--test-firewall")) {
                FirewallToggleTests.Run(Check);
                Console.WriteLine($"PASS: {_checks} isolated Windows firewall and wired button checks");
                return 0;
            }
            if(args.Contains("--inspect-player-cheats")) {
                using var game = Process.GetProcessesByName("DOOMEternalx64vk").Single();
                var layout = NativeGameLayout.Resolve(game.MainModule!.FileName);
                GameVersion.Collection = Newtonsoft.Json.JsonConvert.DeserializeObject<List<GameVersion>>(
                    System.Text.Encoding.UTF8.GetString(DESpeedrunUtil.Properties.Resources.gameVersions))!;
                var version = GameVersion.GetVersionByModuleSize(game.MainModule.ModuleMemorySize);
                var playerOffsets = Newtonsoft.Json.JsonConvert.DeserializeObject<List<DESpeedrunUtil.Define.Structs.KnownOffsets>>(
                    System.Text.Encoding.UTF8.GetString(DESpeedrunUtil.Properties.Resources.offsets))!.Single(o => o.Version == version.Name);
                IntPtr root = game.ReadPointer(game.MainModule.BaseAddress + playerOffsets.Velocity), slot = game.ReadPointer(root + 0x1510);
                IntPtr player = game.ReadPointer(slot + (int.TryParse(version.Name[..1], out int major) && major >= 3 ? 0x5c0 : 0x598));
                var state = new PlayerCheatMonitor(game, layout.PlayerCheats ?? throw new NotSupportedException("Player cheat getters are unavailable")).Read(player);
                Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(new { PlayerAvailable = player != IntPtr.Zero, state.Noclip, state.Notarget, state.God }));
                return 0;
            }
            if(args.Contains("--live-combined-toggle")) {
                LiveCombinedToggleTests.Run(Check);
                Console.WriteLine($"PASS: {_checks} live combined button checks");
                return 0;
            }
            if(args.Length == 2 && args[0] == "--resolve-game") {
                Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(NativeGameLayout.Resolve(args[1])));
                return 0;
            }
            if(args.Contains("--inspect-cvars") || args.Contains("--live-meathook-cycle")) {
                using var game = Process.GetProcessesByName("DOOMEternalx64vk").Single();
                var layout = NativeGameLayout.Resolve(game.MainModule!.FileName);
                var monitor = new ProtectedCvarMonitor(game, layout.Cvars ?? throw new NotSupportedException("No cvar layout"));
                var differences = monitor.Read();
                Console.WriteLine($"changed protected cvars: {differences.Count}");
                foreach(var item in differences.Take(60)) Console.WriteLine($"{item.Name} = {item.Value} (default {item.Default})");
                if(args.Contains("--inspect-cvars")) return 0;
                using var native = NativeSpeedometer.Attach(game);
                var runtime = new LiveMeathook(game, native.Extension, layout);
                var session = new CheatCvarSession(game, native.Extension, monitor);
                if(runtime.Active && session.Active) {
                    Console.WriteLine("Restoring the previous interrupted diagnostic session...");
                    session.RestoreAsync().GetAwaiter().GetResult(); runtime.Disable(); session.End();
                }
                Check(!session.Active && !runtime.Active, "Live test starts with cheats and cvar snapshot inactive");
                var initial = monitor.Read();
                var initialVolume = initial.Single(c => c.Name == "s_volume");
                var initialShakes = initial.Single(c => c.Name == "view_skipShakes");
                session.BeginAsync().GetAwaiter().GetResult();
                Console.WriteLine("Enabling live Meathook...");
                runtime.EnableAsync(Path.Combine(Environment.CurrentDirectory, "output/meath00k/XINPUT1_3.dll")).GetAwaiter().GetResult();
                Check(runtime.Active, "Live Meathook enabled");
                Console.WriteLine("Meathook attached. Testing cvar action and display...");
                native.Extension.ExecuteAsync("timescale 0.5").GetAwaiter().GetResult();
                differences = monitor.Read();
                Check(differences.Any(c => c.Name == "timescale" && c.Value.StartsWith("0.5")), "Game-thread command changes a protected cvar and is detected");
                native.Extension.SetCvarAsync("view_skipShakes", "0").GetAwaiter().GetResult();
                Check(!monitor.Read().Any(c => c.Name == "view_skipShakes"), "Native setter accepts explicit cvar arguments");
                if(!SpeedometerOverlay.TryGetPhysicalViewport(game.MainWindowHandle, out var cvarViewport)) cvarViewport = new Rectangle(0, 0, 1920, 1080);
                native.Extension.Update(differences, cvarViewport, 740, 1, native.GlyphWidth, native.GlyphHeight, true);
                Thread.Sleep(1000);
                var resumedSession = new CheatCvarSession(game, native.Extension, monitor);
                Check(resumedSession.Active, "Initial preferences reconnect across DESRU instances");
                Check(resumedSession.RestoreAsync().GetAwaiter().GetResult() == 2, "Only the two changes made during cheats are restored");
                Check(!monitor.Read().Any(c => c.Name == "timescale"), "Reset cvar disappears from the changed-values list");
                Check(monitor.Read().Single(c => c.Name == "view_skipShakes").Value == initialShakes.Value &&
                    monitor.Read().Single(c => c.Name == "s_volume").Value == initialVolume.Value, "Initial shake and volume settings are preserved");
                runtime.Disable(); Check(!runtime.Active, "Live Meathook disabled");
                resumedSession.End();
                Check(!new CheatCvarSession(game, native.Extension, monitor).Active, "Ended cvar sessions reconnect as inactive");
                Console.WriteLine("Meathook disabled. Reconnecting and enabling a second time...");
                var reconnected = new LiveMeathook(game, native.Extension, layout);
                Check(!reconnected.Active, "Disabled state reconnects from game-owned session data");
                reconnected.EnableAsync(Path.Combine(Environment.CurrentDirectory, "output/meath00k/XINPUT1_3.dll")).GetAwaiter().GetResult();
                Check(reconnected.Active, "Meathook re-enabled without rerunning initialization");
                reconnected.Disable(); Check(!reconnected.Active, "Meathook disabled a second time");
                Console.WriteLine($"PASS: {_checks} live extension and Meathook checks");
                return 0;
            }
            if(args.Contains("--inspect-driver-name") || args.Contains("--hide-driver-name")) {
                using var game = Process.GetProcessesByName("DOOMEternalx64vk").Single();
                GameVersion.Collection = Newtonsoft.Json.JsonConvert.DeserializeObject<List<GameVersion>>(
                    System.Text.Encoding.UTF8.GetString(DESpeedrunUtil.Properties.Resources.gameVersions))!;
                var version = GameVersion.GetVersionByModuleSize(game.MainModule!.ModuleMemorySize);
                var known = Newtonsoft.Json.JsonConvert.DeserializeObject<List<DESpeedrunUtil.Define.Structs.KnownOffsets>>(
                    System.Text.Encoding.UTF8.GetString(DESpeedrunUtil.Properties.Resources.offsets))!.Single(o => o.Version == version.Name);
                if(known.GPUVendor == 0) throw new NotSupportedException("No known driver-name offset for this game build");
                IntPtr inline = game.MainModule.BaseAddress + known.GPUVendor;
                IntPtr data = game.ReadPointer(inline - 16);
                Console.WriteLine($"game={version.Name} inlineRva={known.GPUVendor:X} heap={data != inline} name={game.ReadString(data, ReadStringType.ASCII, 64)}");
                byte[] header = game.ReadBytes(inline - 24, 24), neighbors = game.ReadBytes(inline + 20, 76);
                if(args.Contains("--hide-driver-name")) {
                    Check(PerformanceMetricsDriverName.Hide(game, inline), "Live driver name is hidden");
                    Check(game.ReadValue<byte>(data) == 0, "Live metrics consume an empty driver name");
                    Check(header.SequenceEqual(game.ReadBytes(inline - 24, 24)) && neighbors.SequenceEqual(game.ReadBytes(inline + 20, 76)),
                        "Live string header and adjacent driver-version objects remain intact");
                    Console.WriteLine($"PASS: {_checks} live driver-name checks");
                }
                return 0;
            }
            if(args.Contains("--inspect-lowhp") || args.Contains("--trace-lowhp")) {
                using var game = Process.GetProcessesByName("DOOMEternalx64vk").First(p => p.MainModule!.ModuleMemorySize == NativeSpeedometer.ModuleSize11);
                IntPtr image = game.MainModule!.BaseAddress;
                IntPtr root = game.ReadPointer(image + 80143112), slot = game.ReadPointer(root + 0x1510), player = game.ReadPointer(slot + 0x598);
                Console.WriteLine($"exe={game.MainModule.FileName} root={root.ToInt64():X} slot={slot.ToInt64():X} player={player.ToInt64():X} vtableRva={game.ReadPointer(player).ToInt64()-image.ToInt64():X} lowWarning={game.ReadValue<byte>(player + 0x25F48)} fxSlot={game.ReadValue<int>(player + 0x25F60)} fxCondition={game.ReadValue<int>(player + 0x25F64)}");
                IntPtr render = game.ReadPointer(image + 0x2608800, true, out var renderer) ? renderer : IntPtr.Zero;
                Console.WriteLine($"renderer={render.ToInt64():X}");
                byte[] header = game.ReadBytes(render, 20);
                if(header != null && header.AsSpan(0, 12).SequenceEqual(Convert.FromHexString("5356574883EC40488BF248BB"))) {
                    IntPtr state = new(BitConverter.ToInt64(header, 12));
                    if(args.Contains("--trace-lowhp")) { LowHealthTintHook.Attach(game, state); LowHealthHudHook.Attach(game, state); new LowHealthTintTarget(game).Update(state); Thread.Sleep(100); }
                    Console.WriteLine($"state={state.ToInt64():X} magic={System.Text.Encoding.ASCII.GetString(game.ReadBytes(state + 48, 8))} registered={game.ReadValue<int>(state + 4)} speedLease={game.ReadValue<int>(state)} tintLease={game.ReadValue<int>(state + 192)} scale={game.ReadValue<float>(state + 196)} frames={game.ReadValue<long>(state + 8)}");
                    Console.WriteLine($"tintMailbox={Convert.ToHexString(game.ReadBytes(state + 0x600, 80))}");
                    IntPtr view = game.ReadPointer(state + 200);
                    Console.WriteLine($"view={view.ToInt64():X} combinedCalls={game.ReadValue<long>(state + 208)} normalCalls={game.ReadValue<long>(state + 216)} postProcessCalls={game.ReadValue<long>(state + 224)}");
                    Console.WriteLine($"fxHandle={game.ReadValue<long>(state + 232):X} indices={game.ReadValue<int>(state + 240)},{game.ReadValue<int>(state + 244)} matches={game.ReadValue<long>(state + 248)} alpha={game.ReadValue<float>(state + 256)} -> {game.ReadValue<float>(state + 260)}");
                    Console.WriteLine($"hud={game.ReadPointer(state + 280).ToInt64():X} swf={game.ReadPointer(state + 288).ToInt64():X} calls={game.ReadValue<long>(state + 296)} matches={game.ReadValue<long>(state + 304)}");
                    if(view != IntPtr.Zero) Console.WriteLine($"viewEnabled={game.ReadValue<byte>(view + 0xC88)} lowAlpha={game.ReadValue<int>(view + 0x4010)} damageAlpha={game.ReadValue<float>(view + 0x4014)} targetLowHealth={game.ReadValue<float>(view + 0x359C)} postLowHealth={game.ReadValue<float>(view + 0x358C)} material={game.ReadPointer(view + 0x4008).ToInt64():X}");
                }
                foreach(int site in new[] { 0x1756076, 0x175614a, 0x17501bd }) Console.WriteLine($"{site:X}: {Convert.ToHexString(game.ReadBytes(image + site, 16))}");
                return 0;
            }
            if(args.Contains("--landing-trace")) return LandingTrace();
            if(args.Contains("--inspect-render")) {
                using var game = Process.GetProcessesByName("DOOMEternalx64vk").First(p => { try { return p.MainModule!.ModuleMemorySize == 492113920; } catch { return false; } });
                foreach(int rva in new[] { 0x2608800, 0x44be70, 0x5a5b90, 0x5a6090, 0x5a6060, 0x4f6855c, 0x4f68560, 0x4f68564 })
                    Console.WriteLine($"{rva:X}: {Convert.ToHexString(game.ReadBytes(game.MainModule!.BaseAddress + rva, 32))}");
                return 0;
            }
            if(args.Length == 2 && args[0] == "--verify-package") {
                Directory.SetCurrentDirectory(Path.GetFullPath(args[1]));
                bool valid = (bool) typeof(DESpeedrunUtil.Program).GetMethod("FileCheck", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, null)!;
                Check(valid, "The real DESRU startup file check accepts the assembled package");
                var map = new System.Configuration.ExeConfigurationFileMap { ExeConfigFilename = Path.GetFullPath("DESRU.dll.config") };
                var config = System.Configuration.ConfigurationManager.OpenMappedExeConfiguration(map, System.Configuration.ConfigurationUserLevel.None);
                var section = (System.Configuration.ClientSettingsSection) config.GetSection("userSettings/DESpeedrunUtil.Properties.Settings");
                Check(section.Settings.Get("SpeedometersEnabled")?.Value.ValueXml.InnerText == "True", "Packaged settings load through the real .NET configuration parser");
                Check(section.Settings.Get("CurrentSpeedOverlayColor")?.Value.ValueXml.InnerText == "White" && section.Settings.Get("LandingSpeedOverlayColor")?.Value.ValueXml.InnerText == "White", "Packaged RGB defaults deserialize");
                Check(config.AppSettings.Settings["EnableUpstreamUpdates"]?.Value == "False", "Standalone cannot silently replace the fork with an upstream update");
                foreach(string application in new[] { "DESRU", "Updater" }) {
                    using var runtime = System.Text.Json.JsonDocument.Parse(File.ReadAllText(application + ".runtimeconfig.json"));
                    var options = runtime.RootElement.GetProperty("runtimeOptions");
                    Check(!options.TryGetProperty("framework", out _) && !options.TryGetProperty("frameworks", out _), application + " has no external framework dependency");
                    Check(options.GetProperty("includedFrameworks").EnumerateArray().Count(f => f.GetProperty("version").GetString() == "6.0.36") == 2, application + " includes both pinned desktop/runtime frameworks");
                }
                foreach(string runtimeFile in new[] { "coreclr.dll", "hostfxr.dll", "hostpolicy.dll", "System.Windows.Forms.dll" })
                    Check(File.Exists(runtimeFile), "Standalone includes " + runtimeFile);
                byte[] native = File.ReadAllBytes("visuals/DESRU.Visuals.Runtime.dll");
                int pe = BitConverter.ToInt32(native, 0x3c);
                Check(BitConverter.ToUInt16(native, pe + 4) == 0x8664, "Packaged visual module is x64");
                Check(System.Text.Encoding.ASCII.GetString(native).Contains("DesruVisualsStart") && System.Text.Encoding.ASCII.GetString(native).Contains("DESRUVisualControl"), "Packaged native bridge exports are present");
                var packagedAssembly = Assembly.LoadFile(Path.GetFullPath("DESRU.dll"));
                Check(packagedAssembly.GetName().Version == new Version(1,1,0,0), "Patch keeps the installed settings assembly identity");
                Check(FileVersionInfo.GetVersionInfo(Path.GetFullPath("DESRU.dll")).FileVersion == "1.1.1.0", "Patch file version identifies the current release independently of the settings identity");
                Check(packagedAssembly.GetType("DESpeedrunUtil.Overlay.NativeModbotInputGuard") != null, "Standalone includes the tested modbot input protection");
                var resources = new System.Resources.ResourceManager("DESpeedrunUtil.Properties.Resources", packagedAssembly);
                foreach(string name in new[] { "HelpPage_OSD", "HelpPage_InfoPanel", "HelpPage_Other" })
                    Check(resources.GetString(name) == DESpeedrunUtil.Properties.Resources.ResourceManager.GetString(name) && resources.GetString(name) != null, "Packaged approved help matches tested source: " + name);
                Console.WriteLine($"PASS: {_checks} startup, self-contained runtime, native module and approved Help package checks");
                return 0;
            }
            if(args.Contains("--probe-game") || args.Contains("--live-overlay") || args.Contains("--native-overlay"))
                return ProbeGame(args.Contains("--live-overlay"), args.Contains("--native-overlay"));
            if(args.Contains("--settings-write")) {
                var s = Settings.Default;
                s.CurrentSpeedOverlayEnabled = true;
                s.LandingSpeedOverlayEnabled = false;
                s.CurrentSpeedOverlayX = 2500; s.CurrentSpeedOverlayY = 1350;
                s.LandingSpeedOverlayX = 3000; s.LandingSpeedOverlayY = 1500;
                s.SpeedometerPositionsVersion = 1;
                s.LowHpTintOpacity = 37.5;
                s.SpeedometersEnabled = false;
                s.CurrentSpeedOverlayColor = Color.FromArgb(12, 34, 56); s.LandingSpeedOverlayColor = Color.FromArgb(255, 128, 0);
                s.CurrentSpeedOverlayScale = 1.25; s.LandingSpeedOverlayScale = 1.75;
                s.ShowSpeedometer = true;
                s.Save();
                Check(SpeedometerCommandHelp.Format(s, 6).Contains("default:\"255 255 255\""), "Saving colors cannot mutate the default shown in help");
                Console.WriteLine("Saved settings in the separate test executable's profile.");
                return 0;
            }
            if(args.Contains("--settings-read")) {
                var s = Settings.Default;
                Check(s.CurrentSpeedOverlayEnabled && !s.LandingSpeedOverlayEnabled, "Independent enable flags survive restart");
                Check(s.CurrentSpeedOverlayX == 2500 && s.CurrentSpeedOverlayY == 1350 && s.LandingSpeedOverlayX == 3000 && s.LandingSpeedOverlayY == 1500 && s.SpeedometerPositionsVersion == 1, "Distinct pixel coordinates and migration version survive restart");
                Check(s.ShowSpeedometer, "Existing speedometer setting survives restart");
                Check(s.LowHpTintOpacity == 37.5, "Low-health tint opacity survives restart");
                Check(!s.SpeedometersEnabled && s.CurrentSpeedOverlayColor.ToArgb() == Color.FromArgb(12, 34, 56).ToArgb() && s.LandingSpeedOverlayColor.ToArgb() == Color.FromArgb(255, 128, 0).ToArgb(), "Master visibility and distinct RGB colors survive restart");
                Check(s.CurrentSpeedOverlayScale == 1.25 && s.LandingSpeedOverlayScale == 1.75, "Independent sizes survive restart");
                Check(SpeedometerCommandHelp.Format(s, 6).Contains("is:\"current=12 34 56 landing=255 128 0\" default:\"255 255 255\""), "Help separates saved profile colors from immutable application defaults after restart");
                s.Reset();
                Console.WriteLine($"PASS: {_checks} restart persistence checks");
                return 0;
            }

            var tracker = new HorizontalSpeedTracker();
            Check(!(bool) typeof(DESpeedrunUtil.Program).GetMethod("UpdateCheck", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, null)!, "Fork does not query or install upstream updates by default");
            NativeAdapterTests.Run(Check);
            ModbotInputTests.Run(Check);
            NativeStartupTests.Run(Check);
            CommandDefaultsTests.Run(Check);
            LowHealthTintTests.Run(Check);
            PerformanceMetricsDriverNameTests.Run(Check);
            LiveToolsTests.Run(Check);
            var offsets = Newtonsoft.Json.JsonConvert.DeserializeObject<List<DESpeedrunUtil.Define.Structs.KnownOffsets>>(
                System.Text.Encoding.UTF8.GetString(DESpeedrunUtil.Properties.Resources.offsets))!;
            Check(offsets.Single(o => o.Version == "1.1").GroundPlaneFromVelocity == 136, "Compiled offset resource contains the verified 1.1 contact address");
            Check(tracker.Current == null && tracker.LastLanding == null, "No fabricated initial speed");
            tracker.Update(3, 4, true);
            Check(tracker.Current == 5 && tracker.LastLanding == null, "Horizontal magnitude; attaching on ground is not a landing");
            tracker.Update(6, 8, false);
            Check(tracker.LastLanding == null, "Takeoff does not capture a landing");
            tracker.Update(9, 12, false);
            Check(tracker.LastLanding == null && tracker.Current == 15, "Airborne updates (including apex) do not capture a landing");
            tracker.Update(12, 16, true);
            Check(tracker.LastLanding == 20, "First ground contact captures current horizontal speed");
            tracker.Update(0, 0, true);
            Check(tracker.LastLanding == 20 && tracker.Current == 0, "Walking and stopping do not overwrite the landing");
            tracker.Update(-3, -4, false);
            Check(tracker.LastLanding == 20, "Previous landing persists through flight");
            tracker.Update(-6, -8, true);
            Check(tracker.LastLanding == 10, "Next landing replaces the previous speed");
            tracker.Reset();
            tracker.Update(1, 1, true);
            Check(tracker.LastLanding == null, "Load/respawn/pointer reset does not fabricate a landing");
            tracker.Update(3, 4, null);
            Check(tracker.Current == 5 && tracker.LastLanding == null, "Unsupported contact data still allows current speed");
            tracker.Update(float.NaN, 0, false);
            Check(tracker.Current == null && tracker.LastLanding == null, "Invalid read clears the state");
            tracker.Update(float.PositiveInfinity, 0, true);
            Check(tracker.Current == null, "Infinite samples are rejected");
            tracker.Update(float.MaxValue, float.MaxValue, true);
            Check(tracker.Current == null, "Overflow cannot produce an infinite display");
            tracker.Update(float.MaxValue, 0, true);
            Check(tracker.Current == float.MaxValue, "Finite horizontal magnitude avoids intermediate float overflow");

            var impact = new HorizontalSpeedTracker(8, 8);
            impact.Update(0, 0, true, 0);
            impact.Update(9.52f, 0, false, 100);
            impact.Update(9.52f, 0, false, 200);
            impact.Update(22.10f, 0, true, 204);
            impact.Update(21.21f, 0, true, 208);
            impact.Update(18.75f, 0, true, 220);
            Check(impact.LastLanding == 22.10f && impact.Current == 18.75f, "Landing retains the observed 4 ms burst after horizontal speed decays");
            impact.Update(18f, 0, false, 221);
            impact.Update(17f, 0, true, 222);
            Check(impact.LastLanding == 22.10f, "Transient ground flag during a physics update cannot overwrite the impact");
            impact.Update(30, 0, true, 240);
            Check(impact.LastLanding == 22.10f, "Later grounded acceleration cannot replace the impact peak");
            impact.Update(9, 0, false, 300);
            impact.Update(10, 0, true, 400);
            impact.Update(15, 0, true, 404);
            impact.Update(14, 0, true, 408);
            Check(impact.LastLanding == 15, "The short impact window captures an impulse published immediately after the contact flag");
            VerifySampler();

            ApplicationConfiguration.Initialize();
            VisualFeatureTests.Run(Check);
            HelpScrollingTests.Run(Check);
            var output = Path.GetFullPath(args.FirstOrDefault() ?? "artifacts/test-previews");
            Directory.CreateDirectory(output);
            var commandSettings = new Settings();
            commandSettings.CurrentSpeedOverlayScale = commandSettings.LandingSpeedOverlayScale = 4;
            string sizeHelp = SpeedometerCommandHelp.Format(commandSettings, 3);
            Check(sizeHelp == "\"desru_speedometer_size\" is:\"4\" default:\"3.5\"\nSet text size (0.1 to 10). Always applies to both speedometers.\n", "Bare-command help has the requested two-line format and source defaults instead of saved values");
            commandSettings.CurrentSpeedOverlayX = 400; commandSettings.LandingSpeedOverlayX = 800;
            Check(SpeedometerCommandHelp.Format(commandSettings, 1).Contains("is:\"current=400 landing=800\""), "Independent coordinates are reported without losing either value");
            commandSettings.CurrentSpeedOverlayColor = Color.Red; commandSettings.LandingSpeedOverlayColor = Color.Lime;
            Check(SpeedometerCommandHelp.Format(commandSettings, 6).Contains("is:\"current=255 0 0 landing=0 255 0\" default:\"255 255 255\""), "Help reports both RGB colors and the white default");
            commandSettings.SpeedometersEnabled = false; commandSettings.LandingSpeedOverlayEnabled = true;
            Check(SpeedometerCommandHelp.Format(commandSettings, 0).Contains("is:\"0\" default:\"1\"") && SpeedometerCommandHelp.Format(commandSettings, 5).Contains("is:\"1\" default:\"1\""), "Master and landing help describe independent stored flags");
            Check(Enumerable.Range(0, 7).All(i => SpeedometerCommandHelp.Format(commandSettings, i).Split('\n').Length == 3 && System.Text.Encoding.ASCII.GetByteCount(SpeedometerCommandHelp.Format(commandSettings, i)) < NativeSpeedometerCode.ConsoleTextCapacity), "All seven help responses contain two lines and fit the bounded buffer");
            Check(commandSettings.CurrentSpeedOverlayScale == 4 && !commandSettings.SpeedometersEnabled && commandSettings.CurrentSpeedOverlayX == 400, "Help does not modify settings");
            commandSettings = new Settings();
            commandSettings.CurrentSpeedOverlayX = 3; commandSettings.CurrentSpeedOverlayY = 70;
            commandSettings.LandingSpeedOverlayX = 3; commandSettings.LandingSpeedOverlayY = 77;
            Check(SpeedometerCommands.Apply(commandSettings, 0, "0", "") && !commandSettings.SpeedometersEnabled, "Master show command hides both without changing the landing preference");
            Check(SpeedometerCommands.Apply(commandSettings, 5, "0", "") && !commandSettings.LandingSpeedOverlayEnabled, "Landing flag can disable the last-landing reading");
            Check(SpeedometerCommands.Apply(commandSettings, 0, "1", "") && commandSettings.SpeedometersEnabled && commandSettings.CurrentSpeedOverlayEnabled && !commandSettings.LandingSpeedOverlayEnabled, "Showing meters preserves the current-only preference");
            Check(SpeedometerCommands.Apply(commandSettings, 0, "1", "current") && commandSettings.CurrentSpeedOverlayEnabled && !commandSettings.LandingSpeedOverlayEnabled, "Current meter enables independently");
            Check(SpeedometerCommands.Apply(commandSettings, 0, "1", "landing") && commandSettings.LandingSpeedOverlayEnabled, "Landing meter enables independently");
            Check(SpeedometerCommands.Apply(commandSettings, 1, "31", "landing") && commandSettings.LandingSpeedOverlayX == 31 && commandSettings.CurrentSpeedOverlayX == 3, "Targeted landing X preserves current X");
            Check(SpeedometerCommands.Apply(commandSettings, 2, "40", "") && commandSettings.CurrentSpeedOverlayY == 40 && commandSettings.LandingSpeedOverlayY == 40, "Both numbers share a horizontal baseline");
            Check(SpeedometerCommands.Apply(commandSettings, 2, "20", "current") && commandSettings.CurrentSpeedOverlayY == 20 && commandSettings.LandingSpeedOverlayY == 40, "Targeted current Y preserves landing Y");
            Check(SpeedometerCommands.Apply(commandSettings, 1, "100", "current") && commandSettings.CurrentSpeedOverlayX == 100 && commandSettings.LandingSpeedOverlayX == 31, "Targeted current X preserves landing X");
            Check(SpeedometerCommands.Apply(commandSettings, 2, "200", "landing") && commandSettings.LandingSpeedOverlayY == 200 && commandSettings.CurrentSpeedOverlayY == 20, "Targeted landing Y preserves current Y");
            Check(SpeedometerCommands.Apply(commandSettings, 1, "400", "") && commandSettings.CurrentSpeedOverlayX == 400 && commandSettings.LandingSpeedOverlayX == 400, "Untargeted X sets both coordinates");
            commandSettings.CurrentSpeedOverlayY = commandSettings.LandingSpeedOverlayY = 600;
            commandSettings.CurrentSpeedOverlayScale = commandSettings.LandingSpeedOverlayScale = 1;
            commandSettings.SpeedometerPositionsVersion = 0;
            Check(SpeedometerCommands.InitializeIndependentPositions(commandSettings) && commandSettings.CurrentSpeedOverlayX == 400 && commandSettings.LandingSpeedOverlayX == 560, "Old identical positions split once on upgrade");
            commandSettings.LandingSpeedOverlayX = 400;
            Check(!SpeedometerCommands.InitializeIndependentPositions(commandSettings) && commandSettings.LandingSpeedOverlayX == 400, "Intentional overlap is preserved after migration");
            Check(SpeedometerCommands.Apply(commandSettings, 3, "1.5", "") && commandSettings.CurrentSpeedOverlayScale == 1.5 && commandSettings.LandingSpeedOverlayScale == 1.5, "Size command changes both");
            Check(SpeedometerCommands.Apply(commandSettings, 3, "2", "landing") && commandSettings.CurrentSpeedOverlayScale == 2 && commandSettings.LandingSpeedOverlayScale == 2, "Both numbers share their size");
            Check(!SpeedometerCommands.Apply(commandSettings, 3, "NaN", ""), "NaN size rejected");
            Check(!SpeedometerCommands.Apply(commandSettings, 3, "0", ""), "Zero size rejected");
            Check(SpeedometerCommands.Apply(commandSettings, 1, "2500", "") && commandSettings.CurrentSpeedOverlayX == 2500, "Pixel coordinates above 100 are accepted");
            Check(!SpeedometerCommands.Apply(commandSettings, 1, "-1", ""), "Negative position rejected");
            Check(!SpeedometerCommands.Apply(commandSettings, 0, "2", ""), "Only 0 and 1 accepted for visibility");
            Check(!SpeedometerCommands.Apply(commandSettings, 0, "1", "other"), "Invalid target rejected");
            Check(!SpeedometerCommands.Apply(commandSettings, 9, "1", ""), "Unknown command rejected");
            Check(!SpeedometerCommands.Apply(commandSettings, 5, "2", "") && !SpeedometerCommands.Apply(commandSettings, 5, "1", "landing"), "Landing flag accepts only 0/1 with no target");
            Check(SpeedometerCommands.Apply(commandSettings, 6, new[] { "255", "0", "128", "current" }) && commandSettings.CurrentSpeedOverlayColor.ToArgb() == Color.FromArgb(255, 0, 128).ToArgb() && commandSettings.LandingSpeedOverlayColor.ToArgb() == Color.White.ToArgb(), "RGB command sets current independently");
            Check(SpeedometerCommands.Apply(commandSettings, 6, new[] { "0", "255", "0", "landing" }) && commandSettings.LandingSpeedOverlayColor.ToArgb() == Color.Lime.ToArgb() && commandSettings.CurrentSpeedOverlayColor.R == 255, "RGB command sets landing independently");
            var beforeColor = commandSettings.CurrentSpeedOverlayColor;
            Check(!SpeedometerCommands.Apply(commandSettings, 6, new[] { "256", "0", "0" }) && !SpeedometerCommands.Apply(commandSettings, 6, new[] { "0", "1.5", "0" }) && !SpeedometerCommands.Apply(commandSettings, 6, new[] { "0", "-1", "0" }) && !SpeedometerCommands.Apply(commandSettings, 6, new[] { "0", "0" }) && commandSettings.CurrentSpeedOverlayColor == beforeColor, "Invalid RGB channels never partly modify settings");
            Check(SpeedometerCommands.Apply(commandSettings, 6, new[] { "0", "0", "0" }) && commandSettings.CurrentSpeedOverlayColor.ToArgb() == Color.Black.ToArgb() && commandSettings.LandingSpeedOverlayColor.ToArgb() == Color.Black.ToArgb(), "Untargeted RGB applies to both, including black");
            Check(SpeedometerOverlay.ReadingText("", new SpeedometerReading(true, 0, 0, 42.5f)) == "42.50", "Display contains only a number with two decimal places");

            using var meter = new SpeedometerOverlay();
            var cp = (CreateParams) typeof(SpeedometerOverlay).GetProperty("CreateParams", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(meter)!;
            Check((cp.ExStyle & 0x080800A0) == 0x080800A0, "Overlay is layered, click-through, nonactivating, and a tool window");
            var viewport = new Rectangle(-1920, 80, 1920, 1080);
            var current = new SpeedometerReading(true, 3, 70, 42.5f);
            var landing = new SpeedometerReading(true, 100, 100, 123.4f);
            meter.UpdateDisplay(viewport, current, landing);
            Check(viewport.Contains(meter.Bounds), "Bottom-right positioning stays inside the game on a negative-origin monitor");
            var clamped = SpeedometerOverlay.ReadingBounds(viewport.Size, new SpeedometerReading(true, -5, 5000, null));
            Check(clamped.Left == 0 && clamped.Bottom == viewport.Height, "Saved out-of-range coordinates are clamped");
            Check(SpeedometerOverlay.ReadingText("Last landing", new SpeedometerReading(true, 0, 0, null)).Contains("--"), "No landing displays --");
            Check(SpeedometerOverlay.ReadingText("Last landing", new SpeedometerReading(true, 0, 0, null, false)).Contains("N/A"), "Unverified contact layout displays N/A");
            landing = new SpeedometerReading(true, 450, 300, 123.4f);
            Check(SpeedometerText.Format(current) == "42.50" && SpeedometerText.Format(landing) == "123.40", "Readings format independently with two decimals");
            Check(SpeedometerText.Format(current with { Speed = float.MaxValue }).Length < 32, "Exceptional finite values fit each native text buffer");
            byte[] records = NativeSpeedometer.CreateReadings(current, landing, 10);
            Check(BitConverter.ToInt32(records, 0) == 1 && BitConverter.ToInt32(records, 64) == 1 && BitConverter.ToSingle(records, 4) == 3 && BitConverter.ToSingle(records, 68) == 450 && BitConverter.ToSingle(records, 72) == 300, "Native data carries both independent coordinates");
            Check(BitConverter.ToSingle(records, 48) == 50 && BitConverter.ToSingle(records, 112) == 60, "Each native record has its own text width");
            meter.UpdateDisplay(viewport, current, landing);
            using(var font = new Font("Segoe UI", 22, FontStyle.Bold))
                Check(TextRenderer.MeasureText(SpeedometerOverlay.ReadingText("Last landing", landing), font).Width + 4 <= 420, "Landing label and units fit without clipping");
            using(var bitmap = new Bitmap(meter.Width, meter.Height)) {
                meter.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
                Check(HasWhiteText(bitmap, SpeedometerOverlay.ReadingBounds(viewport.Size, current)) && HasWhiteText(bitmap, SpeedometerOverlay.ReadingBounds(viewport.Size, landing)), "Both readings paint at their independent positions");
                bitmap.Save(Path.Combine(output, "both-overlays.png"));
            }
            meter.UpdateDisplay(viewport, current with { Enabled = false }, landing);
            using(var bitmap = new Bitmap(meter.Width, meter.Height)) {
                meter.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
                Check(HasWhiteText(bitmap, SpeedometerOverlay.ReadingBounds(viewport.Size, landing)) && !HasWhiteText(bitmap, SpeedometerOverlay.ReadingBounds(viewport.Size, current)), "Hiding current keeps landing at its position and clears current pixels");
            }
            meter.UpdateDisplay(viewport, current, landing with { Enabled = false });
            using(var bitmap = new Bitmap(meter.Width, meter.Height)) {
                meter.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
                Check(HasWhiteText(bitmap, SpeedometerOverlay.ReadingBounds(viewport.Size, current)) && !HasWhiteText(bitmap, SpeedometerOverlay.ReadingBounds(viewport.Size, landing)), "Hiding landing keeps current at its position and clears landing pixels");
            }
            meter.UpdateDisplay(viewport, default, default);
            Check(!meter.Visible, "Disabling both hides the surface");
            meter.Hide();
            meter.UpdateDisplay(new Rectangle(10, 20, 180, 35), current, default);
            Check(meter.Bounds == new Rectangle(10, 20, 180, 35), "Small client area bounds are respected");
            meter.UpdateDisplay(viewport, current with { TextColor = Color.Red }, landing with { TextColor = Color.Lime });
            using(var bitmap = new Bitmap(meter.Width, meter.Height)) {
                meter.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
                Check(HasColor(bitmap, SpeedometerOverlay.ReadingBounds(viewport.Size, current), Color.Red) && HasColor(bitmap, SpeedometerOverlay.ReadingBounds(viewport.Size, landing), Color.Lime), "Desktop fallback paints both requested RGB colors independently");
            }
            meter.UpdateDisplay(viewport, current with { TextColor = Color.Magenta }, landing with { TextColor = Color.Cyan });
            using(var bitmap = new Bitmap(meter.Width, meter.Height)) {
                meter.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
                Check(meter.TransparencyKey != Color.Magenta && meter.TransparencyKey != Color.Cyan && HasColor(bitmap, SpeedometerOverlay.ReadingBounds(viewport.Size, current), Color.Magenta), "RGB colors that match common transparency keys remain visible");
            }
            meter.Hide();
            Console.WriteLine($"PASS: {_checks} logic and UI checks; previews: {output}");
            return 0;
        } catch(Exception e) {
            Console.Error.WriteLine(e);
            return 1;
        }
    }

    private static bool HasWhiteText(Bitmap bitmap, Rectangle bounds) {
        for(int y = bounds.Top; y < bounds.Bottom; y++)
            for(int x = bounds.Left; x < bounds.Right; x++) {
                var pixel = bitmap.GetPixel(x, y);
                if(pixel.R > 240 && pixel.G > 240 && pixel.B > 240) return true;
            }
        return false;
    }

    private static bool HasColor(Bitmap bitmap, Rectangle bounds, Color color) {
        for(int y = bounds.Top; y < bounds.Bottom; y++)
            for(int x = bounds.Left; x < bounds.Right; x++)
                if(bitmap.GetPixel(x, y).ToArgb() == color.ToArgb()) return true;
        return false;
    }

    private static void VerifySampler() {
        using var self = Process.GetCurrentProcess();
        IntPtr memory = self.AllocateMemory(4096);
        try {
            using var sampler = new HorizontalSpeedSampler(self);
            var sample = new byte[137];
            BitConverter.GetBytes(3f).CopyTo(sample, 0); BitConverter.GetBytes(4f).CopyTo(sample, 4); sample[136] = 1;
            self.WriteBytes(memory, sample); sampler.SetContext(memory, 136, true);
            Check(SpinWait.SpinUntil(() => sampler.Snapshot.Current == 5, 1000), "Background reader publishes horizontal speed independently of the UI timer");
            self.WriteBytes(memory + 136, new byte[] { 0 }); Thread.Sleep(40);
            BitConverter.GetBytes(12f).CopyTo(sample, 0); BitConverter.GetBytes(16f).CopyTo(sample, 4);
            self.WriteBytes(memory, sample);
            Check(SpinWait.SpinUntil(() => sampler.Snapshot.LastLanding == 20, 1000), "Background reader detects a landing and publishes an immutable snapshot");
            sampler.SetContext(IntPtr.Zero, 136, false);
            Check(SpinWait.SpinUntil(() => sampler.Snapshot == HorizontalSpeedSnapshot.Empty, 1000), "Disabling/loading clears the background reading");
        } finally { self.FreeMemory(memory); }
    }

    [System.Runtime.InteropServices.DllImport("winmm.dll")] private static extern uint timeBeginPeriod(uint period);
    [System.Runtime.InteropServices.DllImport("winmm.dll")] private static extern uint timeEndPeriod(uint period);
    private static int LandingTrace() {
        using var game = Process.GetProcessesByName("DOOMEternalx64vk").First(p => p.MainModule!.ModuleMemorySize == 492113920);
        var pointer = new DeepPointer("DOOMEternalx64vk.exe", 80143112, 0x1510, 0x598, 0x1D0, 0x3F40);
        var rows = new List<string> { "ms,ground,x,y,z,speed" };
        var watch = Stopwatch.StartNew();
        IntPtr address = IntPtr.Zero;
        long nextPointer = 0;
        bool timer = timeBeginPeriod(1) == 0;
        try {
            while(watch.Elapsed.TotalSeconds < 60) {
                if(watch.ElapsedMilliseconds >= nextPointer) { pointer.DerefOffsets(game, out address); nextPointer = watch.ElapsedMilliseconds + 16; }
                if(address != IntPtr.Zero && game.ReadBytes(address, 137, out var data) && data[136] <= 1) {
                    float x = BitConverter.ToSingle(data, 0), y = BitConverter.ToSingle(data, 4), z = BitConverter.ToSingle(data, 8);
                    rows.Add(FormattableString.Invariant($"{watch.Elapsed.TotalMilliseconds:F3},{data[136]},{x:R},{y:R},{z:R},{Math.Sqrt((double)x*x+(double)y*y):F6}"));
                }
                Thread.Sleep(1);
            }
        } finally { if(timer) timeEndPeriod(1); }
        File.WriteAllLines("artifacts/landing-trace.csv", rows);
        Console.WriteLine($"Recorded {rows.Count - 1} samples");
        return 0;
    }

    private static int ProbeGame(bool showOverlay, bool nativeMode) {
        // Read-only probe: deliberately does not construct MemoryHandler (which also writes cvars).
        using var process = Process.GetProcessesByName("DOOMEternalx64vk").First(p => {
            try { return p.MainModule!.ModuleMemorySize == 492113920; } catch { return false; }
        });
        var pointer = new DeepPointer("DOOMEternalx64vk.exe", 80143112, 0x1510, 0x598, 0x1D0, 0x3F40);
        var tracker = new HorizontalSpeedTracker();
        var clock = Stopwatch.StartNew();
        int valid = 0, invalid = 0, groundedSamples = 0, airborneSamples = 0, landings = 0;
        bool? lastGround = null;
        IntPtr lastPointer = IntPtr.Zero;
        var events = new List<object>();
        float maxSpeed = 0;
        int displayedSamples = 0;
        using var overlay = new SpeedometerOverlay();
        using var native = nativeMode ? NativeSpeedometer.Attach(process) : null;
        using var sampler = nativeMode ? new HorizontalSpeedSampler(process) : null;
        var settings = Settings.Default;
        if(SpeedometerCommands.InitializeIndependentPositions(settings)) settings.Save();
        if(nativeMode) settings.CurrentSpeedOverlayEnabled = settings.LandingSpeedOverlayEnabled = true;
        var consoleCommands = new List<object>();
        while(clock.Elapsed.TotalSeconds < (nativeMode ? 300 : 30) && (!nativeMode || !File.Exists("artifacts/stop-native-check"))) {
            if(pointer.DerefOffsets(process, out var address) && process.ReadBytes(address, 137, out var sample) && sample[136] <= 1) {
                if(address != lastPointer) { tracker.Reset(); lastGround = null; }
                lastPointer = address;
                sampler?.SetContext(address, 136, true);
                float x = BitConverter.ToSingle(sample, 0), y = BitConverter.ToSingle(sample, 4), z = BitConverter.ToSingle(sample, 8);
                bool ground = sample[136] != 0;
                tracker.Update(x, y, ground);
                if(ground) groundedSamples++; else airborneSamples++;
                if(lastGround != ground) events.Add(new { seconds = Math.Round(clock.Elapsed.TotalSeconds, 3), ground, x, y, z, speed = tracker.Current, landing = tracker.LastLanding });
                if(lastGround == false && ground) landings++;
                lastGround = ground;
                if(tracker.Current.HasValue) maxSpeed = Math.Max(maxSpeed, tracker.Current.Value);
                if(native != null) {
                    foreach(var command in native.ReadCommands()) {
                        if(command.Arguments.Length == 0) {
                            native.QueueConsoleMessage(SpeedometerCommandHelp.Format(settings, command.Command));
                            continue;
                        }
                        bool applied = SpeedometerCommands.Apply(settings, command.Command, command.Arguments);
                        consoleCommands.Add(new { command.Command, command.Arguments, applied });
                        if(applied) settings.Save();
                    }
                    var speed = sampler!.Snapshot;
                    native.Update(new SpeedometerReading(settings.SpeedometersEnabled && settings.CurrentSpeedOverlayEnabled, settings.CurrentSpeedOverlayX, settings.CurrentSpeedOverlayY, speed.Current, true, settings.CurrentSpeedOverlayScale, settings.CurrentSpeedOverlayColor),
                        new SpeedometerReading(settings.SpeedometersEnabled && settings.LandingSpeedOverlayEnabled, settings.LandingSpeedOverlayX, settings.LandingSpeedOverlayY, speed.LastLanding, true, settings.LandingSpeedOverlayScale, settings.LandingSpeedOverlayColor), settings.LowHpTintOpacity);
                    displayedSamples++;
                }
                if(showOverlay && DESpeedrunUtil.Interop.DLLImports.GetForegroundWindow() == process.MainWindowHandle &&
                    SpeedometerOverlay.TryGetViewport(process.MainWindowHandle, out var viewport)) {
                    overlay.UpdateDisplay(viewport, new SpeedometerReading(true, 3, 70, tracker.Current), new SpeedometerReading(true, 3, 77, tracker.LastLanding));
                    displayedSamples++;
                } else overlay.Hide();
                valid++;
            } else { invalid++; tracker.Reset(); lastGround = null; }
            Thread.Sleep(16);
            if(showOverlay) Application.DoEvents();
        }
        var result = System.Text.Json.JsonSerializer.Serialize(new { processId = process.Id, moduleSize = process.MainModule!.ModuleMemorySize,
            valid, invalid, groundedSamples, airborneSamples, landings, maxSpeed, displayedSamples, nativeFrames = native?.RenderCount,
            nativeCommandsRegistered = native?.CommandsRegistered, tintAttached = native?.TintAttached, consoleCommands, events }, new System.Text.Json.JsonSerializerOptions { WriteIndented = true });
        Directory.CreateDirectory("artifacts");
        File.WriteAllText(nativeMode ? "artifacts/native-overlay.json" : showOverlay ? "artifacts/live-overlay.json" : "artifacts/live-probe.json", result);
        Console.WriteLine(result);
        return valid > 0 && groundedSamples > 0 && airborneSamples > 0 && landings > 0 ? 0 : 1;
    }
}
