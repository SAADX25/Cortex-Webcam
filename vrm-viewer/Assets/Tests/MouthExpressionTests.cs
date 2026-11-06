using System.Collections;
using System.IO;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using UniVRM10;

namespace Cortex.Vrm.Tests
{
    public sealed class MouthExpressionTests
    {
        [UnityTest]
        public IEnumerator SmileWorksWithoutTeethAndReturnsToNeutral()
        {
            var path=Path.Combine(Application.streamingAssetsPath,"MaleAvatar.vrm");
            if(!File.Exists(path))Assert.Ignore("Bundled male VRM fixture not present");
            var load=Vrm10.LoadPathAsync(path,canLoadVrm0X:true);while(!load.IsCompleted)yield return null;
            var avatar=load.GetAwaiter().GetResult();Mesh baseline=null,closed=null;
            try
            {
                avatar.UpdateType=Vrm10Instance.UpdateTypes.None;
                MouthSurface.RemoveTeeth(avatar);
                var style=new AvatarAppearanceToggle(avatar);var driver=new MouthExpressionDriver(avatar);
                Assert.IsTrue(driver.HasDetailedMouth);
                SkinnedMeshRenderer face=null;foreach(var r in avatar.GetComponentsInChildren<SkinnedMeshRenderer>())if(r.name=="Face")face=r;
                Assert.NotNull(face);var mesh=face.sharedMesh;
                foreach(bool enabled in new[]{true,false,true})
                {
                    style.SetEnabled(enabled);
                    int mouth=System.Array.FindIndex(face.sharedMaterials,m=>m!=null&&m.name.Contains("FaceMouth"));
                    Assert.GreaterOrEqual(mouth,0);var uv=face.sharedMesh.uv;var triangles=face.sharedMesh.GetTriangles(mouth);
                    Assert.Greater(triangles.Length,0,"Retain the inside mouth, not a hole through the head");
                    for(int t=0;t<triangles.Length;t+=3)
                    {
                        bool enamel=true;for(int k=0;k<3;++k){var p=uv[triangles[t+k]];enamel&=p.x>=0&&p.x<.5f&&p.y>.5f&&p.y<=1;}
                        Assert.IsFalse(enamel,"Teeth must stay removed in both appearances");
                    }
                    foreach(var m in face.sharedMaterials)Assert.AreNotEqual("Ren teeth enamel",m.name);
                }
                driver.Apply(0,0,0);avatar.Runtime.Process();baseline=new Mesh();face.BakeMesh(baseline);
                avatar.Runtime.Expression.SetWeight(ExpressionKey.BlinkLeft,.8f);driver.Apply(.04f,.8f,0);avatar.Runtime.Process();
                Assert.Greater(face.GetBlendShapeWeight(mesh.GetBlendShapeIndex("mouthSmileLeft")),60);
                Assert.AreEqual(.8f,avatar.Runtime.Expression.GetWeight(ExpressionKey.BlinkLeft),.001f);
                driver.Apply(.85f,0,0);avatar.Runtime.Process();Assert.Greater(face.GetBlendShapeWeight(mesh.GetBlendShapeIndex("jawOpen")),70);
                avatar.Runtime.Expression.SetWeight(ExpressionKey.BlinkLeft,0);driver.Apply(0,0,0);avatar.Runtime.Process();
                closed=new Mesh();face.BakeMesh(closed);var a=baseline.vertices;var b=closed.vertices;
                for(int i=0;i<a.Length;++i)Assert.Less(Vector3.Distance(a[i],b[i]),.00001f);
            }
            finally
            {
                if(baseline!=null)Object.DestroyImmediate(baseline);if(closed!=null)Object.DestroyImmediate(closed);
                Object.DestroyImmediate(avatar.gameObject);
            }
        }
        [UnityTest]
        public IEnumerator StandardVrmUsesBoundedFallbackWithoutLosingBlink()
        {
            var path=Path.Combine(Application.streamingAssetsPath,"Alicia.vrm");if(!File.Exists(path))Assert.Ignore("Alicia fixture not present");
            var load=Vrm10.LoadPathAsync(path,canLoadVrm0X:true);while(!load.IsCompleted)yield return null;
            var avatar=load.GetAwaiter().GetResult();
            try
            {
                avatar.UpdateType=Vrm10Instance.UpdateTypes.None;var driver=new MouthExpressionDriver(avatar);Assert.IsFalse(driver.HasDetailedMouth);
                avatar.Runtime.Expression.SetWeight(ExpressionKey.BlinkLeft,.7f);driver.Apply(.8f,.9f,.3f);avatar.Runtime.Process();
                var e=avatar.Runtime.Expression;Assert.LessOrEqual(e.GetWeight(ExpressionKey.Aa)+e.GetWeight(ExpressionKey.Ee),1);
                Assert.AreEqual(.7f,e.GetWeight(ExpressionKey.BlinkLeft),.001f);driver.Apply(0,0,0);avatar.Runtime.Process();Assert.AreEqual(0,e.GetWeight(ExpressionKey.Aa));
            }
            finally{Object.DestroyImmediate(avatar.gameObject);}
        }
    }
}
