using DESpeedrunUtil.Overlay;
using Serilog;
using DESpeedrunUtil.Memory;

namespace DESpeedrunUtil {
    public partial class MainWindow {
        private SpeedometerOverlay? _speedOverlay;
        private NativeSpeedometer? _nativeSpeedometer;
        private readonly NativeAttachRetry _nativeAttachRetry = new();
        private long _nextNativeAttachLog;
        private ProtectedCvarMonitor? _cvarMonitor;
        private Task<IReadOnlyList<CvarDifference>>? _cvarTask;
        private IReadOnlyList<CvarDifference> _changedCvars = Array.Empty<CvarDifference>();
        private long _lastCvarRead;
        private LiveMeathook? _liveMeathook;
        private bool _switchingCheats;
        private CheatCvarSession? _cheatCvarSession;
        private Task? _cheatSessionBegin;
        private PlayerCheatMonitor? _playerCheatMonitor;
        private IReadOnlyList<CvarDifference>? _displayCvarSource;
        private PlayerCheatState _displayPlayerState;
        private IReadOnlyList<CvarDifference> _displayCheatRows = Array.Empty<CvarDifference>();

        private void InitializeSpeedOverlays() {
            _speedOverlay = new SpeedometerOverlay();
        }

        private void HideSpeedOverlays() {
            _speedOverlay?.Hide();
        }

        private void ReleaseSpeedOverlays() {
            Memory?.StopSpeedSampler();
            _nativeSpeedometer?.Dispose();
            _nativeSpeedometer = null;
            _nativeAttachRetry.Reset(); _nextNativeAttachLog = 0;
            _cvarMonitor = null; _cvarTask = null; _changedCvars = Array.Empty<CvarDifference>(); _liveMeathook = null; _cheatCvarSession = null; _cheatSessionBegin = null;
            _playerCheatMonitor = null; _displayCvarSource = null; _displayCheatRows = Array.Empty<CvarDifference>();
            HideSpeedOverlays();
        }

        private void UpdateSpeedOverlays() {
            var settings = Properties.Settings.Default;
            if(_speedOverlay == null) return;
            if(Memory == null || GameProcess == null) { HideSpeedOverlays(); return; }
            if(SpeedometerCommands.InitializeIndependentPositions(settings)) settings.Save();
            var speed = Memory.HorizontalSpeed;
            if(_nativeSpeedometer == null && _nativeAttachRetry.CanAttempt(Environment.TickCount64)) {
                try {
                    _nativeSpeedometer = NativeSpeedometer.Attach(GameProcess);
                    Memory.SetGroundContactOffset(_nativeSpeedometer.GroundPlaneFromVelocity);
                    if(_nativeSpeedometer.Layout.Cvars != null) _cvarMonitor = new ProtectedCvarMonitor(GameProcess, _nativeSpeedometer.Layout.Cvars);
                    if(_nativeSpeedometer.Layout.PlayerCheats != null) _playerCheatMonitor = new PlayerCheatMonitor(GameProcess, _nativeSpeedometer.Layout.PlayerCheats);
                    try {
                        _liveMeathook = new LiveMeathook(GameProcess, _nativeSpeedometer.Extension, _nativeSpeedometer.Layout);
                        if(_cvarMonitor != null) _cheatCvarSession = new CheatCvarSession(GameProcess, _nativeSpeedometer.Extension, _cvarMonitor);
                        if(_liveMeathook.Active) { Memory.SetFlag(true, "meath00k"); Memory.EnableCheats = true; }
                        if(_cheatCvarSession?.Active == true) Memory.EnableCheats = true;
                        if(Memory.EnableCheats && _cheatCvarSession != null) _cheatSessionBegin = _cheatCvarSession.BeginAsync();
                    } catch(Exception e) { Log.Warning(e, "Live cheat control is unavailable; native speedometers remain active"); }
                    Log.Information("Native speedometers and console commands attached");
                    _nativeAttachRetry.Reset(); _nextNativeAttachLog = 0;
                } catch(NotSupportedException e) {
                    _nativeAttachRetry.RejectUnsupported();
                    Log.Warning(e, "Native speedometers are unavailable on this game build; using the desktop fallback");
                } catch(Exception e) {
                    _nativeAttachRetry.RetryLater(Environment.TickCount64);
                    if(Environment.TickCount64 >= _nextNativeAttachLog) {
                        _nextNativeAttachLog = Environment.TickCount64 + 30000;
                        if(e is NativeGameNotReadyException) Log.Information("Waiting for game initialization before attaching native speedometers: {Reason}", e.Message);
                        else Log.Warning(e, "Native speedometer attachment will be retried");
                    }
                }
            }
            if(_nativeSpeedometer != null) {
                try {
                    bool changed = false;
                    foreach(var command in _nativeSpeedometer.ReadCommands()) {
                        if(command.Arguments.Length == 0) {
                            _nativeSpeedometer.QueueConsoleMessage(SpeedometerCommandHelp.Format(settings, command.Command));
                            continue;
                        }
                        bool applied = SpeedometerCommands.Apply(settings, command.Command, command.Arguments);
                        changed |= applied;
                        if(!applied) Log.Warning("Invalid DESRU console command {Command}: {Arguments}", command.Command, string.Join(" ", command.Arguments));
                    }
                    if(changed) settings.Save();
                    bool show = settings.SpeedometersEnabled && _gameInFocus && !Memory.IsLoadingOrInMenu() && speed.Current.HasValue;
                    _nativeSpeedometer.Update(
                        new SpeedometerReading(show && settings.CurrentSpeedOverlayEnabled, settings.CurrentSpeedOverlayX, settings.CurrentSpeedOverlayY,
                            speed.Current, true, settings.CurrentSpeedOverlayScale, settings.CurrentSpeedOverlayColor),
                        new SpeedometerReading(show && settings.LandingSpeedOverlayEnabled, settings.LandingSpeedOverlayX, settings.LandingSpeedOverlayY,
                            speed.LastLanding, Memory.GroundContactSupported, settings.LandingSpeedOverlayScale, settings.LandingSpeedOverlayColor), settings.LowHpTintOpacity, Memory.ReadPlayerAddress());
                    UpdateProtectedCvarDisplay();
                } catch(Exception e) {
                    Log.Warning(e, "Native speedometer update stopped; the original HUD remains active");
                    _nativeSpeedometer.Dispose();
                    _nativeSpeedometer = null;
                    _nativeAttachRetry.RetryLater(Environment.TickCount64);
                }
                HideSpeedOverlays();
                return;
            }
            // A supported game that is still starting must not get permanently
            // replaced by the desktop renderer (which uses a different font).
            if(!_nativeAttachRetry.Unsupported) { HideSpeedOverlays(); return; }
            if(!settings.SpeedometersEnabled || !_gameInFocus || Memory.IsLoadingOrInMenu() ||
                !speed.Current.HasValue ||
                !SpeedometerOverlay.TryGetViewport(GameProcess.MainWindowHandle, out var viewport)) {
                HideSpeedOverlays();
                return;
            }
            _speedOverlay.UpdateDisplay(viewport,
                new SpeedometerReading(settings.CurrentSpeedOverlayEnabled, settings.CurrentSpeedOverlayX, settings.CurrentSpeedOverlayY, speed.Current, true, settings.CurrentSpeedOverlayScale, settings.CurrentSpeedOverlayColor),
                new SpeedometerReading(settings.LandingSpeedOverlayEnabled, settings.LandingSpeedOverlayX, settings.LandingSpeedOverlayY,
                    speed.LastLanding, Memory.GroundContactSupported, settings.LandingSpeedOverlayScale, settings.LandingSpeedOverlayColor));
        }

        private void UpdateProtectedCvarDisplay() {
            if(_cvarMonitor == null || _nativeSpeedometer == null || Memory == null || GameProcess == null) return;
            if(_cvarTask?.IsCompleted == true) {
                if(_cvarTask.IsCompletedSuccessfully) _changedCvars = _cvarTask.Result;
                else Log.Warning(_cvarTask.Exception, "Could not read protected cvars");
                _cvarTask = null;
            }
            if(_cvarTask == null && Environment.TickCount64 - _lastCvarRead >= 500) {
                var monitor = _cvarMonitor; _lastCvarRead = Environment.TickCount64;
                _cvarTask = Task.Run(monitor.Read);
            }
            if(!SpeedometerOverlay.TryGetPhysicalViewport(GameProcess.MainWindowHandle, out var viewport)) return;
            var displayed = _cheatCvarSession?.DisplayChanges(_changedCvars) ?? Array.Empty<CvarDifference>();
            var playerState = Memory.EnableCheats || Memory.GetFlag("meath00k") ? _playerCheatMonitor?.Read(Memory.ReadPlayerAddress()) ?? default : default;
            if(!ReferenceEquals(displayed, _displayCvarSource) || playerState != _displayPlayerState) {
                _displayCvarSource = displayed; _displayPlayerState = playerState; _displayCheatRows = playerState.AddTo(displayed);
            }
            var virtualViewport = _nativeSpeedometer.Extension.VirtualViewport;
            float glyphWidth = _nativeSpeedometer.GlyphWidth, glyphHeight = _nativeSpeedometer.GlyphHeight;
            var anchor = PerformanceMetricsAnchor.BelowModifiers(viewport, virtualViewport.Width, virtualViewport.Height, glyphHeight, Memory.MinimalOSD);
            _nativeSpeedometer.Extension.Update(_displayCheatRows, viewport, anchor.Top, anchor.Scale, glyphWidth, glyphHeight,
                Memory.EnableOSD && _gameInFocus && !Memory.IsLoadingOrInMenu(), true, anchor.Right);
        }

        private void UpdateLiveCheatsButton() {
            if(IsDisposed || Disposing || Memory == null) return;
            bool enabled = Memory.EnableCheats || Memory.GetFlag("meath00k") || _liveMeathook?.Active == true;
            cheatsToggleButton.Text = _switchingCheats ? "Switching..." : enabled ? "Disable Cheats / Meath00k" : "Enable Cheats / Meath00k";
            cheatsToggleButton.Enabled = Hooked && !_switchingCheats && _liveMeathook != null && _cheatCvarSession != null;
        }

        private async Task SwitchLiveCheats() {
            if(Memory == null || GameProcess == null || _switchingCheats) return;
            var memory = Memory;
            var game = GameProcess;
            bool wasEnabled = memory.EnableCheats;
            bool enabling = false;
            _switchingCheats = true; UpdateLiveCheatsButton();
            try {
                if(_cheatSessionBegin != null) { var begin = _cheatSessionBegin; _cheatSessionBegin = null; await begin; }
                if(Memory != memory || GameProcess != game || game.HasExited) return;
                if(_cheatCvarSession == null) throw new NotSupportedException("Protected cvar tracking is unavailable on this game build");
                bool disable = memory.EnableCheats || memory.GetFlag("meath00k") || _liveMeathook?.Active == true;
                if(disable) {
                    if(_cvarMonitor == null || _nativeSpeedometer == null || _cheatCvarSession == null) throw new NotSupportedException("Cannot safely restore protected cvars on this game build");
                    if(!_cheatCvarSession.Active) await _cheatCvarSession.BeginAsync(); // adopt an already running startup proxy without guessing its previous settings
                    Memory.EnableCheats = true;
                    int restored = await _cheatCvarSession.RestoreAsync();
                    // These are player state, rather than cvars, but must not remain active.
                    if(_liveMeathook != null) await _liveMeathook.ClearPlayerCheatsAsync(Memory.ReadPlayerAddress());
                    if(_liveMeathook?.Active == true) {
                        _liveMeathook.Disable();
                    } else if(Memory.GetFlag("meath00k")) {
                        throw new NotSupportedException("The startup Meathook is not managed by live control in this session");
                    }
                    Memory.EnableCheats = false; Memory.SetFlag(false, "meath00k");
                    _cheatCvarSession.End();
                    _changedCvars = await Task.Run(_cvarMonitor.Read);
                    Log.Information("Live cheats disabled; restored {Count} protected cvars changed during the session", restored);
                } else {
                    enabling = true;
                    if(_liveMeathook == null) throw new NotSupportedException("Native live control is unavailable");
                    await _cheatCvarSession.BeginAsync();
                    memory.EnableCheats = true;
                    await _liveMeathook.EnableAsync(Path.Combine(AppContext.BaseDirectory, "meath00k", "XINPUT1_3.dll"));
                    Memory.EnableCheats = true; Memory.SetFlag(true, "meath00k");
                    Log.Information("Cheats and Meathook enabled together without restarting the game");
                }
            } catch(Exception e) {
                if(Memory != memory || GameProcess != game) { Log.Warning(e, "The game session ended while switching cheats"); return; }
                if(_liveMeathook?.Active == true) { Memory.EnableCheats = true; Memory.SetFlag(true, "meath00k"); }
                else if(enabling) memory.EnableCheats = wasEnabled;
                if(!Memory.EnableCheats && !Memory.GetFlag("meath00k") && _cheatCvarSession?.Active == true) {
                    try { _cheatCvarSession.End(); } catch(Exception endError) { Log.Warning(endError, "Could not close unused cvar snapshot"); }
                }
                Log.Error(e, "Could not switch live cheats");
                if(!IsDisposed && !Disposing) MessageBox.Show(this, e.Message, "Cheats / Meath00k", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            } finally {
                _switchingCheats = false;
                UpdateLiveCheatsButton();
            }
        }
    }
}
