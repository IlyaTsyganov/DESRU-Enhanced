namespace DESpeedrunUtil.Overlay {
    internal sealed class NativeGameNotReadyException : InvalidOperationException {
        internal NativeGameNotReadyException(string message) : base(message) { }
    }

    internal sealed class NativeAttachRetry {
        private long _nextAttempt;
        private int _failures;
        internal bool Unsupported { get; private set; }
        internal bool CanAttempt(long now) => !Unsupported && now >= _nextAttempt;
        internal void RetryLater(long now) {
            _nextAttempt = now + Math.Min(2000, 250 << Math.Min(_failures, 3));
            _failures = Math.Min(_failures + 1, 3);
        }
        internal void RejectUnsupported() => Unsupported = true;
        internal void Reset() { _nextAttempt = 0; _failures = 0; Unsupported = false; }
    }
}
