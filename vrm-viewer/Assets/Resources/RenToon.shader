Shader "Cortex/Ren Toon"
{
    Properties
    {
        _Color ("Color", Color) = (1,1,1,1)
        _MainTex ("Surface texture", 2D) = "white" {}
        _Hair ("Hair strands", Range(0,1)) = 0
        _Gloss ("Satin highlight", Range(0,1)) = 0.06
        _OutlineWidth ("Outline width", Float) = 0.00065
        [Enum(UnityEngine.Rendering.CullMode)] _CullMode ("Face culling", Float) = 0
        _Visibility ("Surface visibility", Range(0,1)) = 1
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }
        Cull [_CullMode]
        Pass
        {
            Tags { "LightMode"="ForwardBase" }
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"
            #include "Lighting.cginc"
            struct appdata { float4 vertex:POSITION; float3 normal:NORMAL; float2 uv:TEXCOORD0; };
            struct v2f { float4 position:SV_POSITION; float3 normal:TEXCOORD0; float3 view:TEXCOORD1; float2 uv:TEXCOORD2; };
            fixed4 _Color;
            sampler2D _MainTex;
            float _Hair, _Gloss, _Visibility;
            v2f vert(appdata v)
            {
                v2f o;o.position=UnityObjectToClipPos(v.vertex);o.normal=UnityObjectToWorldNormal(v.normal);
                o.view=_WorldSpaceCameraPos-mul(unity_ObjectToWorld,v.vertex).xyz;o.uv=v.uv;return o;
            }
            fixed4 frag(v2f i, fixed facing:VFACE):SV_Target
            {
                clip(_Visibility-.5);
                float3 n=normalize(i.normal)*(facing>0?1:-1);
                float diffuse=dot(n,normalize(_WorldSpaceLightPos0.xyz));
                float band=smoothstep(-.25,.30,diffuse);
                float3 view=normalize(i.view);
                float rim=pow(1-saturate(dot(n,view)),3)*.065;
                float3 halfVector=normalize(normalize(_WorldSpaceLightPos0.xyz)+view);
                float gloss=pow(saturate(dot(n,halfVector)),lerp(28,48,_Hair))*lerp(_Gloss,.23,_Hair);
                float strand=lerp(1,.94+.06*sin(i.uv.x*180),_Hair);
                float3 base=tex2D(_MainTex,i.uv).rgb*_Color.rgb;
                float3 shade=base*float3(.57,.61,.70);
                float3 lit=lerp(shade,base,band)*strand;
                return fixed4(lit+rim*_Color.rgb+gloss*float3(.72,.76,.85),1);
            }
            ENDCG
        }
        Pass
        {
            Cull Front
            CGPROGRAM
            #pragma vertex outlineVert
            #pragma fragment outlineFrag
            #include "UnityCG.cginc"
            struct appdata { float4 vertex:POSITION; float3 normal:NORMAL; };
            float _OutlineWidth;
            float4 outlineVert(appdata v):SV_POSITION
            { v.vertex.xyz+=v.normal*_OutlineWidth;return UnityObjectToClipPos(v.vertex); }
            fixed4 outlineFrag():SV_Target { clip(_OutlineWidth-.00001);return fixed4(.065,.070,.085,1); }
            ENDCG
        }
    }
}
