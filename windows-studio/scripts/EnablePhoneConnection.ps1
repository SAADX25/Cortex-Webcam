param(
    [Parameter(Mandatory=$true)][string]$Program,
    [ValidateRange(1,65535)][int]$VideoPort=5004,
    [ValidateRange(1,65535)][int]$ControlPort=5005
)
$ErrorActionPreference='Stop'
$resolved=(Resolve-Path -LiteralPath $Program).Path
if([IO.Path]::GetFileName($resolved) -ne 'CortexStudio.exe'){throw 'Expected CortexStudio.exe'}
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if(-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'Administrator permission is required to allow the local phone connection.'
}
# Keep the exception tied to this executable, these UDP ports, and the local subnet.
# Public is included because Windows often classifies a home Ethernet network as Public.
$sha=[Security.Cryptography.SHA256]::Create()
try{$hash=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($resolved.ToLowerInvariant()))).Replace('-','')}finally{$sha.Dispose()}
$ruleName='CortexStudio-LAN-'+$hash.Substring(0,12)
Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -Name $ruleName -DisplayName 'Cortex Studio — phone on local network' -Description 'Phone video and discovery. Local subnet only; no internet-wide access.' -Direction Inbound -Action Allow -Enabled True -Profile Any -Program $resolved -Protocol UDP -LocalPort $VideoPort,$ControlPort -RemoteAddress LocalSubnet | Out-Null
'Local phone connection is enabled.'
