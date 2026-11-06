using System;
using System.IO;
using System.Threading.Tasks;
using UnityEditor;
using UnityEngine;
using UniVRM10;
using Cortex.Vrm;

// Offline renders exercise the same imported rig as the player, without a camera
// stream, UDP receiver, or changes to the user's running viewer.
public static class CortexPreview
{
    private static Task work;
    public static void Render()
    {
        work = RenderAsync();
        EditorApplication.update += Poll;
    }
    private static void Poll()
    {
        if (!work.IsCompleted) return;
        EditorApplication.update -= Poll;
        if (work.IsFaulted) Debug.LogException(work.Exception);
        EditorApplication.Exit(work.IsFaulted ? 1 : 0);
    }
    private static async Task RenderAsync()
    {
        var model = await Vrm10.LoadPathAsync(Path.Combine(Application.streamingAssetsPath,"MaleAvatar.vrm"),canLoadVrm0X:true);
        model.UpdateType = Vrm10Instance.UpdateTypes.None;
        var bones = new Transform[38]; var rest = new Quaternion[38];
        var prefix = new[]{HumanBodyBones.Chest,HumanBodyBones.Head,HumanBodyBones.LeftUpperArm,HumanBodyBones.LeftLowerArm,HumanBodyBones.LeftHand,HumanBodyBones.RightUpperArm,HumanBodyBones.RightLowerArm,HumanBodyBones.RightHand};
        for(int i=0;i<8;++i)bones[i]=model.Runtime.ControlRig.GetBoneTransform(prefix[i]);
        string[] fingers={"Thumb","Index","Middle","Ring","Little"}, joints={"Proximal","Intermediate","Distal"};
        for(int s=0;s<2;++s)for(int f=0;f<5;++f)for(int j=0;j<3;++j)
            bones[8+s*15+f*3+j]=model.Runtime.ControlRig.GetBoneTransform((HumanBodyBones)Enum.Parse(typeof(HumanBodyBones),(s==0?"Left":"Right")+fingers[f]+joints[j]));
        for(int i=0;i<38;++i)if(bones[i]!=null)rest[i]=bones[i].localRotation;
        var leftEye=model.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.LeftEye);
        var rightEye=model.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.RightEye);
        var neck=model.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.Neck);
        var neckRest=neck!=null?neck.localRotation:Quaternion.identity;
        var headRoot=Quaternion.Inverse(model.transform.rotation)*bones[1].rotation;
        var headNeck=neck!=null?Quaternion.Inverse(neck.rotation)*bones[1].rotation:Quaternion.identity;
        Debug.Log($"PREVIEW anatomy head={bones[1].position:F4} leftEye={leftEye.position:F4} rightEye={rightEye.position:F4} shoulder={bones[2].position:F4} elbow={bones[3].position:F4} wrist={bones[4].position:F4}");
        // Appearance hook is shared with the standalone viewer.
        var style = RenAppearance.Apply(model);
        var mouth=new MouthExpressionDriver(model);
        Debug.Log("PREVIEW detailed mouth="+mouth.HasDetailedMouth);
        model.Runtime.Process();
        var presets=new ArmPosePresets(bones,rest,leftEye,rightEye,AvatarFaceGeometry.EyeSurfaceDepth(model,bones[1],leftEye,rightEye));
        var camera=new GameObject("Preview camera").AddComponent<Camera>();
        camera.backgroundColor=new Color(.035f,.045f,.065f);camera.clearFlags=CameraClearFlags.SolidColor;
        camera.transform.position=new Vector3(0,1.32f,2.05f);camera.transform.LookAt(new Vector3(0,1.25f,0));
        camera.fieldOfView=34;
        var light=new GameObject("Preview light").AddComponent<Light>();light.type=LightType.Directional;light.intensity=.8f;light.transform.rotation=Quaternion.Euler(25,160,0);
        RenderSettings.ambientMode=UnityEngine.Rendering.AmbientMode.Flat;
        RenderSettings.ambientLight=new Color(.35f,.38f,.42f);RenderSettings.skybox=null;
        string output=Path.GetFullPath(Path.Combine(Application.dataPath,"../../windows-studio/build/avatar-preview"));Directory.CreateDirectory(output);
        // Same neutral rig and lens in both frames: isolate viewpoint elevation
        // from tracking/calibration. No compensating head rotation is applied.
        bones[2].localRotation=rest[2]*Quaternion.Euler(0,0,72);bones[5].localRotation=rest[5]*Quaternion.Euler(0,0,-72);
        HeadMotionController.ApplyToRig(model.transform,bones[1],headRoot,neck,neckRest,headNeck,Quaternion.identity);
        model.Runtime.Process();await Task.Delay(150);
        foreach(var view in new[]{"LegacyViewNeutral","EyeLevelNeutral"})
        {
            camera.fieldOfView=60;
            if(view=="LegacyViewNeutral"){camera.transform.position=new Vector3(0,1.34f,1.45f);camera.transform.LookAt(new Vector3(0,1.28f,0));}
            else AvatarCameraFraming.Configure(camera,model);
            var target=new RenderTexture(1000,1100,24);camera.targetTexture=target;camera.Render();
            var shot=new Texture2D(1000,1100,TextureFormat.RGB24,false);RenderTexture.active=target;shot.ReadPixels(new Rect(0,0,1000,1100),0,0);shot.Apply();
            File.WriteAllBytes(Path.Combine(output,view+".png"),shot.EncodeToPNG());
            camera.targetTexture=null;RenderTexture.active=null;UnityEngine.Object.DestroyImmediate(target);UnityEngine.Object.DestroyImmediate(shot);
        }
        camera.fieldOfView=34;AvatarCameraFraming.Configure(camera,model,2.05f);
        foreach(var pose in new[]{ArmPoseMode.Camera,ArmPoseMode.CrossArms,ArmPoseMode.CoverEyes})
        {
            if(pose==ArmPoseMode.Camera){bones[2].localRotation=rest[2]*Quaternion.Euler(0,0,72);bones[5].localRotation=rest[5]*Quaternion.Euler(0,0,-72);}
            else for(int i=0;i<300;++i){presets.Apply(pose,camera.transform.position,1f/60);model.Runtime.Process();}
            model.Runtime.Process();
            await Task.Delay(150);
            Debug.Log($"PREVIEW pose {pose}: wrist={bones[4].position:F4} actual={model.Humanoid.GetBoneTransform(HumanBodyBones.LeftHand).position:F4}");
            foreach(var r in model.GetComponentsInChildren<SkinnedMeshRenderer>())r.updateWhenOffscreen=true;
            var target=new RenderTexture(1000,1100,24);camera.targetTexture=target;camera.Render();
            var image=new Texture2D(1000,1100,TextureFormat.RGB24,false);RenderTexture.active=target;image.ReadPixels(new Rect(0,0,1000,1100),0,0);image.Apply();
            File.WriteAllBytes(Path.Combine(output,pose+".png"),image.EncodeToPNG());
            camera.targetTexture=null;RenderTexture.active=null;UnityEngine.Object.DestroyImmediate(target);UnityEngine.Object.DestroyImmediate(image);
        }
        for(int i=0;i<38;++i)if(bones[i]!=null)bones[i].localRotation=rest[i];
        bones[2].localRotation=rest[2]*Quaternion.Euler(0,0,72);bones[5].localRotation=rest[5]*Quaternion.Euler(0,0,-72);
        model.Runtime.Process();await Task.Delay(150);
        // Three-quarter and full-body views expose flat silhouettes, bare knees
        // and disconnected garment seams which a front-only crop can conceal.
        foreach(var view in new[]{"ThreeQuarter","FullBody","Face","Expression","Smile","Grin","MouthOpen","MouthClosed","SmileSide","HeadUp","HeadDown"})
        {
            bones[1].localRotation=rest[1];if(neck!=null)neck.localRotation=neckRest;
            if(view=="ThreeQuarter"){camera.transform.position=new Vector3(1.1f,1.35f,2);camera.transform.LookAt(new Vector3(0,1.25f,0));camera.fieldOfView=34;}
            else if(view=="FullBody"){camera.transform.position=new Vector3(0,1.02f,3.5f);camera.transform.LookAt(new Vector3(0,.91f,0));camera.fieldOfView=33;}
            else{camera.transform.position=new Vector3(0,1.62f,.85f);camera.transform.LookAt(new Vector3(0,1.60f,0));camera.fieldOfView=32;}
            if(view=="SmileSide"){camera.transform.position=new Vector3(.40f,1.62f,.80f);camera.transform.LookAt(new Vector3(0,1.60f,0));camera.fieldOfView=32;}
            if(view=="HeadUp"||view=="HeadDown")HeadMotionController.ApplyToRig(model.transform,bones[1],headRoot,neck,neckRest,headNeck,Quaternion.Euler(view=="HeadUp"?-20:20,0,0));
            model.Runtime.Expression.SetWeight(ExpressionKey.BlinkLeft,0);
            mouth.Apply(view=="MouthOpen"?.85f:view=="Grin"?.22f:view=="Smile"||view=="SmileSide"?.04f:0,
                view=="Smile"||view=="SmileSide"?.8f:view=="Grin"?1:0,0);
            if(view=="Expression")
            {
                model.Runtime.Expression.SetWeight(ExpressionKey.BlinkLeft,1);
                mouth.Apply(.45f,.15f,0);
            }
            model.Runtime.Process();await Task.Delay(150);
            var target=new RenderTexture(1000,1100,24);camera.targetTexture=target;camera.Render();
            var image=new Texture2D(1000,1100,TextureFormat.RGB24,false);RenderTexture.active=target;image.ReadPixels(new Rect(0,0,1000,1100),0,0);image.Apply();
            File.WriteAllBytes(Path.Combine(output,view+".png"),image.EncodeToPNG());
            camera.targetTexture=null;RenderTexture.active=null;UnityEngine.Object.DestroyImmediate(target);UnityEngine.Object.DestroyImmediate(image);
        }
        UnityEngine.Object.DestroyImmediate(model.gameObject);
    }
}
