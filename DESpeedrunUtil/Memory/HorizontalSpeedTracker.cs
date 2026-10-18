namespace DESpeedrunUtil.Memory {
    // Ground contact comes from playerPState_t.groundPlane, never from vertical speed.
    internal sealed class HorizontalSpeedTracker {
        private bool? _wasGrounded;
        private double? _airSince;
        private double _impactUntil = double.NegativeInfinity;
        private readonly double _minimumAir, _impactWindow;
        public float? Current { get; private set; }
        public float? LastLanding { get; private set; }

        internal HorizontalSpeedTracker(double minimumAirMilliseconds = 0, double impactWindowMilliseconds = 0) {
            _minimumAir = minimumAirMilliseconds;
            _impactWindow = impactWindowMilliseconds;
        }

        public void Update(float x, float y, bool? grounded, double milliseconds = 0) {
            if(!float.IsFinite(x) || !float.IsFinite(y)) {
                Reset();
                return;
            }
            double speed = Math.Sqrt((double) x * x + (double) y * y);
            if(speed > float.MaxValue) {
                Reset();
                return;
            }
            Current = (float) speed;
            if(!grounded.HasValue) {
                _wasGrounded = null;
                LastLanding = null;
                _airSince = null;
                _impactUntil = double.NegativeInfinity;
                return;
            }
            // Attaching while already standing is not a landing.
            if(!grounded.Value) {
                _airSince ??= milliseconds;
            } else {
                if(_wasGrounded == false && _airSince.HasValue && milliseconds - _airSince.Value >= _minimumAir) {
                    LastLanding = Current;
                    _impactUntil = milliseconds + _impactWindow;
                } else if(milliseconds <= _impactUntil && LastLanding.HasValue) {
                    // Contact and velocity can be published in adjacent physics steps. Retain
                    // the impact peak, not later walking acceleration or the decayed UI sample.
                    LastLanding = Math.Max(LastLanding.Value, Current.Value);
                }
                _airSince = null;
            }
            _wasGrounded = grounded;
        }

        public void Reset() {
            Current = LastLanding = null;
            _wasGrounded = null;
            _airSince = null;
            _impactUntil = double.NegativeInfinity;
        }
    }
}
