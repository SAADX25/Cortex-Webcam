using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using UnityEngine;
using UniVRM10;

namespace Cortex.Vrm
{
    public sealed class CortexVrmViewer : MonoBehaviour
    {
        public string ModelPath;
        public int MotionPort = 39539;
        public bool IdleMotion;
        public bool HandsEnabled {get;private set;}=true;
        private readonly ArmTrackingGate[] armTracking={new ArmTrackingGate(),new ArmTrackingGate()};
        private ArmPosePresets armPresets;
        private ArmPoseMode armPoseMode;
        private AvatarAppearanceToggle appearance;
        private MouthExpressionDriver mouthDriver;
        private readonly HeadMotionController headMotion=new HeadMotionController();
        private Quaternion headRestInRoot,headRestInNeck;
        private string requestedAppearance;
        private UdpClient socket;
        private Thread worker;
        private volatile bool stopping;
        private readonly object gate = new object();
        private MotionPacket latest;
        private double receivedAt;
        private ulong lastSequence, gaps;
        private bool hasSequence;
        private string status = "Loading VRM", networkError;
        private Vrm10Instance instance;
        private readonly Transform[] bones = new Transform[MotionPacket.BoneCount];
        private readonly Quaternion[] rest = new Quaternion[MotionPacket.BoneCount];
        private readonly Quaternion[] neutral = new Quaternion[MotionPacket.BoneCount];
        private readonly Quaternion[] trackedRotations = new Quaternion[MotionPacket.BoneCount];
        private readonly BoneTrackingState[] trackingStates = CreateTrackingStates();
        private MotionPacket observedPacket;
        private static BoneTrackingState[] CreateTrackingStates()
        {
            var states = new BoneTrackingState[MotionPacket.BoneCount];
            for (int i = 0; i < states.Length; ++i) states[i] = new BoneTrackingState();
            return states;
        }
        private Transform spineBone, upperChestBone, neckBone, leftShoulderBone, rightShoulderBone;
        private Quaternion spineRest, upperChestRest, neckRest, leftShoulderRest, rightShoulderRest;
        private float smoothBlinkL, smoothBlinkR, smoothMouth, smoothSmile, smoothBrow, smoothGazeX, smoothGazeY, smoothSwayX;
        private float nextAutoBlink = 2.5f, autoBlinkStart = -1f;
        private static readonly HumanBodyBones[] BoneMap = BuildMap();
        private static HumanBodyBones[] BuildMap()
        {
            var map = new HumanBodyBones[38];
            var prefix = new[] { HumanBodyBones.Chest, HumanBodyBones.Head, HumanBodyBones.LeftUpperArm,
                HumanBodyBones.LeftLowerArm, HumanBodyBones.LeftHand, HumanBodyBones.RightUpperArm,
                HumanBodyBones.RightLowerArm, HumanBodyBones.RightHand };
            Array.Copy(prefix, map, 8);
            string[] fingers = { "Thumb", "Index", "Middle", "Ring", "Little" };
            string[] joints = { "Proximal", "Intermediate", "Distal" };
            for (int side = 0; side < 2; ++side)
                for (int finger = 0; finger < 5; ++finger)
                    for (int joint = 0; joint < 3; ++joint)
                        map[8 + side * 15 + finger * 3 + joint] = (HumanBodyBones)Enum.Parse(typeof(HumanBodyBones),
                            (side == 0 ? "Left" : "Right") + fingers[finger] + joints[joint]);
            return map;
        }
        // Reflect the solver's anatomical X convention into the avatar rig:
        // quaternion (x,y,z,w) becomes (x,-y,-z,w).
        private static Quaternion FromPacket(float[] v, int j) => new Quaternion(v[j], -v[j+1], -v[j+2], v[j+3]);
        public void SetHandsEnabled(bool enabled)
        {
            if(HandsEnabled==enabled)return;
            HandsEnabled=enabled;armPoseMode=ArmPoseMode.Camera;
            foreach(var gate in armTracking)gate.Reset();
            for(int i=2;i<trackingStates.Length;++i)trackingStates[i].Reset();
        }
        private static int ArmSide(int bone)=>bone>=8?(bone-8)/15:(bone>=5?1:0);
        private Quaternion GetRelaxedPose(int i, float breath)
        {
            if (i == 2) return rest[2] * Quaternion.Euler(0f, 0f, 72f + breath * 0.9f);
            if (i == 5) return rest[5] * Quaternion.Euler(0f, 0f, -72f - breath * 0.9f);
            if (i == 3) return rest[3] * Quaternion.Euler(0f, -7f, 5f);
            if (i == 6) return rest[6] * Quaternion.Euler(0f, 7f, -5f);
            if (i >= 8)
            {
                int rel = i - 8, side = rel / 15, rem = rel % 15, finger = rem / 3, joint = rem % 3;
                float sign = side == 0 ? 1f : -1f;
                if (finger == 0)
                    return rest[i] * Quaternion.Euler(5f, -sign * (joint == 0 ? 14f : 6f), sign * (joint == 0 ? 10f : 8f));
                float curl = (10f + 3.2f * (finger - 1)) * (joint == 1 ? 1.15f : (joint == 0 ? 0.9f : 0.75f));
                return rest[i] * Quaternion.Euler(0f, 0f, sign * curl);
            }
            return rest[i];
        }
        private static double Now => System.Diagnostics.Stopwatch.GetTimestamp() / (double)System.Diagnostics.Stopwatch.Frequency;
        private async void Start()
        {
            try
            {
                Application.targetFrameRate = 60;
                string[] args = Environment.GetCommandLineArgs();
                for (int i = 0; i < args.Length - 1; ++i)
                {
                    if (args[i] == "--vrm") ModelPath = args[++i];
                    else if (args[i] == "--motion-port") MotionPort = int.Parse(args[++i]);
                    else if (args[i] == "--appearance") requestedAppearance = args[++i];
                }
                if (string.IsNullOrEmpty(ModelPath))
                {
                    var male = Path.Combine(Application.streamingAssetsPath, "MaleAvatar.vrm");
                    ModelPath = File.Exists(male) ? male : Path.Combine(Application.streamingAssetsPath, "Alicia.vrm");
                }
                if (!File.Exists(ModelPath)) throw new FileNotFoundException("Choose a .vrm with --vrm PATH", ModelPath);
                instance = await Vrm10.LoadPathAsync(ModelPath, canLoadVrm0X: true);
                if (this == null) { if (instance != null) Destroy(instance.gameObject); return; }
                instance.UpdateType = Vrm10Instance.UpdateTypes.None;
                for (int i = 0; i < bones.Length; ++i)
                {
                    bones[i] = instance.Runtime.ControlRig.GetBoneTransform(BoneMap[i]);
                    if (bones[i] != null) rest[i] = bones[i].localRotation;
                    neutral[i] = Quaternion.identity;
                }
                // Match the solver's chest parent even when a model omits Chest, and cache secondary anatomy bones.
                spineBone = instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.Spine);
                if (spineBone != null) spineRest = spineBone.localRotation;
                if (bones[0] == null) { bones[0] = spineBone; rest[0] = spineRest; spineBone = null; }
                upperChestBone = instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.UpperChest);
                if (upperChestBone != null) upperChestRest = upperChestBone.localRotation;
                neckBone = instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.Neck);
                if (neckBone != null) neckRest = neckBone.localRotation;
                if(bones[1]!=null)
                {
                    headRestInRoot=Quaternion.Inverse(instance.transform.rotation)*bones[1].rotation;
                    headRestInNeck=neckBone!=null?Quaternion.Inverse(neckBone.rotation)*bones[1].rotation:Quaternion.identity;
                }
                leftShoulderBone = instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.LeftShoulder);
                if (leftShoulderBone != null) leftShoulderRest = leftShoulderBone.localRotation;
                rightShoulderBone = instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.RightShoulder);
                if (rightShoulderBone != null) rightShoulderRest = rightShoulderBone.localRotation;
                instance.LookAtTargetType = VRM10ObjectLookAt.LookAtTargetTypes.YawPitchValue;
                armPresets = new ArmPosePresets(bones,rest,
                    instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.LeftEye),
                    instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.RightEye),
                    AvatarFaceGeometry.EyeSurfaceDepth(instance,bones[1],
                        instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.LeftEye),
                        instance.Runtime.ControlRig.GetBoneTransform(HumanBodyBones.RightEye)));
                // Only the bundled male base opts in automatically. A user's
                // separate VRM retains its authored appearance by default.
                if(Path.GetFileName(ModelPath).Equals("MaleAvatar.vrm",StringComparison.OrdinalIgnoreCase))
                    MouthSurface.RemoveTeeth(instance);
                if(string.Equals(requestedAppearance,"ren",StringComparison.OrdinalIgnoreCase) ||
                    (requestedAppearance==null && Path.GetFileName(ModelPath).Equals("MaleAvatar.vrm",StringComparison.OrdinalIgnoreCase)))
                    appearance = new AvatarAppearanceToggle(instance);
                mouthDriver=new MouthExpressionDriver(instance);
                for (int i = 0; i < bones.Length; ++i)
                    if (bones[i] != null) bones[i].localRotation = GetRelaxedPose(i, 0f);
                instance.Runtime.Process();
                AvatarCameraFraming.Configure(Camera.main,instance);
                headMotion.RecenterAfterInput(1.5);
                socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, MotionPort));
                socket.Client.ReceiveTimeout = 250;
                worker = new Thread(Receive) { IsBackground = true, Name = "Cortex motion" }; worker.Start();
                status = "Waiting for Cortex body/hand samples";
                Debug.Log("Cortex avatar ready: "+Path.GetFileName(ModelPath)+", appearance="+(appearance != null?"Ren inspired":"original")+", motion port="+MotionPort);
            }
            catch (Exception e) { status = e.Message; Debug.LogException(e); }
        }
        private void Receive()
        {
            while (!stopping)
            {
                try
                {
                    IPEndPoint source = new IPEndPoint(IPAddress.Loopback, 0);
                    byte[] bytes = socket.Receive(ref source);
                    if (!IPAddress.IsLoopback(source.Address) || !MotionPacket.TryParse(bytes, out var packet)) continue;
                    lock (gate)
                    {
                        double now = Now;
                        // A one-second gap permits publisher restart/sequence reset.
                        if (hasSequence && now - receivedAt < 1 && packet.Sequence <= lastSequence) continue;
                        if (hasSequence && packet.Sequence > lastSequence + 1) gaps += packet.Sequence - lastSequence - 1;
                        hasSequence = true; lastSequence = packet.Sequence; latest = packet; receivedAt = now;
                    }
                }
                catch (SocketException e) { if (e.SocketErrorCode != SocketError.TimedOut && !stopping) { lock(gate) networkError=e.Message; return; } }
                catch (ObjectDisposedException) { return; }
            }
        }
        private void Update()
        {
            if (instance == null) return;
            MotionPacket packet; double age, sampleTime; ulong lost; string error;
            lock (gate) { packet = latest; sampleTime = receivedAt; age = Now - sampleTime; lost = gaps; error=networkError; }
            float dt = Time.unscaledDeltaTime;
            float t = Time.unscaledTime;
            // Camera tracking stays still when the person is still. Procedural
            // swaying is an explicit option rather than an extra motion source.
            float breath = IdleMotion ? Mathf.Sin(t * 1.85f) : 0f;
            float microSway = IdleMotion ? Mathf.Sin(t * 0.72f) : 0f;
            bool fresh = packet != null && age < .30;
            if (fresh && !ReferenceEquals(packet, observedPacket))
            {
                observedPacket = packet;
                bool left=HandsEnabled&&armTracking[0].Observe(ArmTrackingGate.Reliable(packet,0),sampleTime);
                bool right=HandsEnabled&&armTracking[1].Observe(ArmTrackingGate.Reliable(packet,1),sampleTime);
                for (int i = 0; i < bones.Length; ++i)
                {
                    if(i>=2 && !(ArmSide(i)==0?left:right))continue;
                    // Coarse Pose and fitted face angles have different neutral
                    // biases. Establish neutral only from a valid face fit;
                    // retain the existing short-occlusion fallback afterwards.
                    if(i==1 && !headMotion.IsCentered && !HeadMotionController.HasMeasuredFace(packet))continue;
                    if (trackingStates[i].Observe(packet.Values[i * 5 + 4], sampleTime))
                    {
                        trackedRotations[i] = FromPacket(packet.Values, i * 5);
                        if(i==1)headMotion.Observe(trackedRotations[i],packet.Values[4]>=.35f?FromPacket(packet.Values,0):Quaternion.identity,
                            packet.Values[i*5+4],sampleTime);
                    }
                }
            }
            double renderNow = Now;
            float swayTarget = (fresh && packet.HasExpressions) ? Mathf.Clamp(packet.Expressions[7], -1f, 1f) : 0f;
            smoothSwayX = Mathf.Lerp(smoothSwayX, swayTarget, 1f - Mathf.Exp(-dt / .09f));
            Quaternion breathChest = Quaternion.Euler(breath * 1.3f, microSway * 0.8f - smoothSwayX * 6f, -smoothSwayX * 8f);
            int measured = 0;
            for (int i = 0; i < bones.Length; ++i)
            {
                if (bones[i] == null) continue;
                if(i==1)continue;
                if (i >= 2 && armPoseMode != ArmPoseMode.Camera) continue;
                Quaternion target = GetRelaxedPose(i, i>=2?0:breath);
                if (i == 0) target = rest[0] * breathChest;
                Quaternion relaxed = target;
                float trackingWeight = i>=2
                    ? (HandsEnabled?armTracking[ArmSide(i)].Weight(renderNow)*trackingStates[i].Weight(armTracking[ArmSide(i)].LastAcceptedTime):0)
                    : trackingStates[i].Weight(renderNow);
                if (trackingWeight > 0)
                {
                    Quaternion delta = Quaternion.Inverse(neutral[i]) * trackedRotations[i];
                    if (i == 0)
                    {
                        Quaternion fullTorso = Quaternion.Slerp(breathChest, delta * breathChest, trackingWeight);
                        if (spineBone != null)
                        {
                            Quaternion spineTarget = spineRest * Quaternion.Slerp(Quaternion.identity, fullTorso, 0.45f);
                            spineBone.localRotation = Quaternion.Slerp(spineBone.localRotation, spineTarget, 1f - Mathf.Exp(-dt / .055f));
                            target = rest[0] * Quaternion.Slerp(Quaternion.identity, fullTorso, 0.55f);
                        }
                        else target = rest[0] * fullTorso;
                        if (upperChestBone != null)
                        {
                            Quaternion ucTarget = upperChestRest * Quaternion.Slerp(Quaternion.identity, fullTorso, 0.2f);
                            upperChestBone.localRotation = Quaternion.Slerp(upperChestBone.localRotation, ucTarget, 1f - Mathf.Exp(-dt / .055f));
                        }
                    }
                    else
                    {
                        target = Quaternion.Slerp(relaxed, rest[i] * delta, trackingWeight);
                    }
                    ++measured;
                }
                else if (i == 0)
                {
                    if (spineBone != null)
                    {
                        Quaternion spineTarget = spineRest * Quaternion.Slerp(Quaternion.identity, breathChest, 0.45f);
                        spineBone.localRotation = Quaternion.Slerp(spineBone.localRotation, spineTarget, 1f - Mathf.Exp(-dt / .065f));
                    }
                    if (upperChestBone != null)
                        upperChestBone.localRotation = Quaternion.Slerp(upperChestBone.localRotation, upperChestRest, 1f - Mathf.Exp(-dt / .065f));
                }
                float angle = Quaternion.Angle(bones[i].localRotation, target);
                bool isHandOrFinger = i == 4 || i == 7 || i >= 8;
                float tau = isHandOrFinger
                    ? Mathf.Lerp(0.038f, 0.012f, Mathf.Clamp01(angle / 25f))
                    : Mathf.Lerp(0.065f, 0.024f, Mathf.Clamp01(angle / 35f));
                var eased=Quaternion.Slerp(bones[i].localRotation,target,1f-Mathf.Exp(-dt/tau));
                bones[i].localRotation=i>=2?Quaternion.RotateTowards(bones[i].localRotation,eased,(i>=8?900:(isHandOrFinger?540:420))*Mathf.Min(dt,.1f)):eased;
            }
            float headWeight=trackingStates[1].Weight(renderNow);
            HeadMotionController.ApplyToRig(instance.transform,bones[1],headRestInRoot,neckBone,neckRest,headRestInNeck,
                headMotion.Render(dt,headWeight));
            if(headWeight>0 && headMotion.IsCentered)++measured;
            if (leftShoulderBone != null && bones[2] != null && armPoseMode == ArmPoseMode.Camera && HandsEnabled && armTracking[0].Weight(renderNow)>0)
            {
                float lift = Mathf.Clamp01(((bones[2].localRotation * Vector3.left).y + 0.75f) / 1.15f);
                Quaternion shTarget = leftShoulderRest * Quaternion.Euler(0f, -lift * 5f, -lift * 14f - breath * 0.7f);
                leftShoulderBone.localRotation = Quaternion.Slerp(leftShoulderBone.localRotation, shTarget, 1f - Mathf.Exp(-dt / .045f));
            }
            if (rightShoulderBone != null && bones[5] != null && armPoseMode == ArmPoseMode.Camera && HandsEnabled && armTracking[1].Weight(renderNow)>0)
            {
                float lift = Mathf.Clamp01(((bones[5].localRotation * Vector3.right).y + 0.75f) / 1.15f);
                Quaternion shTarget = rightShoulderRest * Quaternion.Euler(0f, lift * 5f, lift * 14f + breath * 0.7f);
                rightShoulderBone.localRotation = Quaternion.Slerp(rightShoulderBone.localRotation, shTarget, 1f - Mathf.Exp(-dt / .045f));
            }
            if(leftShoulderBone!=null && (!HandsEnabled || armTracking[0].Weight(renderNow)==0))
                leftShoulderBone.localRotation=Quaternion.Slerp(leftShoulderBone.localRotation,leftShoulderRest,1f-Mathf.Exp(-dt/.12f));
            if(rightShoulderBone!=null && (!HandsEnabled || armTracking[1].Weight(renderNow)==0))
                rightShoulderBone.localRotation=Quaternion.Slerp(rightShoulderBone.localRotation,rightShoulderRest,1f-Mathf.Exp(-dt/.12f));
            UpdateExpressions(packet, fresh, dt, t);
            if (HandsEnabled && armPoseMode != ArmPoseMode.Camera)
            {
                float blend = 1f-Mathf.Exp(-dt/.10f);
                if(leftShoulderBone != null)leftShoulderBone.localRotation=Quaternion.Slerp(leftShoulderBone.localRotation,leftShoulderRest,blend);
                if(rightShoulderBone != null)rightShoulderBone.localRotation=Quaternion.Slerp(rightShoulderBone.localRotation,rightShoulderRest,blend);
                armPresets?.Apply(armPoseMode,Camera.main != null?Camera.main.transform.position:instance.transform.position+Vector3.forward,dt);
            }
            instance.Runtime.Process();
            status = error ?? (packet == null ? "Waiting for motion on 127.0.0.1:" + MotionPort :
                "Measured bones " + measured + "/38" + (packet.HasExpressions ? " + Face/Gaze" : "") + " | received age " + (age*1000).ToString("F0") + " ms | sequence gaps " + lost);
            if(error==null && fresh && !headMotion.IsCentered)status+=" | Centering head: look straight ahead";
            if(!HandsEnabled)status+=" | Hands paused";
        }
        private void UpdateExpressions(MotionPacket packet, bool fresh, float dt, float t)
        {
            float targetBlinkL = 0f, targetBlinkR = 0f, targetMouth = 0f, targetSmile = 0f, targetBrow = 0f, targetGazeX = 0f, targetGazeY = 0f;
            if (fresh && packet.HasExpressions)
            {
                targetBlinkL = Mathf.Clamp01(packet.Expressions[0]);
                targetBlinkR = Mathf.Clamp01(packet.Expressions[1]);
                targetMouth = Mathf.Clamp01(packet.Expressions[2]);
                targetSmile = Mathf.Clamp(packet.Expressions[3], -1f, 1f);
                targetBrow = Mathf.Clamp(packet.Expressions[4], -1f, 1f);
                targetGazeX = Mathf.Clamp(packet.Expressions[5], -1f, 1f);
                targetGazeY = Mathf.Clamp(packet.Expressions[6], -1f, 1f);
                if (targetBlinkL > 0.5f || targetBlinkR > 0.5f)
                    nextAutoBlink = t + UnityEngine.Random.Range(3.2f, 5.5f);
            }
            bool measuredFace = fresh && packet.HasExpressions;
            if(measuredFace) autoBlinkStart=-1f;
            if (!measuredFace && IdleMotion && t >= nextAutoBlink)
            {
                autoBlinkStart = t;
                nextAutoBlink = t + UnityEngine.Random.Range(3.2f, 5.5f);
            }
            if (autoBlinkStart >= 0f)
            {
                float phase = (t - autoBlinkStart) / 0.15f;
                if (phase < 1f)
                {
                    float autoVal = Mathf.Sin(phase * Mathf.PI);
                    targetBlinkL = Mathf.Max(targetBlinkL, autoVal);
                    targetBlinkR = Mathf.Max(targetBlinkR, autoVal);
                }
                else autoBlinkStart = -1f;
            }
            float blinkBlend = 1f - Mathf.Exp(-dt / .022f);
            // Suppress tiny lip/gaze noise while retaining a fast response to
            // deliberate movement. Camera blinks take precedence over idle.
            targetMouth=Mathf.Clamp01((targetMouth-.025f)/.975f);
            targetGazeX=QuietTarget(smoothGazeX,targetGazeX,.018f);
            targetGazeY=QuietTarget(smoothGazeY,targetGazeY,.018f);
            targetBrow=QuietTarget(smoothBrow,targetBrow,.025f);
            targetSmile=QuietTarget(smoothSmile,targetSmile,.018f);
            float mouthBlend = 1f - Mathf.Exp(-dt / (targetMouth>smoothMouth?.030f:.055f));
            float exprBlend = 1f - Mathf.Exp(-dt / .065f);
            smoothBlinkL = Mathf.Lerp(smoothBlinkL, targetBlinkL, blinkBlend);
            smoothBlinkR = Mathf.Lerp(smoothBlinkR, targetBlinkR, blinkBlend);
            smoothMouth = Mathf.Lerp(smoothMouth, targetMouth, mouthBlend);
            smoothSmile = Mathf.Lerp(smoothSmile, targetSmile, exprBlend);
            smoothBrow = Mathf.Lerp(smoothBrow, targetBrow, exprBlend);
            smoothGazeX = Mathf.Lerp(smoothGazeX, targetGazeX, exprBlend);
            smoothGazeY = Mathf.Lerp(smoothGazeY, targetGazeY, exprBlend);

            var expr = instance.Runtime.Expression;
            expr.SetWeight(ExpressionKey.BlinkLeft, smoothBlinkL);
            expr.SetWeight(ExpressionKey.BlinkRight, smoothBlinkR);
            mouthDriver?.Apply(smoothMouth,smoothSmile,smoothBrow);
            instance.Runtime.LookAt.SetYawPitchManually(-smoothGazeX * 14f, -smoothGazeY * 10f);
        }
        private static float QuietTarget(float current,float target,float band)
            => Mathf.Abs(target-current)<band ? current : target;
        private void OnGUI()
        {
            if(GUI.Button(new Rect(515,65,175,30),HandsEnabled?"Disable hands":"Enable hands"))SetHandsEnabled(!HandsEnabled);
            IdleMotion = GUI.Toggle(new Rect(15,105,220,25),IdleMotion,"Idle breathing and sway");
            bool guiEnabled=GUI.enabled;GUI.enabled=guiEnabled&&HandsEnabled;
            if(armPresets != null && armPresets.Available)
                armPoseMode=(ArmPoseMode)GUI.Toolbar(new Rect(15,140,420,32),(int)armPoseMode,new[]{"Camera","Cross arms","Cover eyes"});
            GUI.enabled=guiEnabled;
            if(appearance != null)
                appearance.SetEnabled(GUI.Toggle(new Rect(15,182,290,25),appearance.Enabled,"Ren inspired 3D outfit"));
            GUI.Label(new Rect(15,15,Screen.width-30,60),status);
            if(GUI.Button(new Rect(15,65,170,30),"Center head"))headMotion.Recenter(Now+1);
            if(GUI.Button(new Rect(195,65,310,30),"Calibrate arms in T-pose with open hands"))
            {
                lock(gate) if(latest != null && Now-receivedAt<.25)
                    for(int i=0;i<neutral.Length;++i) if(latest.Values[i*5+4]>=.5f)
                    { int j=i*5; neutral[i]=FromPacket(latest.Values,j); }
                headMotion.Recenter(Now+1);
            }
        }
        private void OnDestroy()
        {
            stopping = true; socket?.Close(); worker?.Join(1000);
            if(instance != null) Destroy(instance.gameObject);
        }
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void Bootstrap()
        {
            if (FindAnyObjectByType<CortexVrmViewer>() != null) return;
            new GameObject("Cortex VRM").AddComponent<CortexVrmViewer>();
            var camera = Camera.main;
            if (camera == null)
            {
                camera = new GameObject("Camera").AddComponent<Camera>();
                camera.tag = "MainCamera";
            }
            camera.clearFlags = CameraClearFlags.SolidColor;
            camera.backgroundColor = new Color(.035f, .045f, .065f);
            camera.transform.position = new Vector3(0, 1.615f, 1.45f);
            camera.transform.LookAt(new Vector3(0, 1.28f, 0));
            foreach (var l in FindObjectsByType<Light>(FindObjectsSortMode.None)) Destroy(l.gameObject);
            var light = new GameObject("Key light").AddComponent<Light>();
            light.type = LightType.Directional;
            light.intensity = 0.65f;
            light.transform.rotation = Quaternion.Euler(25, 160, 0);
            RenderSettings.ambientMode = UnityEngine.Rendering.AmbientMode.Flat;
            RenderSettings.ambientLight = new Color(.35f, .38f, .42f);
            RenderSettings.skybox = null;
        }
    }
}
