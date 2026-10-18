using System.Diagnostics;
using System.Runtime.InteropServices;
using Serilog;

namespace DESpeedrunUtil.Memory {
    internal sealed record HorizontalSpeedSnapshot(float? Current, float? LastLanding) {
        internal static readonly HorizontalSpeedSnapshot Empty = new(null, null);
    }

    // The WinForms timer can miss a landing impulse that lasts only one 4 ms physics step.
    // This worker only reads game memory; the existing trainer and memory writes stay on the UI thread.
    internal sealed class HorizontalSpeedSampler : IDisposable {
        private sealed record Context(IntPtr Address, int GroundOffset, bool Active);
        private readonly Process _game;
        private readonly CancellationTokenSource _stop = new();
        private readonly Thread _thread;
        private Context _context = new(IntPtr.Zero, 0, false);
        private HorizontalSpeedSnapshot _snapshot = HorizontalSpeedSnapshot.Empty;
        internal HorizontalSpeedSnapshot Snapshot => Volatile.Read(ref _snapshot);

        internal HorizontalSpeedSampler(Process game) {
            _game = game;
            _thread = new Thread(Sample) { IsBackground = true, Name = "DESRU horizontal speed reader" };
            _thread.Start();
        }

        internal void SetContext(IntPtr address, int groundOffset, bool active) =>
            Volatile.Write(ref _context, new Context(address, groundOffset, active && address != IntPtr.Zero));

        private void Sample() {
            var clock = Stopwatch.StartNew();
            var tracker = new HorizontalSpeedTracker(8, 8);
            IntPtr previous = IntPtr.Zero;
            bool timer = false;
            try {
                while(!_stop.IsCancellationRequested) {
                    var context = Volatile.Read(ref _context);
                    if(!context.Active) {
                        tracker.Reset(); previous = IntPtr.Zero;
                        Volatile.Write(ref _snapshot, HorizontalSpeedSnapshot.Empty);
                        if(timer) { timeEndPeriod(1); timer = false; }
                        _stop.Token.WaitHandle.WaitOne(20);
                        continue;
                    }
                    if(!timer) timer = timeBeginPeriod(1) == 0;
                    if(context.Address != previous) tracker.Reset();
                    previous = context.Address;
                    int length = context.GroundOffset > 0 ? context.GroundOffset + 1 : 12;
                    if(_game.ReadBytes(context.Address, length, out var sample) &&
                        float.IsFinite(BitConverter.ToSingle(sample, 8)) &&
                        (context.GroundOffset <= 0 || sample[context.GroundOffset] <= 1)) {
                        tracker.Update(BitConverter.ToSingle(sample, 0), BitConverter.ToSingle(sample, 4),
                            context.GroundOffset > 0 ? sample[context.GroundOffset] != 0 : null, clock.Elapsed.TotalMilliseconds);
                    } else tracker.Reset();
                    var next = new HorizontalSpeedSnapshot(tracker.Current, tracker.LastLanding);
                    if(next != Snapshot) Volatile.Write(ref _snapshot, next);
                    Thread.Sleep(1);
                }
            } catch(Exception e) {
                Log.Warning(e, "Horizontal speed sampling stopped");
            } finally {
                if(timer) timeEndPeriod(1);
                Volatile.Write(ref _snapshot, HorizontalSpeedSnapshot.Empty);
            }
        }

        public void Dispose() {
            _stop.Cancel();
            _thread.Join(); // Finish reads before the owner closes the game process handle.
            _stop.Dispose();
        }

        [DllImport("winmm.dll")] private static extern uint timeBeginPeriod(uint period);
        [DllImport("winmm.dll")] private static extern uint timeEndPeriod(uint period);
    }
}
