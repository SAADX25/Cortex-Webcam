using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
public static class CortexBuild
{
    public static void Windows()
    {
        var scene=EditorSceneManager.NewScene(NewSceneSetup.EmptyScene,NewSceneMode.Single);
        // Runtime-imported VRMs have no authored scene material references.
        // Keep the importer shaders and their variants in the standalone player.
        var settings = new SerializedObject(AssetDatabase.LoadAllAssetsAtPath("ProjectSettings/GraphicsSettings.asset")[0]);
        var included = settings.FindProperty("m_AlwaysIncludedShaders");
        foreach(var path in new[]{"Packages/com.vrmc.vrm/MToon10/Shaders/vrmc_materials_mtoon.shader","Packages/com.vrmc.gltf/UniUnlit/Shaders/UniUnlit.shader"})
        {
            var shader=AssetDatabase.LoadAssetAtPath<Shader>(path);
            if(shader==null)throw new System.Exception("Runtime VRM shader is missing: "+path);
            bool exists=false;
            for(int i=0;i<included.arraySize;++i)if(included.GetArrayElementAtIndex(i).objectReferenceValue==shader)exists=true;
            if(!exists){included.InsertArrayElementAtIndex(included.arraySize);included.GetArrayElementAtIndex(included.arraySize-1).objectReferenceValue=shader;}
        }
        settings.ApplyModifiedPropertiesWithoutUndo(); AssetDatabase.SaveAssets();
        EditorSceneManager.SaveScene(scene,"Assets/CortexViewer.unity");
        var output=System.IO.Path.GetFullPath(System.IO.Path.Combine(Application.dataPath,"../../dist/CortexVRM-Viewer/CortexVRM.exe"));
        var report=BuildPipeline.BuildPlayer(new[]{"Assets/CortexViewer.unity"},output,BuildTarget.StandaloneWindows64,BuildOptions.None);
        if(report.summary.result!=UnityEditor.Build.Reporting.BuildResult.Succeeded)throw new System.Exception("VRM viewer build failed");
    }
}
