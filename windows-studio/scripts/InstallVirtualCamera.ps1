#Requires -RunAsAdministrator
param([switch]$Uninstall)
$ErrorActionPreference='Stop'
if(![Environment]::Is64BitProcess){throw 'Run this script from 64-bit PowerShell as Administrator.'}
$folder=Join-Path $PSScriptRoot '..\third_party\unitycapture\Install'
$files=@(
    @{Name='UnityCaptureFilter64.dll';Hash='72812f5363d8ecb45632253f8c8c888844b1b62e27616f3c8cc21064ccde25e5';Exe="$env:SystemRoot\System32\regsvr32.exe"},
    @{Name='UnityCaptureFilter32.dll';Hash='aa3ebdf03dea7f3aab3dd7b724751f49ed71672256b57c6a19aa6809cabf30ba';Exe="$env:SystemRoot\SysWOW64\regsvr32.exe"}
)
foreach($item in $files){
    $dll=(Resolve-Path -LiteralPath (Join-Path $folder $item.Name)).Path
    if((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.Hash){throw "Unexpected filter binary hash: $dll"}
    if($Uninstall){& $item.Exe /s /u $dll}else{& $item.Exe /s $dll}
    if($LASTEXITCODE -ne 0){throw "Filter registration failed: $($item.Name), code $LASTEXITCODE"}
}
if($Uninstall){Write-Output 'UnityCapture filters unregistered.'}else{Write-Output 'Unity Video Capture registered for 32-bit and 64-bit webcam clients. Restart the receiving app to refresh its device list.'}
