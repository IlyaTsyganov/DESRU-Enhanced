using DESpeedrunUtil.Overlay;
using Serilog;

namespace DESpeedrunUtil {
    public partial class MainWindow {
        private SpeedometerOverlay? _speedOverlay;
        private NativeSpeedometer? _nativeSpeedometer;
        private bool _nativeAttachFailed;

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
            _nativeAttachFailed = false;
            HideSpeedOverlays();
        }

        private void UpdateSpeedOverlays() {
            var settings = Properties.Settings.Default;
            if(_speedOverlay == null) return;
            if(Memory == null || GameProcess == null) { HideSpeedOverlays(); return; }
            if(SpeedometerCommands.InitializeIndependentPositions(settings)) settings.Save();
            var speed = Memory.HorizontalSpeed;
            if(_nativeSpeedometer == null && !_nativeAttachFailed) {
                try {
                    _nativeSpeedometer = NativeSpeedometer.Attach(GameProcess);
                    Memory.SetGroundContactOffset(_nativeSpeedometer.GroundPlaneFromVelocity);
                    Log.Information("Native speedometers and console commands attached");
                } catch(Exception e) {
                    _nativeAttachFailed = true;
                    Log.Warning(e, "Native speedometers are unavailable on this game build; using the desktop fallback");
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
                } catch(Exception e) {
                    Log.Warning(e, "Native speedometer update stopped; the original HUD remains active");
                    _nativeSpeedometer.Dispose();
                    _nativeSpeedometer = null;
                    _nativeAttachFailed = true;
                }
                HideSpeedOverlays();
                return;
            }
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
    }
}
