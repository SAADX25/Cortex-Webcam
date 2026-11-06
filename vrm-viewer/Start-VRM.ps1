param([string]$ModelPath)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$viewer=Join-Path $workspace 'dist/CortexVRM-Viewer/CortexVRM.exe'
$backend=Join-Path $workspace 'dist/CortexMotion-x64/cortex-motion.exe'
if(!(Test-Path -LiteralPath $backend)){$backend=Join-Path $workspace 'windows-studio/build/Release/cortex-motion.exe'}
if(!(Test-Path -LiteralPath $viewer)){throw 'Build the Unity viewer with Build-Viewer.ps1 first.'}
if(!(Test-Path -LiteralPath $backend)){throw 'Build the Windows Release targets first.'}
if(Get-Process CortexStudio -ErrorAction SilentlyContinue){throw 'Close Cortex Studio first: it owns the same phone ports.'}
$log=Join-Path $workspace 'windows-studio/build/vrm-motion.log'
$motion=Start-Process -FilePath $backend -WorkingDirectory (Split-Path $backend) -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError ($log+'.error') -PassThru
try{
    $arguments=if($ModelPath){@('--vrm',('"'+(Resolve-Path -LiteralPath $ModelPath).Path+'"'))}else{@()}
    $player=Start-Process -FilePath $viewer -WorkingDirectory (Split-Path $viewer) -ArgumentList $arguments -PassThru
    $player.WaitForExit()
}finally{if(!$motion.HasExited){Stop-Process -Id $motion.Id}}
