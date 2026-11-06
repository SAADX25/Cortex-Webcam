using UnityEngine;

namespace Cortex.Vrm
{
    // Own one neutral frame at the receiver. Calibration uses several stable
    // measurements; rendering a cached packet cannot count as another sample.
    public sealed class HeadMotionController
    {
        private Quaternion baseline=Quaternion.identity,mean=Quaternion.identity,target=Quaternion.identity,rendered=Quaternion.identity;
        private double windowStart,lastSample=double.NegativeInfinity;
        private double notBefore=double.NegativeInfinity;
        private double inputDelay;
        private bool waitForInput;
        private int samples;
        public bool IsCentered {get;private set;}
        public static bool HasMeasuredFace(MotionPacket packet)=>packet.HasExpressions&&packet.Values[1*5+4]>=.55f;
        public void Recenter(double notBeforeSampleTime=double.NegativeInfinity)
        {IsCentered=false;samples=0;lastSample=double.NegativeInfinity;target=Quaternion.identity;notBefore=notBeforeSampleTime;waitForInput=false;}
        public void RecenterAfterInput(double delaySeconds=1.5)
        {Recenter();inputDelay=System.Math.Max(0,delaySeconds);waitForInput=true;}
        public void Observe(Quaternion localHead,Quaternion senderChest,float confidence,double sampleTime)
        {
            if(confidence<.55f || double.IsNaN(sampleTime) || double.IsInfinity(sampleTime) || sampleTime<=lastSample)return;
            if(waitForInput){notBefore=sampleTime+inputDelay;waitForInput=false;}
            lastSample=sampleTime;
            if(sampleTime<notBefore)return;
            Quaternion world=Quaternion.Normalize(senderChest*localHead);
            if(!IsCentered)
            {
                if(samples==0 || Quaternion.Angle(mean,world)>6)
                {samples=1;mean=world;windowStart=sampleTime;return;}
                ++samples;mean=Quaternion.Slerp(mean,world,1f/samples);
                if(samples<6 || sampleTime-windowStart<.30)return;
                baseline=mean;IsCentered=true;
            }
            var e=(Quaternion.Inverse(baseline)*world).eulerAngles;
            // Negative Unity X looks up; keep pitch anatomically bounded and
            // prevent a noisy inference from throwing the face at the ceiling.
            target=Quaternion.Euler(Mathf.Clamp(Mathf.DeltaAngle(0,e.x),-28,38),
                Mathf.Clamp(Mathf.DeltaAngle(0,e.y),-60,60),Mathf.Clamp(Mathf.DeltaAngle(0,e.z),-28,28));
        }
        public Quaternion Render(float dt,float confidenceWeight)
        {
            dt=Mathf.Clamp(dt,0,.1f);
            Quaternion desired=IsCentered?Quaternion.Slerp(Quaternion.identity,target,Mathf.Clamp01(confidenceWeight)):Quaternion.identity;
            float angle=Quaternion.Angle(rendered,desired);
            if(angle<.22f)return rendered;
            float tau=Mathf.Lerp(.095f,.040f,Mathf.Clamp01(angle/25));
            var eased=Quaternion.Slerp(rendered,desired,1-Mathf.Exp(-dt/tau));
            rendered=Quaternion.RotateTowards(rendered,eased,220*dt);return rendered;
        }
        public static void ApplyToRig(Transform root,Transform head,Quaternion headRestInRoot,
            Transform neck,Quaternion neckRest,Quaternion headRestInNeck,Quaternion relative)
        {
            if(head==null)return;
            Quaternion desired=root.rotation*relative*headRestInRoot;
            if(neck!=null)
            {
                Quaternion restWorld=(neck.parent!=null?neck.parent.rotation:root.rotation)*neckRest;
                neck.rotation=Quaternion.Slerp(restWorld,desired*Quaternion.Inverse(headRestInNeck),.35f);
            }
            // The sender has already removed chest rotation. Reconstruct world
            // head orientation once, then counter the actual displayed torso.
            // Applying a chest-relative delta as a head local delta adds tilt.
            head.rotation=desired;
        }
    }
}
