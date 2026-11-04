param([string]$CubismSdkRoot)
$ErrorActionPreference='Stop'
$root=Join-Path $PSScriptRoot '..\.deps\onnx'
New-Item -ItemType Directory -Path $root -Force | Out-Null
function Fetch([string]$Url,[string]$Name,[string]$Hash){
    $path=Join-Path $root $Name
    if(!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Hash){
        Invoke-WebRequest -Uri $Url -OutFile $path
    }
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Hash){throw "Checksum mismatch: $Name"}
    return $path
}
$ort=Fetch 'https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg' 'ort.zip' '57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681'
$dml=Fetch 'https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg' 'directml.zip' '4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9'
$model=Fetch 'https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx' 'rvm_mobilenetv3_fp32.onnx' '88d4531297118f595bf2fd60f6f566aec2e559393802d1f436c380f0cbbd2828'
Expand-Archive -LiteralPath $ort -DestinationPath (Join-Path $root 'runtime') -Force
Expand-Archive -LiteralPath $dml -DestinationPath (Join-Path $root 'directml') -Force
Write-Output "Dependencies ready. Configure CMake with ONNXRUNTIME_ROOT=$root/runtime and DIRECTML_ROOT=$root/directml. Model: $model"

if (!$CubismSdkRoot) {
    $localSdk = Join-Path $PSScriptRoot '..\.deps\cubism-sdk'
    if (Test-Path -LiteralPath $localSdk) { $CubismSdkRoot = $localSdk }
}
if ($CubismSdkRoot) {
    $sdk = (Resolve-Path -LiteralPath $CubismSdkRoot).Path
    foreach ($required in 'Core/include/Live2DCubismCore.h','Framework/CMakeLists.txt','Framework/src/Rendering/D3D11/Shaders') {
        if (!(Test-Path -LiteralPath (Join-Path $sdk $required))) { throw "Incomplete Cubism SDK: missing $required" }
    }
    Write-Output "Cubism SDK located. Configure with -DCUBISM_SDK_ROOT=`"$sdk`"."
}

& (Join-Path $PSScriptRoot "GetFaceTrackingModels.ps1")

& (Join-Path $PSScriptRoot "GetBodyTrackingModels.ps1")
