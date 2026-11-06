using System;

namespace Cortex.Vrm
{
    // Uses reception time, once per new packet. Rendering cached packets must
    // never extend this hold or manufacture additional confident observations.
    public sealed class BoneTrackingState
    {
        public const double HoldSeconds = .12;
        public const double FadeSeconds = .18;
        private double lastGood = double.NegativeInfinity;
        private bool tracking;
        public void Reset(){lastGood=double.NegativeInfinity;tracking=false;}

        public bool Observe(float confidence, double sampleTime)
        {
            if (double.IsNaN(sampleTime) || double.IsInfinity(sampleTime) ||
                float.IsNaN(confidence) || float.IsInfinity(confidence)) return false;
            Weight(sampleTime);
            if (confidence < (tracking ? .35f : .55f) || confidence > 1) return false;
            lastGood = sampleTime;
            tracking = true;
            return true;
        }

        public float Weight(double now)
        {
            if (!tracking) return 0;
            double age = now - lastGood;
            if (age < 0 || double.IsNaN(age) || age >= HoldSeconds + FadeSeconds)
            {
                tracking = false;
                return 0;
            }
            if (age <= HoldSeconds) return 1;
            double t = (age - HoldSeconds) / FadeSeconds;
            return (float)(1 - t * t * (3 - 2 * t));
        }
    }
}
