using DESpeedrunUtil.Memory;
using Serilog;
using Settings = DESpeedrunUtil.Properties.Settings;
using static DESpeedrunUtil.Define.Constants;

namespace DESpeedrunUtil {
    public partial class MainWindow {
        private readonly CheckBox lightingFixCheckbox = new();
        private readonly Label lightingFixStatus = new();
        private readonly Label lightingFixStatusLabel = new();
        private LiveVisualFeatures? _visualFeatures;
        private bool _visualBusy;
        private string? _visualError;
        private long _visualRetry;
        // Viewer visibility is deliberately local to this game session; old saved
        // checkbox values never start rendering on launch or after disabling cheats.
        private bool _volumesShown;
        private bool VolumeCommandsEnabled => Hooked && Memory?.EnableCheats == true;

        private void InitializeVisualFeatures() {
            int oldBottom = panel2.Bottom;
            foreach(Control control in collapsiblePanel.Controls) {
                if(control.Top >= oldBottom) control.Top += 27;
                else if(control.Left >= 316 && control.Top >= statusPanel.Bottom) control.Top += 27;
            }
            statusPanel.Height += 27;
            int visualOptionsTop = firewallToggleButton.Top;
            firewallToggleButton.Top += 27;
            cheatsToggleButton.Top += 27;
            panel2.Height += 27; collapsiblePanel.Height += 27;
            ClientSize = new Size(ClientSize.Width, ClientSize.Height + 27);
            void Status(Label title, Label value, string name, string text, int y) {
                title.Name = name + "StatusLabel"; title.Text = text; title.AutoSize = true;
                title.Location = new Point(3,y); title.ForeColor = COLOR_TEXT_FORE; title.Font = rtssStatusLabel.Font;
                value.Name = name + "Status"; value.Text = "Disabled"; value.AutoSize = true;
                value.Location = new Point(171,y); value.ForeColor = COLOR_TEXT_FORE; value.Font = rtssStatus.Font;
                statusPanel.Controls.Add(title); statusPanel.Controls.Add(value);
            }
            Status(lightingFixStatusLabel,lightingFixStatus,"lightingFix","Lighting Fix:",275);
            void Option(CheckBox box, string name, string text, bool value, int y) {
                box.Name = name; box.Text = text; box.AutoSize = true; box.Location = new Point(6,y);
                box.ForeColor = COLOR_TEXT_FORE; box.Font = enableOSDCheckbox.Font; box.UseVisualStyleBackColor = true;
                box.Checked = value; box.CheckedChanged += VisualOptionChanged; panel2.Controls.Add(box);
            }
            Option(lightingFixCheckbox,"lightingFixCheckbox","Enable Lighting Fix",Settings.Default.LightingFixEnabled,visualOptionsTop-1);
            toolTip7500.SetToolTip(lightingFixCheckbox,"Restore missing lighting. Applies to the running game and future direct launches while DESRU is running.");
            (panel2.Location,trainerPanel.Location)=(trainerPanel.Location,panel2.Location);
            trainerPanel.Height=panel2.Bottom-trainerPanel.Top;
            (optionsTitle.Location,trainerTitle.Location)=(trainerTitle.Location,optionsTitle.Location);
        }
        private void SetVisualFeatureFonts() {
            lightingFixCheckbox.Font = EternalUIRegular;
            lightingFixStatus.Font = EternalUIRegular;
            lightingFixStatusLabel.Font = EternalUIBold;
        }
        private void VisualOptionChanged(object? sender, EventArgs e) {
            Settings.Default.LightingFixEnabled = lightingFixCheckbox.Checked;
            Settings.Default.Save(); _visualError = null; _visualRetry = 0; UpdateVisualFeatures();
        }
        private int VisualRequest => LiveVisualFeatures.Request(lightingFixCheckbox.Checked, VolumeCommandsEnabled,
            _volumesShown, Settings.Default.CollisionVolumesFill, Settings.Default.CollisionTriggersVisible,
            Settings.Default.CollisionGeometryVisible, Settings.Default.CollisionWallsVisible);

        private void UpdateVisualFeatures() {
            if(!VolumeCommandsEnabled) _volumesShown = false;
            int desired = VisualRequest;
            if(_visualFeatures != null) {
                try {
                    _visualFeatures.Update(desired,Settings.Default.CollisionRenderDistance);
                    var state = _visualFeatures.State;
                    if(VolumeCommandsEnabled) {
                        _volumesShown = (state.Requested & 2) != 0;
                        bool triggers = (state.Requested & 4) != 0, geometry = (state.Requested & 8) != 0, walls = (state.Requested & 16) != 0;
                        bool fill = (state.Requested & 32) != 0;
                        if(Settings.Default.CollisionTriggersVisible != triggers || Settings.Default.CollisionGeometryVisible != geometry || Settings.Default.CollisionWallsVisible != walls || Settings.Default.CollisionVolumesFill != fill || Settings.Default.CollisionRenderDistance!=state.RenderDistance) {
                            Settings.Default.CollisionTriggersVisible = triggers; Settings.Default.CollisionGeometryVisible = geometry; Settings.Default.CollisionWallsVisible = walls;
                            Settings.Default.CollisionVolumesFill = fill;
                            if(float.IsFinite(state.RenderDistance)&&state.RenderDistance>=0) Settings.Default.CollisionRenderDistance=state.RenderDistance;
                            Settings.Default.Save();
                        }
                    }
                    if(state.State < 0) _visualError = "Visual module could not attach (code " + state.Error + ")";
                } catch(Exception error) { _visualError = error.Message; Log.Warning(error,"Could not update visual features"); }
            }
            desired = VisualRequest;
            if(Hooked && GameProcess != null && _nativeSpeedometer != null && !_visualBusy && Environment.TickCount64 >= _visualRetry &&
                ((_visualFeatures == null && ((desired & 3) != 0 || VolumeCommandsEnabled)) ||
                 (_visualFeatures != null && ((VolumeCommandsEnabled && !_visualFeatures.CommandsRegistered) ||
                  ((desired & 3) != 0 && !_visualFeatures.RuntimeStarted) || _visualFeatures.RefreshNeeded))))
                _ = AttachVisualFeaturesAsync();
            int active = (_visualFeatures?.State.Active ?? 0) & desired;
            Memory?.SetFlag((active & 1) != 0,"lightingfix");
            void Status(Label label, bool enabled, int bit) {
                string? error=_visualError;
                if(bit==2&&_visualFeatures?.State.State==2) {
                    if(_visualFeatures.State.Error==4) error="Collision viewer stopped after a rendering error. See the visual module log.";
                    else if(_visualFeatures.State.Profile==0) error="Could not resolve this game's camera and entity profile.";
                }
                label.Text = !enabled ? "Disabled" : !Hooked ? "Waiting" : error != null ? "Error" : (active & bit) != 0 ? "Enabled" : "Starting";
                label.ForeColor = label.Text == "Enabled" ? Color.Lime : label.Text == "Error" ? Color.OrangeRed : enabled ? Color.Gold : COLOR_TEXT_FORE;
                toolTip7500.SetToolTip(label,error ?? (!Hooked ? "Waiting for the game. The saved option will apply automatically." : label.Text));
            }
            Status(lightingFixStatus,lightingFixCheckbox.Checked,1);
        }
        private async Task AttachVisualFeaturesAsync() {
            var game = GameProcess; var native = _nativeSpeedometer;
            if(game == null || native == null) return;
            _visualBusy = true;
            try {
                if(_visualFeatures == null) {
                    var attached = await LiveVisualFeatures.AttachAsync(game,native,Path.Combine(AppContext.BaseDirectory,"visuals","DESRU.Visuals.Runtime.dll"),startRuntime:false);
                    if(GameProcess != game) { attached.Dispose(); return; }
                    _visualFeatures = attached; Log.Information("Native visual feature module attached to the running game");
                }
                _visualFeatures.Update(VisualRequest,Settings.Default.CollisionRenderDistance);
                if(VolumeCommandsEnabled) await _visualFeatures.RegisterCommandsAsync();
                if((VisualRequest & 3) != 0) await _visualFeatures.StartRuntimeAsync();
                if(VolumeCommandsEnabled && _volumesShown) {
                    if(_visualFeatures.RefreshNeeded) {
                        Log.Information("Refreshing graphics resources for the live collision viewer");
                        await _visualFeatures.RefreshRendererAsync();
                    }
                }
                _visualError = null;
            } catch(Exception error) {
                if(GameProcess != game) return;
                _visualError = error.Message; _visualRetry = Environment.TickCount64 + 5000;
                Log.Warning(error,"Could not apply native visual features");
            } finally { _visualBusy = false; }
        }
        private void ReleaseVisualFeatures() {
            _visualFeatures?.Dispose(); _visualFeatures = null; _visualError = null; _visualRetry = 0;
            _volumesShown = false;
        }
    }
}
