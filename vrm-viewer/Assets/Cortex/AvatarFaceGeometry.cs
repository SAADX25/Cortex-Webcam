using UnityEngine;
using UniVRM10;

namespace Cortex.Vrm
{
    public static class AvatarFaceGeometry
    {
        // Sample the imported face geometry once at load. Eye transforms are
        // rotation pivots, not facial skin positions, so cannot set clearance.
        public static float EyeSurfaceDepth(Vrm10Instance avatar,Transform head,Transform leftEye,Transform rightEye)
        {
            if(head==null || leftEye==null || rightEye==null)return 0;
            var origin=(leftEye.position+rightEye.position)*.5f;
            var front=avatar.transform.forward;var up=avatar.transform.up;var right=avatar.transform.right;
            float scale=Mathf.Max(.1f,Vector3.Distance(head.position,origin)*2);
            float depth=0;
            foreach(var renderer in avatar.GetComponentsInChildren<SkinnedMeshRenderer>())
            {
                if(!renderer.name.ToLowerInvariant().Contains("face"))continue;
                var mesh=new Mesh();renderer.BakeMesh(mesh);
                foreach(var vertex in mesh.vertices)
                {
                    Vector3 d=renderer.transform.TransformPoint(vertex)-origin;
                    if(Mathf.Abs(Vector3.Dot(d,up))>scale*.35f || Mathf.Abs(Vector3.Dot(d,right))>scale*.8f)continue;
                    float z=Vector3.Dot(d,front);
                    if(z>0 && z<scale*1.6f)depth=Mathf.Max(depth,z);
                }
                if(Application.isPlaying)Object.Destroy(mesh);else Object.DestroyImmediate(mesh);
            }
            return depth;
        }
    }
}
