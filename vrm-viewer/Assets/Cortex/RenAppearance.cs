using System;
using System.Collections.Generic;
using UnityEngine;
using UniVRM10;

namespace Cortex.Vrm
{
    // Original, procedural outfit inspired by the supplied Ren reference. This
    // keeps the imported face, expression blend shapes and humanoid skeleton.
    // It does not convert, extract or repackage the Live2D character's textures.
    public static class RenAppearance
    {
        public static GameObject Apply(Vrm10Instance avatar)
        {
            if (avatar == null) throw new ArgumentNullException(nameof(avatar));
            var existing = avatar.transform.Find("Ren inspired outfit");
            if (existing != null) return existing.gameObject;
            Transform Bone(HumanBodyBones id) => avatar.Humanoid.GetBoneTransform(id);
            var skeleton = new[]{ Bone(HumanBodyBones.Hips), Bone(HumanBodyBones.Spine),
                Bone(HumanBodyBones.Chest) ?? Bone(HumanBodyBones.Spine),
                Bone(HumanBodyBones.UpperChest) ?? Bone(HumanBodyBones.Chest) ?? Bone(HumanBodyBones.Spine),
                Bone(HumanBodyBones.Head), Bone(HumanBodyBones.LeftUpperArm),Bone(HumanBodyBones.LeftLowerArm),
                Bone(HumanBodyBones.RightUpperArm),Bone(HumanBodyBones.RightLowerArm),
                Bone(HumanBodyBones.LeftUpperLeg),Bone(HumanBodyBones.LeftLowerLeg),
                Bone(HumanBodyBones.RightUpperLeg),Bone(HumanBodyBones.RightLowerLeg) };
            foreach(var bone in skeleton) if(bone == null) throw new InvalidOperationException("Ren outfit needs a humanoid torso, head and arms");
            var root = new GameObject("Ren inspired outfit");root.transform.SetParent(avatar.transform,false);
            var space = avatar.transform;
            Vector3 Local(Transform t) => space.InverseTransformPoint(t.position);
            Vector3 hips=Local(skeleton[0]), chest=Local(skeleton[2]), head=Local(skeleton[4]);
            Vector3 neck=Local(Bone(HumanBodyBones.Neck));
            Vector3 shoulderL=Local(skeleton[5]), shoulderR=Local(skeleton[7]);
            float unit = Vector3.Distance(shoulderL,shoulderR)/.298f;
            // Outfit dimensions are in an anatomical coordinate frame, so yaw
            // and root translation do not change garment fit.
            float hip=hips.y, shoulder=(shoulderL.y+shoulderR.y)*.5f, center=(shoulderL.x+shoulderR.x)*.5f;
            var white=Material("Pearl coat",new Color(.88f,.89f,.91f));
            var black=Material("Charcoal panels",new Color(.075f,.082f,.10f));
            var gold=Material("Ochre lining",new Color(.93f,.58f,.19f));
            var shirt=Material("Graphite shirt",new Color(.16f,.17f,.20f));
            var silver=Material("Silver details",new Color(.69f,.74f,.80f));
            var hair=Material("Charcoal hair layers",new Color(.19f,.175f,.20f));
            var hairShade=Material("Hair undertone",new Color(.13f,.17f,.20f));
            var gray=Material("Graphite sleeve insets",new Color(.30f,.32f,.36f));
            var pants=Material("Tailored charcoal trousers",new Color(.085f,.093f,.115f));
            hair.SetFloat("_Hair",1);hairShade.SetFloat("_Hair",1);
            silver.SetFloat("_Gloss",.7f);gold.SetFloat("_Gloss",.12f);
            var mesh=new Garment(root.transform,skeleton,new[]{white,black,gold,shirt,silver,hair,hairShade,gray,pants});
            BoneWeight Torso(float y)
            {
                if(y<hip+.09f*unit)return Weight(0);
                float t=Mathf.InverseLerp(hip+.09f*unit,shoulder-.05f*unit,y);
                return new BoneWeight{boneIndex0=1,boneIndex1=3,weight0=1-t,weight1=t};
            }
            Vector3 P(float x,float y,float z) => new Vector3(center+x*unit,y,z*unit);
            Vector3 Coat(float u,float v)
            {
                float opening=Mathf.Lerp(19,32,v)*Mathf.Deg2Rad;
                float theta=Mathf.Lerp(opening,Mathf.PI*2-opening,u);
                float y=Mathf.Lerp(hip-.18f*unit,shoulder+.012f*unit,v);
                float width=v<.48f?Mathf.Lerp(.203f,.168f,v/.48f):Mathf.Lerp(.168f,.174f,(v-.48f)/.52f);
                float fold=.0035f*Mathf.Sin(theta*9+v*4)*Mathf.Sin(v*Mathf.PI);
                float depth=Mathf.Lerp(.16f,.14f,v);
                y-=unit*.016f*Mathf.Pow(Mathf.Abs(Mathf.Sin(theta)),6)*Mathf.Pow(v,8);
                return P(Mathf.Sin(theta)*(width+fold),y,Mathf.Cos(theta)*(depth+fold)-.005f);
            }
            // Dark inner shirt; the neckline is cut into a shallow V at front.
            mesh.Tube(48,9,(u,v)=>{
                float theta=u*Mathf.PI*2;
                float top=neck.y-unit*(.075f+.075f*Mathf.Max(0,Mathf.Cos(theta)));
                float y=Mathf.Lerp(hip-.035f*unit,top,v);
                float width=Mathf.Lerp(.153f,.165f,v);
                return P(Mathf.Sin(theta)*width,y,Mathf.Cos(theta)*.126f-.003f);
            },3,p=>Torso(p.y));
            // Open long coat: back and two continuous front panels, with a gap
            // for the shirt. Front edges widen slightly toward the hem.
            mesh.Tube(72,32,Coat,0,p=>Torso(p.y));
            for(int side=0;side<2;++side)
            {
                float sign=side==0?1:-1;
                // Gold zipper guards and dark vertical inset beside them.
                for(int strip=0;strip<2;++strip)
                {
                    // Follow the curved front edge instead of floating a flat
                    // vertical strip in front of the body.
                    mesh.Tube(2,32,(u,v)=>{
                        Vector3 edge=Coat(side==0?0:1,v);
                        return edge+new Vector3(sign*((strip==0?.002f:.011f)+u*.008f),0,.0015f)*unit;
                    },strip==0?2:1,p=>Torso(p.y));
                }
                int upper=side==0?5:7, lower=upper+1;
                Vector3 a=Local(skeleton[upper]),b=Local(skeleton[lower]);
                Vector3 wrist=Local(Bone(side==0?HumanBodyBones.LeftHand:HumanBodyBones.RightHand));
                // Long sleeves with smoothly blended elbow weights, unlike
                // rigid cylinders attached at the wrist or stretched joints.
                Vector3 Sleeve(float u,float v,float offset=0)
                {
                    Vector3 c=v<.48f?Vector3.Lerp(a,b,v/.48f):Vector3.Lerp(b,wrist,(v-.48f)/.52f);
                    float radius=v<.48f?Mathf.Lerp(.074f,.063f,v/.48f):Mathf.Lerp(.063f,.043f,(v-.48f)/.52f);
                    radius+=.006f*Mathf.Sin(v*37)*Mathf.Exp(-Mathf.Pow((v-.55f)/.20f,2));
                    radius+=.004f*Mathf.Sin(v*53)*Mathf.Exp(-Mathf.Pow((v-.87f)/.11f,2));
                    float theta=u*Mathf.PI*2;
                    return c+new Vector3(0,Mathf.Cos(theta)*(radius+offset)*unit,Mathf.Sin(theta)*(radius+offset)*unit);
                }
                BoneWeight SleeveWeight(Vector3 p)
                {
                    float t=Mathf.InverseLerp(a.x,wrist.x,p.x);
                    float w=Mathf.SmoothStep(0,1,Mathf.InverseLerp(.32f,.66f,t));
                    return new BoneWeight{boneIndex0=upper,boneIndex1=lower,weight0=1-w,weight1=w};
                }
                mesh.Tube(40,40,(u,v)=>Sleeve(u,v),0,SleeveWeight);
                float anatomicalSign=Mathf.Sign(a.x-center);
                mesh.Tube(32,12,(u,v)=>{
                    Vector3 c=Vector3.Lerp(new Vector3(center+anatomicalSign*.103f*unit,a.y+.006f*unit,a.z),a,v);
                    float r=.075f*Mathf.Sin(v*Mathf.PI*.5f)*unit,t=u*Mathf.PI*2;
                    return c+new Vector3(0,Mathf.Cos(t)*r,Mathf.Sin(t)*r);
                },1,p=>Weight(upper));
                // Raised charcoal shoulder yoke, gold piping, and a shaped
                // gray forearm inset. Their vertices follow the sleeve folds.
                mesh.Tube(32,8,(u,v)=>Sleeve(u,Mathf.Lerp(.025f,.20f,v),.0025f),1,SleeveWeight);
                mesh.Tube(32,2,(u,v)=>Sleeve(u,Mathf.Lerp(.20f,.212f,v),.003f),0,SleeveWeight);
                mesh.Tube(24,32,(u,v)=>Sleeve(Mathf.Lerp(.48f,.99f,u),Mathf.Lerp(.35f+.08f*Mathf.Sin(u*Mathf.PI),.88f,v),.005f),7,SleeveWeight);
                // Dark cuff and a narrow gold piping ring.
                foreach(int band in new[]{0,1})mesh.Tube(24,2,(u,v)=>{
                    float t=band==0?Mathf.Lerp(.90f,.995f,v):Mathf.Lerp(.88f,.90f,v);
                    Vector3 c=Vector3.Lerp(b,wrist,t);float theta=u*Mathf.PI*2;
                    return c+new Vector3(0,Mathf.Cos(theta)*.0475f*unit,Mathf.Sin(theta)*.0475f*unit);
                },band==0?1:2,p=>Weight(lower));
                // Front welt pocket and metal pull.
                mesh.Ribbon(P(sign*.14f,hip+.05f*unit,.118f),P(sign*.13f,hip+.18f*unit,.124f),.014f*unit,1,p=>Torso(p.y));
                mesh.Ribbon(P(sign*.14f,hip+.055f*unit,.121f),P(sign*.138f,hip+.08f*unit,.124f),.004f*unit,4,p=>Torso(p.y));
            }
            // Tall, flared collar: gold inside, charcoal outside and white rim.
            foreach(int layer in new[]{0,1})mesh.Tube(48,5,(u,v)=>{
                float theta=Mathf.Lerp(48,312,u)*Mathf.Deg2Rad;
                float y=neck.y+unit*Mathf.Lerp(-.085f,.045f,v);
                float x=Mathf.Lerp(.106f,.155f,v)*Mathf.Sin(theta);
                float z=Mathf.Lerp(.074f,.112f,v)*Mathf.Cos(theta);
                return P(x*(layer==0?1.02f:1),y,z*(layer==0?1.02f:1));
            },layer==0?1:2,p=>Weight(3));
            for(int side=0;side<2;++side)
            {
                float sign=side==0?1:-1;
                // Folded front lapels have thickness and a convex cross section.
                mesh.Tube(12,16,(u,v)=>{
                    Vector3 inner=Vector3.Lerp(P(sign*.068f,neck.y-.135f*unit,.127f),P(sign*.093f,neck.y+.038f*unit,.078f),v);
                    Vector3 outer=Vector3.Lerp(P(sign*.168f,shoulder-.012f*unit,.078f),P(sign*.188f,neck.y-.015f*unit,.012f),v);
                    return Vector3.Lerp(inner,outer,u)+Vector3.forward*(Mathf.Sin(u*Mathf.PI)*.012f*unit);
                },1,p=>Weight(3));
                mesh.Ribbon(P(sign*.093f,neck.y+.038f*unit,.08f),P(sign*.068f,neck.y-.135f*unit,.129f),.007f*unit,0,p=>Weight(3));
            }
            // Full-length trousers replace the base model's shorts. Each leg
            // has separate knee weights; the pelvis yoke closes the waist.
            mesh.Tube(48,8,(u,v)=>{
                float t=u*Mathf.PI*2;
                return P(Mathf.Sin(t)*.153f,Mathf.Lerp(hip-.095f*unit,hip+.035f*unit,v),Mathf.Cos(t)*.142f);
            },8,p=>Weight(0));
            for(int side=0;side<2;++side)
            {
                int upper=side==0?9:11, lower=upper+1;
                Vector3 thigh=Local(skeleton[upper]),knee=Local(skeleton[lower]);
                Vector3 ankle=Local(Bone(side==0?HumanBodyBones.LeftFoot:HumanBodyBones.RightFoot));
                mesh.Tube(40,40,(u,v)=>{
                    Vector3 c=v<.51f?Vector3.Lerp(thigh,knee,v/.51f):Vector3.Lerp(knee,ankle,(v-.51f)/.49f);
                    float radius=v<.51f?Mathf.Lerp(.091f,.064f,v/.51f):Mathf.Lerp(.064f,.047f,(v-.51f)/.49f);
                    float t=u*Mathf.PI*2;
                    radius+=.002f*Mathf.Cos(t*6)*Mathf.Sin(v*Mathf.PI);
                    radius+=.003f*Mathf.Cos(v*40)*Mathf.Exp(-Mathf.Pow((v-.53f)/.13f,2));
                    return c+new Vector3(Mathf.Sin(t)*radius*unit,0,Mathf.Cos(t)*radius*unit*1.12f);
                },8,p=>{
                    float t=Mathf.InverseLerp(thigh.y,ankle.y,p.y);
                    float w=Mathf.SmoothStep(0,1,Mathf.InverseLerp(.39f,.64f,t));
                    return new BoneWeight{boneIndex0=upper,boneIndex1=lower,weight0=1-w,weight1=w};
                });
            }
            // Geometric shirt stripe and the triangular pendant in the reference.
            void ShirtStripe(Vector2 start,Vector2 end,float width,int material)
            {
                Vector2 normal=new Vector2(-(end-start).y,(end-start).x).normalized*width*.5f;
                mesh.Tube(4,32,(u,v)=>{
                    Vector2 p=Vector2.Lerp(start,end,v)+normal*(u*2-1);
                    float z=.126f*Mathf.Sqrt(Mathf.Max(0,1-Mathf.Pow(p.x/.162f,2)));
                    return P(p.x,p.y,z+.0015f);
                },material,p=>Torso(p.y));
            }
            ShirtStripe(new Vector2(-.115f,hip+.03f*unit),new Vector2(.11f,chest.y+.01f*unit),.026f,0);
            ShirtStripe(new Vector2(.11f,hip+.025f*unit),new Vector2(-.10f,chest.y-.07f*unit),.020f,4);
            Vector3 pendant=P(0,neck.y-.25f*unit,.132f);
            for(int side=0;side<2;++side)
            {
                var start=P(side==0?-.058f:.058f,neck.y-.095f*unit,.108f);
                var control=(start+pendant)*.5f+Vector3.forward*.035f*unit;
                var normal=Vector3.Cross(pendant-start,Vector3.forward).normalized*.0011f*unit;
                mesh.Tube(2,24,(u,v)=>(1-v)*(1-v)*start+2*(1-v)*v*control+v*v*pendant+normal*(u*2-1),4,p=>Torso(p.y));
            }
            Vector3 t0=pendant+new Vector3(0,-.009f*unit,.003f*unit),t1=t0+new Vector3(-.016f,-.028f,0)*unit,t2=t0+new Vector3(.016f,-.028f,0)*unit;
            mesh.Ribbon(t0,t1,.003f*unit,4,p=>Torso(p.y));mesh.Ribbon(t1,t2,.003f*unit,4,p=>Torso(p.y));mesh.Ribbon(t2,t0,.003f*unit,4,p=>Torso(p.y));
            // Solid tapered locks add an asymmetric swept silhouette over the
            // retained hair cap. They follow Head rather than the chest rig.
            Vector3 H(float x,float y,float z)=>head+new Vector3(x,y,z)*unit;
            // Preserve the textured fringe; add only the longer lower layers.
            for(int side=0;side<2;++side)
            {
                float s=side==0?1:-1;
                mesh.Lock(H(s*.103f,.025f,-.035f),H(s*.119f,-.065f,.018f),H(s*.106f,-.13f,.042f),.018f*unit,6,4);
                mesh.Lock(H(s*.109f,.040f,-.047f),H(s*.139f,-.045f,-.008f),H(s*.139f,-.10f,.040f),.013f*unit,5,4);
            }
            mesh.Finish();
            var clones=new Dictionary<Material,Material>();
            foreach(var renderer in avatar.GetComponentsInChildren<SkinnedMeshRenderer>())
            {
                if(renderer.transform.IsChildOf(root.transform))continue;
                bool removed=false;var original=renderer.sharedMesh;
                if(original==null)continue;
                bool face=renderer.name.Equals("Face",StringComparison.OrdinalIgnoreCase);
                bool hairMesh=renderer.name.ToLowerInvariant().Contains("hair");
                if(face || hairMesh)
                {
                    renderer.sharedMesh=RenMeshSculpt.Create(renderer,space,head,unit,face);
                    removed=true;
                }
                var materials=renderer.sharedMaterials;
                for(int i=0;i<materials.Length;++i)
                {
                    var source=materials[i];if(source==null)continue;
                    if(!clones.TryGetValue(source,out var material)){material=new Material(source);clones.Add(source,material);}
                    materials[i]=material;
                    string name=material.name.ToLowerInvariant();
                    // Remove only the old shirt submesh, leaving fingers, skin,
                    // eye whites and all blend shape vertex indices untouched.
                    if((name.Contains("tops") || name.Contains("bottoms")) && i<original.subMeshCount)
                    {
                        if(!removed){renderer.sharedMesh=UnityEngine.Object.Instantiate(original);removed=true;}
                        renderer.sharedMesh.SetTriangles(Array.Empty<int>(),i);
                    }
                    // The base's exposed knees/calves would poke through a
                    // slim trouser silhouette. Hide only those original skin
                    // triangles, selected in the unposed model's coordinates.
                    if(name.Contains("body") && name.Contains("skin") && i<original.subMeshCount)
                    {
                        if(!removed){renderer.sharedMesh=UnityEngine.Object.Instantiate(original);removed=true;}
                        var indices=original.GetTriangles(i);var vertices=original.vertices;var kept=new List<int>();
                        for(int t=0;t<indices.Length;t+=3)
                        {
                            bool leg=true;
                            for(int k=0;k<3;++k)
                            {
                                Vector3 p=space.InverseTransformPoint(renderer.transform.TransformPoint(vertices[indices[t+k]]));
                                leg &= p.y<hip-.07f*unit && Mathf.Abs(p.x-center)<.26f*unit;
                            }
                            if(!leg)kept.AddRange(new[]{indices[t],indices[t+1],indices[t+2]});
                        }
                        renderer.sharedMesh.SetTriangles(kept,i);
                    }
                    if(name.Contains("hair"))Tint(material,new Color(.32f,.29f,.33f));
                    else if(name.Contains("eyeiris"))Tint(material,new Color(.31f,.65f,1f));
                    else if(name.Contains("facebrow"))Tint(material,new Color(.16f,.14f,.18f));
                    else if(name.Contains("bottom"))Tint(material,new Color(.25f,.27f,.32f));
                    else if(name.Contains("skin"))
                    {
                        var texture=material.mainTexture;
                        material.shader=Shader.Find("Cortex/Ren Toon");material.mainTexture=texture;
                        material.SetFloat("_OutlineWidth",.00025f*unit);material.SetFloat("_Gloss",.015f);
                        material.SetFloat("_CullMode",2);
                        Tint(material,new Color(1.05f,1f,1f));
                    }
                    else if(name.Contains("shoe"))Tint(material,new Color(.13f,.14f,.17f));
                }
                renderer.sharedMaterials=materials;
                if(face)MouthSurface.Refine(renderer);
            }
            return root;
        }
        private static void Tint(Material m,Color color)
        {
            if(m.HasProperty("_Color"))m.SetColor("_Color",color);
            if(m.HasProperty("_BaseColor"))m.SetColor("_BaseColor",color);
            if(m.HasProperty("_ShadeColor"))m.SetColor("_ShadeColor",color*.72f);
        }
        private static Material Material(string name,Color color)
        {
            var shader=Shader.Find("Cortex/Ren Toon");
            if(shader==null)throw new InvalidOperationException("Ren Toon shader missing from player");
            return new Material(shader){name=name,color=color};
        }
        private static BoneWeight Weight(int bone)=>new BoneWeight{boneIndex0=bone,weight0=1};
        private sealed class Garment
        {
            readonly Transform root;readonly Transform[] bones;readonly Material[] materials;
            readonly List<Vector3> vertices=new List<Vector3>();readonly List<BoneWeight> weights=new List<BoneWeight>();
            readonly List<Vector2> uvs=new List<Vector2>();
            readonly List<int>[] triangles;
            public Garment(Transform root,Transform[] bones,Material[] materials)
            {this.root=root;this.bones=bones;this.materials=materials;triangles=new List<int>[materials.Length];for(int i=0;i<triangles.Length;++i)triangles[i]=new List<int>();}
            public void Tube(int columns,int rows,Func<float,float,Vector3> point,int material,Func<Vector3,BoneWeight> weight)
            {
                int start=vertices.Count;
                for(int y=0;y<=rows;++y)for(int x=0;x<=columns;++x){var p=point(x/(float)columns,y/(float)rows);vertices.Add(p);weights.Add(weight(p));uvs.Add(new Vector2(x/(float)columns,y/(float)rows));}
                for(int y=0;y<rows;++y)for(int x=0;x<columns;++x)
                {int a=start+y*(columns+1)+x,b=a+1,c=a+columns+1,d=c+1;triangles[material].AddRange(new[]{a,c,b,b,c,d});}
            }
            public void Ribbon(Vector3 a,Vector3 b,float width,int material,Func<Vector3,BoneWeight> weight)
            {
                Vector3 perpendicular=Vector3.Cross(b-a,Vector3.forward).normalized*width*.5f;
                Tube(1,8,(u,v)=>Vector3.Lerp(a,b,v)+perpendicular*(u*2-1),material,weight);
            }
            public void Lock(Vector3 a,Vector3 b,Vector3 tip,float width,int material,int bone)
            {
                Tube(12,20,(u,v)=>{
                    Vector3 c=(1-v)*(1-v)*a+2*(1-v)*v*b+v*v*tip;
                    Vector3 tangent=(2*(1-v)*(b-a)+2*v*(tip-b)).normalized;
                    Vector3 side=Vector3.Cross(tangent,Vector3.forward).normalized;
                    float radius=width*Mathf.Sin(v*Mathf.PI)*(1-v*.55f);float theta=u*Mathf.PI*2;
                    return c+side*Mathf.Cos(theta)*radius+Vector3.forward*Mathf.Sin(theta)*radius*.30f;
                },material,p=>Weight(bone));
            }
            public void Finish()
            {
                var mesh=new Mesh{name="Ren skinned coat collar and hair"};mesh.indexFormat=UnityEngine.Rendering.IndexFormat.UInt32;mesh.SetVertices(vertices);mesh.SetUVs(0,uvs);mesh.boneWeights=weights.ToArray();
                mesh.subMeshCount=materials.Length;for(int i=0;i<materials.Length;++i)mesh.SetTriangles(triangles[i],i);
                var bind=new Matrix4x4[bones.Length];for(int i=0;i<bones.Length;++i)bind[i]=bones[i].worldToLocalMatrix*root.localToWorldMatrix;
                mesh.bindposes=bind;mesh.RecalculateNormals();mesh.RecalculateBounds();
                var renderer=root.gameObject.AddComponent<SkinnedMeshRenderer>();renderer.sharedMesh=mesh;renderer.bones=bones;renderer.rootBone=bones[0];renderer.sharedMaterials=materials;renderer.updateWhenOffscreen=true;
            }
        }
    }
}
