using System.Collections.Generic;
using UnityEngine;
using UniVRM10;

namespace Cortex.Vrm
{
    // Style switching restores the imported meshes/materials exactly. Custom
    // avatars stay in their own appearance unless Ren is explicitly selected.
    public sealed class AvatarAppearanceToggle
    {
        private sealed class State
        {
            public SkinnedMeshRenderer Renderer;
            public Mesh OriginalMesh, StyledMesh;
            public Material[] OriginalMaterials, StyledMaterials;
        }
        private readonly List<State> states=new List<State>();
        private readonly GameObject outfit;
        public bool Enabled {get;private set;}
        public AvatarAppearanceToggle(Vrm10Instance avatar)
        {
            foreach(var renderer in avatar.GetComponentsInChildren<SkinnedMeshRenderer>())
                states.Add(new State{Renderer=renderer,OriginalMesh=renderer.sharedMesh,OriginalMaterials=renderer.sharedMaterials});
            outfit=RenAppearance.Apply(avatar);
            foreach(var s in states){s.StyledMesh=s.Renderer.sharedMesh;s.StyledMaterials=s.Renderer.sharedMaterials;}
            Enabled=true;
        }
        public void SetEnabled(bool enabled)
        {
            if(enabled==Enabled)return;
            Enabled=enabled;outfit.SetActive(enabled);
            foreach(var s in states)
            {
                s.Renderer.sharedMesh=enabled?s.StyledMesh:s.OriginalMesh;
                s.Renderer.sharedMaterials=enabled?s.StyledMaterials:s.OriginalMaterials;
            }
        }
    }
}
