param([string]$UnityEditor)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if(!$UnityEditor){
    $candidates=@(Join-Path $workspace 'windows-studio/.deps/unity/6000.6.8f1/Editor/Unity.exe')
    $candidates+=@(Get-ChildItem 'C:/Program Files/Unity/Hub/Editor/*/Editor/Unity.exe' -ErrorAction SilentlyContinue|Sort-Object FullName -Descending|Select-Object -ExpandProperty FullName)
    $UnityEditor=$candidates|Where-Object {Test-Path -LiteralPath $_}|Select-Object -First 1
}
if(!$UnityEditor){throw 'Unity Editor is not installed. Pass -UnityEditor with the path to an activated Editor.'}
$log=Join-Path $workspace 'windows-studio/build/vrm-unity-build.log'
$editorProcess=Start-Process -FilePath $UnityEditor -ArgumentList @('-batchmode','-quit','-projectPath',('"'+$PSScriptRoot+'"'),'-executeMethod','CortexBuild.Windows','-logFile',('"'+$log+'"')) -WindowStyle Hidden -PassThru -Wait
if($editorProcess.ExitCode){throw 'Unity build failed. See windows-studio/build/vrm-unity-build.log; a Unity license may need activation in Unity Hub.'}
if(!(Test-Path -LiteralPath (Join-Path $workspace 'dist/CortexVRM-Viewer/CortexVRM.exe'))){throw 'Unity exited without producing the VRM player. Inspect vrm-unity-build.log.'}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Start-3D-Avatar.cmd') -Destination (Join-Path $workspace 'dist/Start-3D-Avatar.cmd') -Force
