using System;
using System.Collections.Generic;
using UnityEngine;
using UniVRM10;

namespace Cortex.Vrm
{
    // Prefer the avatar's authored jaw/lip channels to stacking whole-face
    // emotions and vowels. Aperture and smile are independent measured inputs;
    // neither eyelid closure nor brow movement should erase a smile.
    public sealed class MouthExpressionDriver
    {
        private readonly Vrm10RuntimeExpression expressions;
        private static readonly ExpressionKey[] WholeMouth={ExpressionKey.Aa,ExpressionKey.Ih,ExpressionKey.Ou,ExpressionKey.Ee,ExpressionKey.Oh,
            ExpressionKey.Happy,ExpressionKey.Relaxed,ExpressionKey.Sad,ExpressionKey.Surprised,ExpressionKey.Angry};
        private readonly Dictionary<string,ExpressionKey> custom=new Dictionary<string,ExpressionKey>(StringComparer.OrdinalIgnoreCase);
        public bool HasDetailedMouth { get; }
        public MouthExpressionDriver(Vrm10Instance avatar)
        {
            expressions=avatar.Runtime.Expression;
            foreach(var key in expressions.ExpressionKeys)
                if(key.Preset==ExpressionPreset.custom)custom[key.Name]=key;
            HasDetailedMouth=custom.ContainsKey("JawOpen") && custom.ContainsKey("MouthSmileLeft") && custom.ContainsKey("MouthSmileRight");
        }
        private void Set(string name,float value)
        {if(custom.TryGetValue(name,out var key))expressions.SetWeight(key,Mathf.Clamp01(value));}
        public void Apply(float aperture,float smile,float brow)
        {
            aperture=Mathf.Clamp01(aperture);smile=Mathf.Clamp(smile,-1,1);brow=Mathf.Clamp(brow,-1,1);
            float happy=Mathf.SmoothStep(0,1,Mathf.InverseLerp(.025f,.95f,Mathf.Max(0,smile)));
            float grin=Mathf.SmoothStep(0,1,Mathf.InverseLerp(.35f,.95f,smile));
            float frown=Mathf.Max(0,-smile);
            // These presets affect the same vertices as the detailed channels.
            foreach(var key in WholeMouth)
                expressions.SetWeight(key,0);
            if(HasDetailedMouth)
            {
                Set("JawOpen",Mathf.Min(.95f,aperture*.92f+grin*.025f));
                Set("MouthSmileLeft",happy*.9f);Set("MouthSmileRight",happy*.9f);
                Set("MouthUpperUpLeft",grin*.12f*(1-aperture*.4f));Set("MouthUpperUpRight",grin*.12f*(1-aperture*.4f));
                Set("MouthLowerDownLeft",grin*.10f+aperture*.045f);Set("MouthLowerDownRight",grin*.10f+aperture*.045f);
                Set("MouthStretchLeft",grin*.08f);Set("MouthStretchRight",grin*.08f);
                Set("MouthFrownLeft",frown*.65f);Set("MouthFrownRight",frown*.65f);
                Set("CheekSquintLeft",happy*.12f);Set("CheekSquintRight",happy*.12f);
                Set("BrowInnerUp",Mathf.Max(0,brow)*.6f);
                Set("BrowOuterUpLeft",Mathf.Max(0,brow)*.3f);Set("BrowOuterUpRight",Mathf.Max(0,brow)*.3f);
                Set("BrowDownLeft",Mathf.Max(0,-brow)*.65f);Set("BrowDownRight",Mathf.Max(0,-brow)*.65f);
                return;
            }
            // Other VRMs may expose only standard expressions. Keep the total
            // vowel contribution bounded rather than adding A + O at full gain.
            float ee=grin*.20f*(1-aperture);
            expressions.SetWeight(ExpressionKey.Aa,aperture*(1-ee));
            expressions.SetWeight(ExpressionKey.Ee,ee);
            expressions.SetWeight(ExpressionKey.Happy,happy*.55f);
            expressions.SetWeight(ExpressionKey.Sad,frown*.20f);
            expressions.SetWeight(ExpressionKey.Surprised,Mathf.Max(0,brow-.2f)*.45f);
            expressions.SetWeight(ExpressionKey.Angry,Mathf.Max(0,-brow-.25f)*.4f);
        }
    }
}
