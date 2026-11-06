using UnityEngine;
using UniVRM10;

namespace Cortex.Vrm
{
    public static class AvatarCameraFraming
    {
        public static void Configure(Camera camera,Vrm10Instance avatar,float distance=1.45f)
        {
            if(camera==null || avatar==null)return;
            var rig=avatar.Runtime.ControlRig;
            var head=rig.GetBoneTransform(HumanBodyBones.Head);
            var hips=rig.GetBoneTransform(HumanBodyBones.Hips);
            var left=rig.GetBoneTransform(HumanBodyBones.LeftEye);
            var right=rig.GetBoneTransform(HumanBodyBones.RightEye);
            var armL=rig.GetBoneTransform(HumanBodyBones.LeftUpperArm);
            var armR=rig.GetBoneTransform(HumanBodyBones.RightUpperArm);
            float scale=armL!=null&&armR!=null?Vector3.Distance(armL.position,armR.position)/.298f:1;
            Vector3 eyes=left!=null&&right!=null?(left.position+right.position)*.5f:
                head!=null?head.position+avatar.transform.up*.065f*scale:avatar.transform.position+avatar.transform.up*1.615f*scale;
            Vector3 body=hips!=null?hips.position:eyes-avatar.transform.up*.60f*scale;
            Place(camera,avatar.transform,eyes,body,scale,distance);
        }
        public static void Place(Camera camera,Transform root,Vector3 eyes,Vector3 hips,float scale,float distance=1.45f)
        {
            // Framing the torso is independent of viewpoint height. A camera
            // below the eyes shows the underside of the chin even at zero
            // tracked head pitch. Keep the viewpoint at anatomical eye level.
            scale=Mathf.Max(.01f,scale);
            camera.transform.position=eyes+root.forward*distance*scale;
            camera.transform.LookAt(Vector3.Lerp(hips,eyes,.44f),root.up);
            camera.nearClipPlane=.02f*scale;
            camera.farClipPlane=Mathf.Max(10,10*scale);
        }
    }
}
