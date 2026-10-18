using System.Globalization;

namespace DESpeedrunUtil.Overlay {
    internal static class SpeedometerText {
        internal static string Format(SpeedometerReading reading) {
            if(!reading.Supported) return "N/A";
            if(!reading.Speed.HasValue) return "--";
            string text = reading.Speed.Value.ToString("0.00", CultureInfo.InvariantCulture);
            // Keep even exceptional finite values inside the bounded native text buffer.
            return text.Length <= 12 ? text : reading.Speed.Value.ToString("0.00E+0", CultureInfo.InvariantCulture);
        }
    }
}
