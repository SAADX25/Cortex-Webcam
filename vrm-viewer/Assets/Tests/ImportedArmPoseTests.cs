using System.Collections;
using System.IO;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using UniVRM10;

namespace Cortex.Vrm.Tests
{
    public sealed class ImportedArmPoseTests
    {
        [UnityTest]
        public IEnumerator LocalVrmCanCrossArmsAndCoverEyes()
        {
            string path=Path.Combine(Application.streamingAssetsPath,"MaleAvatar.vrm");
            if(!File.Exists(path))path=Path.Combine(Application.streamingAssetsPath,"Alicia.vrm");
            if(!File.Exists(path))Assert.Ignore("No local VRM test model");
            var loading=Vrm10.LoadPathAsync(path,canLoadVrm0X:true);
            while(!loading.IsCompleted)yield return null;
            var instance=loading.GetAwaiter().GetResult();
            try
            {
                instance.UpdateType=Vrm10Instance.UpdateTypes.None;
                var bones=new Transform[38];var rest=new Quaternion[38];var positions=new Vector3[38];
                var map=new[]{HumanBodyBones.Chest,HumanBodyBones.Head,HumanBodyBones.LeftUpperArm,
                    HumanBodyBones.LeftLowerArm,HumanBodyBones.LeftHand,HumanBodyBones.RightUpperArm,
                    HumanBodyBones.RightLowerArm,HumanBodyBones.RightHand};
                for(int i=0;i<8;++i)bones[i]=instance.Runtime.ControlRig.GetBoneTransform(map[i]);
                if(bones[0]==null)bones[0]=instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.Spine);
                string[] fingers={"Thumb","Index","Middle","Ring","Little"};
                string[] joints={"Proximal","Intermediate","Distal"};
                for(int side=0;side<2;++side)for(int f=0;f<5;++f)for(int j=0;j<3;++j)
                {
                    var id=(HumanBodyBones)System.Enum.Parse(typeof(HumanBodyBones),(side==0?"Left":"Right")+fingers[f]+joints[j]);
                    bones[8+side*15+f*3+j]=instance.Runtime.ControlRig.GetBoneTransform(id);
                }
                for(int i=0;i<bones.Length;++i)if(bones[i]!=null){rest[i]=bones[i].localRotation;positions[i]=bones[i].localPosition;}
                var leftEye=instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.LeftEye);
                var rightEye=instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.RightEye);
                float depth=AvatarFaceGeometry.EyeSurfaceDepth(instance,bones[1],leftEye,rightEye);
                var originalRenderers=instance.GetComponentsInChildren<SkinnedMeshRenderer>();
                var originalMeshes=System.Array.ConvertAll(originalRenderers,r=>r.sharedMesh);
                var originalMaterials=System.Array.ConvertAll(originalRenderers,r=>r.sharedMaterials);
                var appearance=new AvatarAppearanceToggle(instance);
                var outfit=instance.transform.Find("Ren inspired outfit").GetComponent<SkinnedMeshRenderer>();
                Assert.Greater(outfit.sharedMesh.vertexCount,1000,"Outfit must be 3D geometry, not a recolored shirt");
                Assert.AreEqual(outfit.bones.Length,outfit.sharedMesh.bindposes.Length);
                foreach(var weight in outfit.sharedMesh.boneWeights)
                {
                    Assert.That(weight.weight0+weight.weight1+weight.weight2+weight.weight3,Is.EqualTo(1).Within(.0001f));
                    Assert.Less(weight.boneIndex0,outfit.bones.Length);
                    Assert.Less(weight.boneIndex1,outfit.bones.Length);
                }
                for(int i=0;i<originalRenderers.Length;++i)
                {
                    var refined=originalRenderers[i].sharedMesh;
                    Assert.AreEqual(originalMeshes[i].blendShapeCount,refined.blendShapeCount,"Keep face and hand expression indices");
                    for(int shape=0;shape<refined.blendShapeCount;++shape)
                    {
                        Assert.AreEqual(originalMeshes[i].GetBlendShapeName(shape),refined.GetBlendShapeName(shape));
                        Assert.AreEqual(originalMeshes[i].GetBlendShapeFrameCount(shape),refined.GetBlendShapeFrameCount(shape));
                    }
                }
                var before=new Mesh();var after=new Mesh();
                try
                {
                    outfit.BakeMesh(before);
                    var knee=instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.LeftLowerLeg);
                    Quaternion kneeRest=knee.localRotation;
                    knee.localRotation=kneeRest*Quaternion.Euler(20,0,0);instance.Runtime.Process();outfit.BakeMesh(after);
                    float moved=0;var a=before.vertices;var b=after.vertices;
                    for(int v=0;v<a.Length;++v)moved=Mathf.Max(moved,Vector3.Distance(a[v],b[v]));
                    Assert.Greater(moved,.025f,"Full-length trousers must actually deform with the knee");
                    knee.localRotation=kneeRest;instance.Runtime.Process();
                }
                finally{Object.DestroyImmediate(before);Object.DestroyImmediate(after);}
                appearance.SetEnabled(false);
                for(int i=0;i<originalRenderers.Length;++i)
                {
                    Assert.AreSame(originalMeshes[i],originalRenderers[i].sharedMesh,"Original model mesh must be restored exactly");
                    CollectionAssert.AreEqual(originalMaterials[i],originalRenderers[i].sharedMaterials);
                }
                appearance.SetEnabled(true);
                var presets=new ArmPosePresets(bones,rest,leftEye,rightEye,depth);
                Assert.IsTrue(presets.Available,"Imported VRM needs a complete arm rig");
                Vector3 up=instance.transform.up;
                Vector3 left=(bones[2].position-bones[5].position).normalized;
                Vector3 camera=bones[1].position+instance.transform.forward*3;
                foreach(var mode in new[]{ArmPoseMode.CrossArms,ArmPoseMode.CoverEyes})
                {
                    for(int frame=0;frame<300;++frame){presets.Apply(mode,camera,1f/60);instance.Runtime.Process();}
                    if(mode==ArmPoseMode.CrossArms)
                    {
                        Vector3 center=(bones[2].position+bones[5].position)*.5f;
                        Assert.Less(Vector3.Dot(bones[4].position-center,left),0,"Left wrist must cross the actual model's torso");
                        Assert.Greater(Vector3.Dot(bones[7].position-center,left),0,"Right wrist must cross the actual model's torso");
                    }
                    else
                    {
                        float arm=Vector3.Distance(bones[2].position,bones[3].position)+Vector3.Distance(bones[3].position,bones[4].position);
                        Assert.Less(Vector3.Distance(bones[4].position,bones[1].position),arm*.6f);
                        Assert.Less(Vector3.Distance(bones[7].position,bones[1].position),arm*.6f);
                        Assert.Greater(Vector3.Dot(bones[4].position-bones[2].position,up),0,"Raised hands must reach the head rather than the chest");
                        Vector3 eyeCenter=(leftEye.position+rightEye.position)*.5f;
                        foreach(int hand in new[]{4,7})
                            Assert.Greater(Vector3.Dot(bones[hand].position-eyeCenter,instance.transform.forward),depth+.012f,"Wrist plane must clear the actual face surface, not the buried eye pivot");
                    }
                    for(int i=0;i<bones.Length;++i)if(bones[i]!=null)
                        Assert.Less(Vector3.Distance(bones[i].localPosition,positions[i]),.0001f,"Pose presets must not detach joints on the imported VRM");
                }
            }
            finally { Object.DestroyImmediate(instance.gameObject); }
        }
    }
}
