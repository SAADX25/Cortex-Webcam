using NUnit.Framework;
using UnityEngine;
using System.Reflection;

namespace Cortex.Vrm.Tests
{
    public sealed class ArmTrackingTests
    {
        [TestCase(30)] [TestCase(60)] [TestCase(120)]
        public void OcclusionHoldsThenFadesAndRejectsFlickeringReacquisition(int fps)
        {
            var gate=new ArmTrackingGate();bool accepted=false;
            for(int i=0;i<=fps;++i)accepted|=gate.Observe(true,i/(double)fps);
            Assert.IsTrue(accepted);Assert.AreEqual(1,gate.Weight(1));
            gate.Observe(false,1+1.0/fps);Assert.AreEqual(1,gate.Weight(1.20));
            float previous=1;
            for(int frame=1;frame<=fps;++frame)
            {
                double now=1+frame/(double)fps;
                Assert.IsFalse(gate.Observe(frame%2==0,now),"Intermittent detector flashes must not refresh the old hand");
                float weight=gate.Weight(now);Assert.LessOrEqual(weight,previous+.00001f);previous=weight;
            }
            Assert.AreEqual(0,previous);
            bool reacquired=false;for(int i=1;i<=fps;++i)reacquired|=gate.Observe(true,2+i/(double)fps);
            Assert.IsTrue(reacquired);Assert.AreEqual(1,gate.Weight(3));
        }
        [Test]
        public void CachedPacketCannotAcquireOrExtendHold()
        {
            var gate=new ArmTrackingGate();for(int i=0;i<100;++i)Assert.IsFalse(gate.Observe(true,0));
            Assert.AreEqual(0,gate.Weight(0));
            gate.Observe(true,.04);Assert.IsTrue(gate.Observe(true,.10));
            for(int i=0;i<100;++i)Assert.IsFalse(gate.Observe(true,.10));
            Assert.AreEqual(0,gate.Weight(1));
        }
        [Test]
        public void ArmsWithoutMeasuredPalmsAreNotReliable()
        {
            var packet=new MotionPacket();for(int i=2;i<8;++i)packet.Values[i*5+4]=.9f;
            Assert.IsFalse(ArmTrackingGate.Reliable(packet,0));Assert.IsFalse(ArmTrackingGate.Reliable(packet,1));
            for(int i=8;i<23;++i)packet.Values[i*5+4]=.9f;
            Assert.IsTrue(ArmTrackingGate.Reliable(packet,0));Assert.IsFalse(ArmTrackingGate.Reliable(packet,1));
            packet.Values[4*5+4]=.3f;Assert.IsFalse(ArmTrackingGate.Reliable(packet,0),"Sparse Pose guesses must not count as a measured hand");
        }
        [Test]
        public void DisableAndEnableClearArmsWithoutResettingHead()
        {
            var go=new GameObject("Arm controls test");
            try
            {
                var viewer=go.AddComponent<CortexVrmViewer>();
                var flags=BindingFlags.Instance|BindingFlags.NonPublic;
                var gates=(ArmTrackingGate[])typeof(CortexVrmViewer).GetField("armTracking",flags).GetValue(viewer);
                var head=(HeadMotionController)typeof(CortexVrmViewer).GetField("headMotion",flags).GetValue(viewer);
                for(int i=0;i<30;++i){head.Observe(Quaternion.identity,Quaternion.identity,1,i/30.0);foreach(var gate in gates)gate.Observe(true,i/30.0);}
                Assert.IsTrue(head.IsCentered);
                typeof(CortexVrmViewer).GetField("armPoseMode",flags).SetValue(viewer,ArmPoseMode.CoverEyes);
                viewer.SetHandsEnabled(false);Assert.IsFalse(viewer.HandsEnabled);
                foreach(var gate in gates)Assert.AreEqual(0,gate.Weight(1));
                Assert.AreEqual(ArmPoseMode.Camera,typeof(CortexVrmViewer).GetField("armPoseMode",flags).GetValue(viewer));
                Assert.IsTrue(head.IsCentered,"The hands button must not reset head or facial tracking");
                viewer.SetHandsEnabled(true);Assert.IsTrue(viewer.HandsEnabled);
                foreach(var gate in gates){Assert.AreEqual(0,gate.Weight(2));Assert.IsFalse(gate.Observe(true,2),"Resume must wait for fresh, stable measurements");}
            }
            finally{Object.DestroyImmediate(go);}
        }
    }
}
