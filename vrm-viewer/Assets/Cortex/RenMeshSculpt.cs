using UnityEngine;

namespace Cortex.Vrm
{
    // Deform the existing textured surfaces rather than covering the face or
    // fringe with polygons. Apply exactly the same continuous mapping to every
    // morph target so blinks, vowels and smiles keep their original indices.
    public static class RenMeshSculpt
    {
        public static Mesh Create(SkinnedMeshRenderer renderer,Transform root,Vector3 head,float unit,bool face)
        {
            Mesh source=renderer.sharedMesh, result=Object.Instantiate(source);
            Vector3[] original=source.vertices, sculpted=new Vector3[original.Length];
            Vector3 Warp(Vector3 vertex)
            {
                Vector3 p=root.InverseTransformPoint(renderer.transform.TransformPoint(vertex));
                Vector3 d=(p-head)/unit;
                if(face)
                {
                    // A narrower jaw and slightly longer lower face replace
                    // the base's very round, youthful silhouette.
                    float jaw=1-Mathf.SmoothStep(0,1,Mathf.InverseLerp(-.045f,.060f,d.y));
                    d.x*=1-.105f*jaw;
                    d.y-=.010f*jaw;
                    // Reduce eye height, including eyelids and brows, with a
                    // smooth falloff into the cheeks and surrounding skin.
                    float eye=Mathf.Exp(-Mathf.Pow((d.y-.064f)/.036f,4))*Mathf.SmoothStep(0,1,Mathf.InverseLerp(.008f,.027f,Mathf.Abs(d.x)));
                    float front=Mathf.SmoothStep(0,1,Mathf.InverseLerp(.005f,.040f,d.z));
                    d.y-= (d.y-.064f)*.17f*eye*front;
                    d.x*=1+.025f*eye*front;
                }
                else
                {
                    float crown=Mathf.SmoothStep(0,1,Mathf.InverseLerp(.080f,.22f,d.y));
                    float azimuth=Mathf.Atan2(d.x,d.z);
                    d.x*=1.06f+.012f*Mathf.Sin(azimuth*7);
                    d.y+=crown*(.008f+.002f*Mathf.Sin(azimuth*5+.5f));
                    float fringe=Mathf.SmoothStep(0,1,Mathf.InverseLerp(.015f,.085f,d.z));
                    d.x-=fringe*.014f*Mathf.Clamp01((d.y+.005f)/.16f);
                    d.y+=fringe*.005f*Mathf.Sin(d.x*80);
                }
                return renderer.transform.InverseTransformPoint(root.TransformPoint(head+d*unit));
            }
            for(int i=0;i<original.Length;++i)sculpted[i]=Warp(original[i]);
            result.ClearBlendShapes();result.vertices=sculpted;
            var delta=new Vector3[original.Length];var normal=new Vector3[original.Length];var tangent=new Vector3[original.Length];
            for(int shape=0;shape<source.blendShapeCount;++shape)
                for(int frame=0;frame<source.GetBlendShapeFrameCount(shape);++frame)
                {
                    source.GetBlendShapeFrameVertices(shape,frame,delta,normal,tangent);
                    for(int i=0;i<delta.Length;++i)delta[i]=Warp(original[i]+delta[i])-sculpted[i];
                    result.AddBlendShapeFrame(source.GetBlendShapeName(shape),source.GetBlendShapeFrameWeight(shape,frame),delta,normal,tangent);
                }
            result.name=source.name+" Ren sculpt";result.RecalculateBounds();return result;
        }
    }
}
