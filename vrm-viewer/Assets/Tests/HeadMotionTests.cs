using NUnit.Framework;
using UnityEngine;

namespace Cortex.Vrm.Tests
{
    public sealed class HeadMotionTests
    {
        private static void Center(HeadMotionController motion,Quaternion pose,double start=0)
        {for(int i=0;i<30;++i)motion.Observe(pose,Quaternion.identity,1,start+i/30.0);Assert.IsTrue(motion.IsCentered);}
        private static Quaternion Settle(HeadMotionController motion,float weight=1)
        {Quaternion q=Quaternion.identity;for(int i=0;i<120;++i)q=motion.Render(1f/60,weight);return q;}
        [Test]
        public void PoseFallbackOrUnfittedFaceCannotBecomeTheNeutralHead()
        {
            var packet=new MotionPacket();packet.Values[9]=1;
            Assert.IsFalse(HeadMotionController.HasMeasuredFace(packet),"Body-Pose head alone must not establish face neutral");
            packet.HasExpressions=true;packet.Values[9]=0;
            Assert.IsFalse(HeadMotionController.HasMeasuredFace(packet),"Visible lips/eyes without a head fit are not a zero-angle head");
            packet.Values[9]=.8f;Assert.IsTrue(HeadMotionController.HasMeasuredFace(packet));
        }
        [Test]
        public void StartupDelayBeginsWithFaceInputEvenIfPhoneConnectsLate()
        {
            var motion=new HeadMotionController();motion.RecenterAfterInput(1.5);
            // Loading/phone connection can take much longer than the viewer
            // startup timer. Those seconds must not consume the settle delay.
            for(int i=0;i<45;++i)motion.Observe(Quaternion.Euler(30,0,0),Quaternion.identity,1,100+i/30.0);
            Assert.IsFalse(motion.IsCentered);
            Center(motion,Quaternion.identity,101.6);
            Assert.Less(Quaternion.Angle(Settle(motion),Quaternion.identity),.25f);
        }
        [Test]
        public void CenterWaitsUntilUserCanFaceTheCamera()
        {
            var motion=new HeadMotionController();motion.Recenter(2);
            for(int i=0;i<60;++i)motion.Observe(Quaternion.Euler(30,0,0),Quaternion.identity,1,i/30.0);
            Assert.IsFalse(motion.IsCentered,"Do not calibrate while the user is still looking down to launch/click");
            Center(motion,Quaternion.identity,2.1);
            Assert.Less(Quaternion.Angle(Settle(motion),Quaternion.identity),.25f);
        }
        [TestCase(.5f,0)] [TestCase(1f,0)] [TestCase(2f,135)]
        public void CameraViewpointIsAtEyeLevelInsteadOfBelowTheChin(float scale,float yaw)
        {
            var root=new GameObject("Camera frame root");var cameraObject=new GameObject("Camera frame test");
            try
            {
                root.transform.position=new Vector3(1,2,-3);root.transform.rotation=Quaternion.Euler(0,yaw,0);
                Vector3 eyes=root.transform.position+root.transform.up*1.615f*scale;
                Vector3 hips=root.transform.position+root.transform.up*1.012f*scale;
                var camera=cameraObject.AddComponent<Camera>();
                AvatarCameraFraming.Place(camera,root.transform,eyes,hips,scale);
                Vector3 ray=camera.transform.position-eyes;
                Assert.That(Vector3.Dot(ray,root.transform.up),Is.EqualTo(0).Within(.0001f));
                Assert.That(Vector3.Dot(ray,root.transform.forward),Is.EqualTo(1.45f*scale).Within(.0001f));
                Assert.Less(Quaternion.Angle(camera.transform.rotation,
                    Quaternion.LookRotation(Vector3.Lerp(hips,eyes,.44f)-camera.transform.position,root.transform.up)),.02f);
            }
            finally{Object.DestroyImmediate(cameraObject);Object.DestroyImmediate(root);}
        }
        [Test]
        public void CenterRejectsFirstOutlierAndDuplicateSamples()
        {
            var motion=new HeadMotionController();motion.Observe(Quaternion.Euler(-40,0,0),Quaternion.identity,1,0);
            for(int i=0;i<100;++i)motion.Observe(Quaternion.Euler(15,0,0),Quaternion.identity,1,.1);
            Assert.IsFalse(motion.IsCentered,"One packet rendered repeatedly must not calibrate a head");
            Center(motion,Quaternion.Euler(15,0,0),.2);
            Assert.Less(Quaternion.Angle(Settle(motion),Quaternion.identity),.25f,"A stable neutral must remove the camera's pitch bias");
        }
        [TestCase(-25f,1f)]
        [TestCase(25f,-1f)]
        public void PitchDirectionAndLimitsArePreserved(float pitch,float sign)
        {
            var motion=new HeadMotionController();Center(motion,Quaternion.identity);
            motion.Observe(Quaternion.Euler(pitch,0,0),Quaternion.identity,1,2);
            Assert.Greater((Settle(motion)*Vector3.forward).y*sign,.3f);
            motion.Observe(Quaternion.Euler(pitch<0?-80:80,0,0),Quaternion.identity,1,3);
            float angle=Mathf.DeltaAngle(0,Settle(motion).eulerAngles.x);
            Assert.That(angle,Is.InRange(-28.25f,38.25f));
        }
        [Test]
        public void HeadDoesNotInheritAnExtraTorsoTilt()
        {
            var root=new GameObject("Head test root");
            try
            {
                var chest=new GameObject("Chest").transform;chest.SetParent(root.transform,false);
                var neck=new GameObject("Neck").transform;neck.SetParent(chest,false);neck.localPosition=Vector3.up*.2f;
                var head=new GameObject("Head").transform;head.SetParent(neck,false);head.localPosition=Vector3.up*.1f;
                root.transform.rotation=Quaternion.Euler(0,35,0);
                var restRoot=Quaternion.Inverse(root.transform.rotation)*head.rotation;
                var headInNeck=Quaternion.Inverse(neck.rotation)*head.rotation;
                var neckPosition=neck.localPosition;var headPosition=head.localPosition;
                var motion=new HeadMotionController();Center(motion,Quaternion.identity);
                var senderChest=Quaternion.Euler(25,0,0);var desired=Quaternion.Euler(-15,20,0);
                motion.Observe(Quaternion.Inverse(senderChest)*desired,senderChest,1,2);
                Quaternion relative=Settle(motion);chest.localRotation=Quaternion.Euler(40,0,0);
                HeadMotionController.ApplyToRig(root.transform,head,restRoot,neck,Quaternion.identity,headInNeck,relative);
                Assert.Less(Quaternion.Angle(head.rotation,root.transform.rotation*desired*restRoot),.3f);
                Assert.AreEqual(neckPosition,neck.localPosition);Assert.AreEqual(headPosition,head.localPosition);
            }
            finally{Object.DestroyImmediate(root);}
        }
        [Test]
        public void RecenterAndLostTrackingReturnSmoothlyToNeutral()
        {
            var motion=new HeadMotionController();Center(motion,Quaternion.identity);
            motion.Observe(Quaternion.Euler(-25,20,0),Quaternion.identity,1,2);Settle(motion);
            var before=motion.Render(1f/60,1);motion.Recenter();var after=motion.Render(1f/60,1);
            Assert.Less(Quaternion.Angle(before,after),4,"Center must not snap the neck on one frame");
            Center(motion,Quaternion.Euler(-25,20,0),3);Assert.Less(Quaternion.Angle(Settle(motion),Quaternion.identity),.25f);
            motion.Observe(Quaternion.Euler(0,45,0),Quaternion.identity,1,5);Settle(motion);
            Assert.Less(Quaternion.Angle(Settle(motion,0),Quaternion.identity),.25f);
        }
    }
}
