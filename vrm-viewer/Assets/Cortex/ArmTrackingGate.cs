using System;

namespace Cortex.Vrm
{
    // One state per anatomical arm. Cached render frames never count as fresh
    // observations. Losing the palm stops all chain updates together.
    public sealed class ArmTrackingGate
    {
        public const double HoldSeconds=.22, FadeSeconds=.38;
        private double lastSample=double.NegativeInfinity,firstCandidate,lastGood=double.NegativeInfinity;
        private int candidates;
        private bool acquired;
        public double LastAcceptedTime=>lastGood;
        public void Reset(){lastSample=lastGood=double.NegativeInfinity;candidates=0;acquired=false;}
        public bool Observe(bool reliable,double time)
        {
            if(double.IsNaN(time)||double.IsInfinity(time)||time<=lastSample)return false;
            if(time-lastSample>.5){candidates=0;acquired=false;}
            lastSample=time;
            if(!reliable){candidates=0;acquired=false;return false;}
            if(!acquired)
            {
                if(candidates++==0)firstCandidate=time;
                if(candidates<3 || time-firstCandidate<.08)return false;
                acquired=true;
            }
            lastGood=time;return true;
        }
        public float Weight(double time)
        {
            double age=time-lastGood;
            if(age<0||double.IsNaN(age)||age>=HoldSeconds+FadeSeconds)return 0;
            if(age<=HoldSeconds)return 1;
            double t=(age-HoldSeconds)/FadeSeconds;return (float)(1-t*t*(3-2*t));
        }
        public static bool Reliable(MotionPacket packet,int side)
        {
            int arm=side==0?2:5,fingers=8+side*15;
            if(packet.Values[arm*5+4]<.6f || packet.Values[(arm+1)*5+4]<.6f || packet.Values[(arm+2)*5+4]<.7f)return false;
            int measured=0;for(int i=fingers;i<fingers+15;++i)if(packet.Values[i*5+4]>=.7f)++measured;
            return measured>=6;
        }
    }
}
