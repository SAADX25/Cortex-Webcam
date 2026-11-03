param(
    [Parameter(Mandatory=$true)][string]$FFmpegExe,
    [string]$ReceiverExe = "$PSScriptRoot\..\build\Debug\cortex-receive.exe",
    [int]$Port = 55004,
    [ValidateSet(30,60)][int]$Fps = 30
)
$ErrorActionPreference = 'Stop'
$output = Join-Path $PSScriptRoot '..\build\smoke'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$receiverOut = Join-Path $output "receiver-$Fps.log"
$receiverErr = Join-Path $output "receiver-$Fps.err"
$senderOut = Join-Path $output "sender-$Fps.log"
$senderErr = Join-Path $output "sender-$Fps.err"
$receiver = $null
$sender = $null
try {
    $receiver = Start-Process -FilePath $ReceiverExe -ArgumentList @("$Port",'127.0.0.1','12') -WindowStyle Hidden -PassThru -RedirectStandardOutput $receiverOut -RedirectStandardError $receiverErr
    Start-Sleep -Seconds 1
    # Software encoding is only a synthetic PC test fixture. The receiver must decode on D3D11VA.
    $arguments = @('-hide_banner','-loglevel','warning','-re','-f','lavfi','-i',"testsrc2=size=1920x1080:rate=$Fps",'-t','8','-an','-c:v','libx264','-profile:v','baseline','-preset','ultrafast','-tune','zerolatency','-bf','0','-g',"$Fps",'-pix_fmt','yuv420p','-b:v','8M','-payload_type','96','-f','rtp',"rtp://127.0.0.1:$Port`?pkt_size=1200")
    $sender = Start-Process -FilePath $FFmpegExe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $senderOut -RedirectStandardError $senderErr
    if (!$sender.WaitForExit(15000)) { throw 'Synthetic sender timed out' }
    if (!$receiver.WaitForExit(15000)) { throw 'Receiver did not shut down' }
    if ($sender.ExitCode -ne 0) { throw "Sender failed: $(Get-Content -LiteralPath $senderErr -Raw)" }
    if ($receiver.ExitCode -ne 0) { throw "Receiver failed: $(Get-Content -LiteralPath $receiverErr -Raw)" }
    $log = Get-Content -LiteralPath $receiverOut -Raw
    $matches = [regex]::Matches($log,'frames=(\d+)')
    $maximum = ($matches | ForEach-Object { [int]$_.Groups[1].Value } | Measure-Object -Maximum).Maximum
    if ($maximum -lt $Fps * 5) { throw "Insufficient GPU frames decoded: $maximum. See $output" }
    if ($log -match 'decodeErrors=[1-9]') { throw "Decode errors in smoke test. See $output" }
    Write-Output "PASS: 1080p/$Fps RTP -> D3D11VA -> GPU NV12, $maximum decoded frames. Logs: $output"
} finally {
    foreach ($process in @($sender,$receiver)) {
        if ($null -ne $process -and !$process.HasExited) { Stop-Process -Id $process.Id }
    }
}
