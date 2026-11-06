using NUnit.Framework;
using UnityEngine;

namespace Cortex.Vrm.Tests
{
    public sealed class ArmPosePresetsTests
    {
        private static Transform Bone(Transform parent,string name,Vector3 position)
        {
            var t=new GameObject(name).transform;
            t.SetParent(parent,false);t.localPosition=position;return t;
        }

        [TestCase(.5f,0f,ArmPoseMode.CrossArms)]
        [TestCase(1f,0f,ArmPoseMode.CrossArms)]
        [TestCase(2f,180f,ArmPoseMode.CrossArms)]
        [TestCase(.5f,0f,ArmPoseMode.CoverEyes)]
        [TestCase(1f,0f,ArmPoseMode.CoverEyes)]
        [TestCase(2f,180f,ArmPoseMode.CoverEyes)]
        public void PresetsReachTargetsAndPreserveJointLengths(float scale,float yaw,ArmPoseMode mode)
        {
            var root=new GameObject("Test rig");
            try
            {
                var bones=new Transform[38];var rest=new Quaternion[38];
                for(int i=0;i<rest.Length;++i)rest[i]=Quaternion.identity;
                bones[0]=Bone(root.transform,"Chest",new Vector3(0,1.3f,0));
                bones[1]=Bone(bones[0],"Head",new Vector3(0,.25f,0));
                var leftEye=Bone(bones[1],"LeftEye",new Vector3(.035f,.07f,.04f));
                var rightEye=Bone(bones[1],"RightEye",new Vector3(-.035f,.07f,.04f));
                for(int side=0;side<2;++side)
                {
                    float sign=side==0?1:-1;int b=side==0?2:5;
                    bones[b]=Bone(bones[0],"UpperArm",new Vector3(sign*.2f,.12f,0));
                    bones[b+1]=Bone(bones[b],"LowerArm",new Vector3(sign*.28f,0,0));
                    bones[b+2]=Bone(bones[b+1],"Hand",new Vector3(sign*.24f,0,0));
                    for(int finger=0;finger<5;++finger)
                    {
                        int i=8+side*15+finger*3;
                        bones[i]=Bone(bones[b+2],"Finger",new Vector3(sign*.07f,0,(2-finger)*.02f));
                        bones[i+1]=Bone(bones[i],"Middle",new Vector3(sign*.03f,0,0));
                        bones[i+2]=Bone(bones[i+1],"Tip",new Vector3(sign*.02f,0,0));
                    }
                }
                root.transform.localScale=Vector3.one*scale;
                root.transform.rotation=Quaternion.Euler(0,yaw,0);
                var preset=new ArmPosePresets(bones,rest,leftEye,rightEye);
                Assert.IsTrue(preset.Available);
                Vector3 camera=root.transform.TransformPoint(new Vector3(0,1.3f,3));
                for(int frame=0;frame<400;++frame)
                {
                    preset.Apply(mode,camera,1f/60);
                    for(int side=0;side<2;++side)
                    {
                        int b=side==0?2:5;
                        Assert.That(Vector3.Distance(bones[b].position,bones[b+1].position),Is.EqualTo(.28f*scale).Within(.0001f*scale));
                        Assert.That(Vector3.Distance(bones[b+1].position,bones[b+2].position),Is.EqualTo(.24f*scale).Within(.0001f*scale));
                    }
                }
                if(mode==ArmPoseMode.CrossArms)
                {
                    Assert.Less(root.transform.InverseTransformPoint(bones[4].position).x,0,"Left hand must cross the chest");
                    Assert.Greater(root.transform.InverseTransformPoint(bones[7].position).x,0,"Right hand must cross the chest");
                }
                else
                {
                    Assert.Less(Vector3.Distance(bones[4].position,leftEye.position),.15f*scale);
                    Assert.Less(Vector3.Distance(bones[7].position,rightEye.position),.15f*scale);
                    Assert.Greater(Vector3.Dot(bones[4].rotation*Vector3.right,root.transform.up),.95f,"Left fingers must point up across eyes");
                    Assert.Greater(Vector3.Dot(bones[7].rotation*Vector3.left,root.transform.up),.95f,"Right fingers must point up across eyes");
                }
                Vector3 wrist=bones[4].position;
                preset.Apply(ArmPoseMode.Camera,camera,1f/60);
                Assert.Less(Vector3.Distance(wrist,bones[4].position),.0001f,"Camera mode must release preset control");
            }
            finally { Object.DestroyImmediate(root); }
        }
    }
}
