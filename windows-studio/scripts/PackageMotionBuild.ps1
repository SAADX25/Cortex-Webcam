$ErrorActionPreference='Stop'
$studio=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$workspace=(Resolve-Path -LiteralPath (Join-Path $studio '..')).Path
$package=Join-Path $workspace 'dist/CortexMotion-x64'
$release=Join-Path $studio 'build/Release'
New-Item -ItemType Directory -Path $package -Force|Out-Null
Copy-Item -LiteralPath (Join-Path $release 'cortex-motion.exe') -Destination $package -Force
foreach($name in @('avcodec-63.dll','avformat-63.dll','avutil-61.dll','swresample-7.dll','onnxruntime.dll','onnxruntime_providers_shared.dll','DirectML.dll')){
    Copy-Item -LiteralPath (Join-Path $release $name) -Destination $package -Force
}
$desktop=Join-Path $workspace 'dist/CortexStudio-Desktop-x64'
Get-ChildItem -LiteralPath $desktop -File -Filter '*.dll'|Where-Object Name -Match '^(msvcp|vcruntime|concrt)'|Copy-Item -Destination $package -Force
New-Item -ItemType Directory -Path (Join-Path $package 'models') -Force|Out-Null
foreach($models in @('face-tracking','upper-body','hands')){
    Copy-Item -LiteralPath (Join-Path $studio ('.deps/'+$models)) -Destination (Join-Path $package 'models') -Recurse -Force
}
Copy-Item -LiteralPath (Join-Path $desktop 'licenses') -Destination $package -Recurse -Force
Copy-Item -LiteralPath (Join-Path $studio '.deps/hands/LICENSE') -Destination (Join-Path $package 'licenses/MediaPipeHands-Apache-2.0.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspace 'vrm-viewer/README.md') -Destination (Join-Path $package 'README.md') -Force
@('@echo off','cd /d "%~dp0"','cortex-motion.exe --diagnostics','pause')|Set-Content (Join-Path $package 'Start-Hand-Diagnostics.cmd') -Encoding ascii
Get-ChildItem -LiteralPath $package -File -Recurse|Where-Object Name -ne 'SHA256SUMS.txt'|ForEach-Object{
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant(),$_.FullName.Substring($package.Length+1)
}|Set-Content (Join-Path $package 'SHA256SUMS.txt') -Encoding utf8
Write-Output ('Motion backend packaged: '+$package)
