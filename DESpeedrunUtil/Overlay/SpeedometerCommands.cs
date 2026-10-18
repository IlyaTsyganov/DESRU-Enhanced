using System.Globalization;
using Settings = DESpeedrunUtil.Properties.Settings;

namespace DESpeedrunUtil.Overlay {
    internal static class SpeedometerCommands {
        internal static bool Apply(Settings settings, int command, IReadOnlyList<string> arguments) {
            if(command != 6) return arguments.Count >= 1 && arguments.Count <= 2 && Apply(settings, command, arguments[0], arguments.Count == 2 ? arguments[1] : "");
            if(arguments.Count != 3 && arguments.Count != 4) return false;
            string target = arguments.Count == 4 ? arguments[3] : "";
            bool current = target == "" || target.Equals("current", StringComparison.OrdinalIgnoreCase);
            bool landing = target == "" || target.Equals("landing", StringComparison.OrdinalIgnoreCase);
            if(!current && !landing) return false;
            int[] channels = new int[3];
            for(int i = 0; i < 3; i++)
                if(!int.TryParse(arguments[i], NumberStyles.None, CultureInfo.InvariantCulture, out channels[i]) || channels[i] < 0 || channels[i] > 255) return false;
            var color = Color.FromArgb(channels[0], channels[1], channels[2]);
            if(current) settings.CurrentSpeedOverlayColor = color;
            if(landing) settings.LandingSpeedOverlayColor = color;
            return true;
        }
        internal static bool InitializeIndependentPositions(Settings settings) {
            if(settings.SpeedometerPositionsVersion >= 1) return false;
            // The previous single-string renderer stored the same position for both values.
            // Split that old layout once; afterwards even intentionally identical positions persist.
            if(settings.CurrentSpeedOverlayX == settings.LandingSpeedOverlayX &&
                settings.CurrentSpeedOverlayY == settings.LandingSpeedOverlayY) {
                double scale = double.IsFinite(settings.CurrentSpeedOverlayScale) ? Math.Clamp(settings.CurrentSpeedOverlayScale, 0.1, 10) : 1;
                settings.LandingSpeedOverlayX = (int) Math.Min(int.MaxValue,
                    (long) Math.Max(0, settings.CurrentSpeedOverlayX) + (int) Math.Ceiling(160 * scale));
            }
            settings.SpeedometerPositionsVersion = 1;
            return true;
        }

        internal static bool Apply(Settings settings, int command, string value, string target) {
            bool current = target == "" || target.Equals("current", StringComparison.OrdinalIgnoreCase);
            bool landing = target == "" || target.Equals("landing", StringComparison.OrdinalIgnoreCase);
            if(!current && !landing) return false;
            if(!double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out double number) || !double.IsFinite(number)) return false;
            switch(command) {
                case 0 when number == 0 || number == 1:
                    if(target == "") {
                        settings.SpeedometersEnabled = number == 1;
                        if(number == 1) settings.CurrentSpeedOverlayEnabled = true;
                        return true;
                    }
                    if(current) settings.CurrentSpeedOverlayEnabled = number == 1;
                    if(landing) settings.LandingSpeedOverlayEnabled = number == 1;
                    return true;
                case 1 when number >= 0 && number <= int.MaxValue:
                    if(current) settings.CurrentSpeedOverlayX = (int) Math.Round(number);
                    if(landing) settings.LandingSpeedOverlayX = (int) Math.Round(number);
                    return true;
                case 2 when number >= 0 && number <= int.MaxValue:
                    if(current) settings.CurrentSpeedOverlayY = (int) Math.Round(number);
                    if(landing) settings.LandingSpeedOverlayY = (int) Math.Round(number);
                    return true;
                case 3 when number >= 0.1 && number <= 10:
                    settings.CurrentSpeedOverlayScale = settings.LandingSpeedOverlayScale = number;
                    return true;
                case 4 when target == "" && number >= 0 && number <= 100:
                    settings.LowHpTintOpacity = number;
                    return true;
                case 5 when target == "" && (number == 0 || number == 1):
                    settings.LandingSpeedOverlayEnabled = number == 1;
                    return true;
                default: return false;
            }
        }
    }
}
