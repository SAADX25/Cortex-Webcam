using UnityEngine;

namespace Cortex.Vrm
{
    public enum ArmPoseMode { Camera, CrossArms, CoverEyes }

    // Uses the loaded avatar's actual bone lengths and anatomical axes. Presets
    // rotate the rig; they never translate joints or assume one model's scale.
    public sealed class ArmPosePresets
    {
        private readonly Transform[] bones;
        private readonly Quaternion[] rest;
        private readonly Transform leftEye, rightEye;
        private readonly float[] upper = new float[2], lower = new float[2], palm = new float[2];
        private readonly Vector3[] fingerDirection = new Vector3[2], dorsalNormal = new Vector3[2];
        private readonly Vector3 headUp, headLeft, headFront;
        private readonly float eyeSurfaceDepth;
        public bool Available { get; private set; }

        public ArmPosePresets(Transform[] bones, Quaternion[] rest, Transform leftEye, Transform rightEye, float eyeSurfaceDepth = 0)
        {
            this.bones = bones; this.rest = rest; this.leftEye = leftEye; this.rightEye = rightEye;
            Available = bones[0] != null && bones[1] != null;
            if(Available && bones[2] != null && bones[5] != null)
            {
                var up=(bones[1].position-bones[0].position).normalized;
                var left=(bones[2].position-bones[5].position).normalized;
                headUp=bones[1].InverseTransformDirection(up);
                headLeft=bones[1].InverseTransformDirection(left);
                Vector3 front=Vector3.Cross(left,up).normalized;
                if(Vector3.Dot(front,bones[1].forward)<0)front=-front;
                headFront=bones[1].InverseTransformDirection(front);
            }
            this.eyeSurfaceDepth = Mathf.Max(0,eyeSurfaceDepth);
            for (int side = 0; side < 2; ++side)
            {
                int b = side == 0 ? 2 : 5;
                if (bones[b] == null || bones[b+1] == null || bones[b+2] == null) { Available = false; continue; }
                upper[side] = Vector3.Distance(bones[b].position, bones[b+1].position);
                lower[side] = Vector3.Distance(bones[b+1].position, bones[b+2].position);
                Transform hand = bones[b+2], middle = bones[8+side*15+6];
                Transform index = bones[8+side*15+3], little = bones[8+side*15+12];
                Vector3 forward = middle != null ? middle.position-hand.position : (hand.position-bones[b+1].position)*.2f;
                palm[side] = Mathf.Max(forward.magnitude, (upper[side]+lower[side])*.10f);
                fingerDirection[side] = hand.InverseTransformDirection(forward.normalized);
                Vector3 normal = index != null && little != null
                    ? Vector3.Cross(index.position-hand.position,little.position-hand.position)*(side==0?1:-1)
                    : Vector3.Cross(forward,hand.forward);
                dorsalNormal[side] = hand.InverseTransformDirection(normal.normalized);
                if (upper[side] < .001f || lower[side] < .001f) Available = false;
            }
        }

        public void Apply(ArmPoseMode mode, Vector3 cameraPosition, float dt)
        {
            if (!Available || mode == ArmPoseMode.Camera) return;
            float blend = 1f-Mathf.Exp(-Mathf.Max(0,dt)/.10f);
            Vector3 up = (bones[1].position-bones[0].position).normalized;
            Vector3 left = (bones[2].position-bones[5].position).normalized;
            Vector3 front = Vector3.ProjectOnPlane(cameraPosition-bones[0].position,up).normalized;
            float scale = (upper[0]+lower[0]+upper[1]+lower[1])*.5f;
            for (int side = 0; side < 2; ++side)
            {
                int b = side == 0 ? 2 : 5;
                float sign = side == 0 ? 1 : -1;
                Vector3 target, pole, fingers;
                if (mode == ArmPoseMode.CrossArms)
                {
                    Vector3 opposite = bones[side==0?5:2].position;
                    target = opposite-up*scale*.20f+front*scale*(side==0?.24f:.15f);
                    pole = bones[b].position+left*sign*scale*.16f-up*scale*.54f+front*scale*.20f;
                    fingers = -left*sign;
                }
                else
                {
                    Vector3 faceUp=bones[1].TransformDirection(headUp).normalized;
                    Vector3 faceLeft=bones[1].TransformDirection(headLeft).normalized;
                    Vector3 faceFront=bones[1].TransformDirection(headFront).normalized;
                    Transform eye = side == 0 ? leftEye : rightEye;
                    Vector3 eyePosition = eye != null ? eye.position
                        : bones[1].position+faceUp*scale*.12f+faceLeft*sign*scale*.075f;
                    // Eye pivots are often deep inside the skull. Place the
                    // back of each hand beyond the measured facial surface,
                    // and separate the palms even on rigs with close eye pivots.
                    Vector3 eyeCenter=leftEye != null && rightEye != null ? (leftEye.position+rightEye.position)*.5f : bones[1].position+faceUp*scale*.12f;
                    float spread=Mathf.Max(Vector3.Dot(eyePosition-eyeCenter,faceLeft)*sign,scale*.082f);
                    eyePosition+=faceLeft*(sign*spread-Vector3.Dot(eyePosition-eyeCenter,faceLeft));
                    float clearance=Mathf.Max(eyeSurfaceDepth+scale*.045f,scale*.205f);
                    target = eyePosition-faceUp*palm[side]*.70f+faceFront*clearance;
                    pole = bones[b].position+left*sign*scale*.38f-up*scale*.32f+front*scale*.28f;
                    fingers = (faceUp+faceLeft*sign*.12f).normalized;
                    front = faceFront;
                }
                SolveArm(bones[b],bones[b+1],bones[b+2],target,pole,upper[side],lower[side],blend);
                AimHand(bones[b+2],fingerDirection[side],dorsalNormal[side],fingers,front,blend);
                for (int finger = 0; finger < 5; ++finger)
                    for (int joint = 0; joint < 3; ++joint)
                    {
                        int i = 8+side*15+finger*3+joint;
                        if (bones[i] == null) continue;
                        Quaternion targetRotation = rest[i];
                        if (mode == ArmPoseMode.CrossArms)
                            targetRotation *= Quaternion.Euler(0,0,sign*(finger==0?(joint==0?10:22):(joint==1?45:30)));
                        bones[i].localRotation = Quaternion.Slerp(bones[i].localRotation,targetRotation,blend);
                    }
            }
        }

        private static void SolveArm(Transform shoulder,Transform elbow,Transform hand,Vector3 target,
            Vector3 pole,float a,float b,float blend)
        {
            Vector3 delta = target-shoulder.position;
            if (delta.sqrMagnitude < 1e-10f) return;
            Vector3 direction = delta.normalized;
            float reach = Mathf.Clamp(delta.magnitude,Mathf.Abs(a-b)+.00001f,a+b-.00001f);
            Vector3 bend = Vector3.ProjectOnPlane(pole-shoulder.position,direction).normalized;
            if (bend.sqrMagnitude < .5f) return;
            float along = (a*a+reach*reach-b*b)/(2*reach);
            float height = Mathf.Sqrt(Mathf.Max(0,a*a-along*along));
            Vector3 desiredElbow = shoulder.position+direction*along+bend*height;
            Vector3 desiredHand = shoulder.position+direction*reach;
            Quaternion upperTarget = Quaternion.FromToRotation(elbow.position-shoulder.position,
                desiredElbow-shoulder.position)*shoulder.rotation;
            shoulder.rotation = Quaternion.Slerp(shoulder.rotation,upperTarget,blend);
            Quaternion lowerTarget = Quaternion.FromToRotation(hand.position-elbow.position,
                desiredHand-desiredElbow)*elbow.rotation;
            elbow.rotation = Quaternion.Slerp(elbow.rotation,lowerTarget,blend);
        }

        private static void AimHand(Transform hand,Vector3 localForward,Vector3 localNormal,
            Vector3 forward,Vector3 normal,float blend)
        {
            Quaternion aim = Quaternion.FromToRotation(hand.rotation*localForward,forward)*hand.rotation;
            Vector3 currentNormal = Vector3.ProjectOnPlane(aim*localNormal,forward);
            Vector3 wantedNormal = Vector3.ProjectOnPlane(normal,forward);
            if (currentNormal.sqrMagnitude > 1e-8f && wantedNormal.sqrMagnitude > 1e-8f)
                aim = Quaternion.AngleAxis(Vector3.SignedAngle(currentNormal,wantedNormal,forward),forward)*aim;
            hand.rotation = Quaternion.Slerp(hand.rotation,aim,blend);
        }
    }
}
