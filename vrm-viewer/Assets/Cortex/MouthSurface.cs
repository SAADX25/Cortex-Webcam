using System;
using System.Collections.Generic;
using UnityEngine;
using UniVRM10;

namespace Cortex.Vrm
{
    public static class MouthSurface
    {
        // Strip only enamel triangles from the known FaceMouth atlas. Keep all
        // vertices/morph indices so the authored lip and jaw channels still work.
        private static int Strip(Mesh mesh,int mouth)
        {
            var uv=mesh.uv;var teeth=new List<int>();var tissue=new List<int>();
            var indices=mesh.GetTriangles(mouth);
            for(int t=0;t<indices.Length;t+=3)
            {
                bool enamel=true;
                for(int k=0;k<3;++k)
                {
                    var p=uv[indices[t+k]];
                    enamel &= p.x>=0 && p.x<.5f && p.y>.5f && p.y<=1;
                }
                (enamel?teeth:tissue).AddRange(new[]{indices[t],indices[t+1],indices[t+2]});
            }
            mesh.SetTriangles(tissue,mouth);return teeth.Count/3;
        }
        public static void RemoveTeeth(Vrm10Instance avatar)
        {
            foreach(var renderer in avatar.GetComponentsInChildren<SkinnedMeshRenderer>())
            {
                var materials=renderer.sharedMaterials;
                int mouth=Array.FindIndex(materials,m=>m!=null && m.name.IndexOf("FaceMouth",StringComparison.OrdinalIgnoreCase)>=0);
                if(renderer.sharedMesh==null || mouth<0 || mouth>=renderer.sharedMesh.subMeshCount)continue;
                var owned=UnityEngine.Object.Instantiate(renderer.sharedMesh);owned.name+=" without teeth";
                Strip(owned,mouth);renderer.sharedMesh=owned;
            }
        }
        public static int Refine(SkinnedMeshRenderer renderer)
        {
            var materials=renderer.sharedMaterials;var mesh=renderer.sharedMesh;
            if(mesh==null)return 0;
            int mouth=Array.FindIndex(materials,m=>m!=null && m.name.IndexOf("FaceMouth",StringComparison.OrdinalIgnoreCase)>=0);
            if(mouth<0 || mouth>=mesh.subMeshCount)return 0;
            int removed=Strip(mesh,mouth);
            var shader=Shader.Find("Cortex/Ren Toon");
            var oral=new Material(shader){name="Ren FaceMouth interior",mainTexture=materials[mouth].mainTexture,color=new Color(.58f,.23f,.28f)};
            oral.SetFloat("_Gloss",0);oral.SetFloat("_OutlineWidth",0);oral.SetFloat("_CullMode",2);
            materials[mouth]=oral;
            renderer.sharedMaterials=materials;
            return removed;
        }
    }
}
