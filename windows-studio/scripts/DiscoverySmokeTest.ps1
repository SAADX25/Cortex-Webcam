param([string]$ReceiverExe="$PSScriptRoot\..\build\Debug\cortex-receive.exe",[string]$FFmpegExe='')
$ErrorActionPreference='Stop'
$directory=Join-Path $PSScriptRoot '..\build\discovery-smoke'
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$process=$null
$client=$null
$query=$null
$sender=$null
try {
    $process=Start-Process -FilePath $ReceiverExe -ArgumentList @('55004','""','15','55005') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $directory 'receiver.log') -RedirectStandardError (Join-Path $directory 'receiver.err')
    $client=[System.Net.Sockets.UdpClient]::new()
    $client.Connect('127.0.0.1',55005)
    $client.Client.ReceiveTimeout=400
    $nonce='0123456789abcdef0123456789abcdef'
    $hello=[Text.Encoding]::ASCII.GetBytes("CORTEX/1 HELLO $nonce H264 1920 1080 30`n")
    $reply=''
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    while([DateTime]::UtcNow -lt $deadline) {
        try {
            [void]$client.Send($hello,$hello.Length)
            $remote=[System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any,0)
            $reply=[Text.Encoding]::ASCII.GetString($client.Receive([ref]$remote)).Trim()
            if($reply -eq "CORTEX/1 READY $nonce 55004 H264 1920 1080 30"){break}
        } catch [System.Net.Sockets.SocketException] { }
        Start-Sleep -Milliseconds 100
    }
    if($reply -ne "CORTEX/1 READY $nonce 55004 H264 1920 1080 30"){throw "Readiness handshake failed: $reply"}
    Write-Output 'PASS: UDP handshake returned matching nonce, codec, geometry, frame rate and video port.'
    $other=[System.Net.Sockets.UdpClient]::new([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Parse('127.0.0.2'),0))
    try {
        $other.Connect('127.0.0.1',55005);$other.Client.ReceiveTimeout=1000
        [void]$other.Send($hello,$hello.Length)
        $remote=[System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any,0)
        $busy=[Text.Encoding]::ASCII.GetString($other.Receive([ref]$remote)).Trim()
        if($busy -ne "CORTEX/1 BUSY $nonce"){throw "Second phone was not rejected: $busy"}
        Write-Output 'PASS: another peer receives BUSY while the lease is active.'
    } finally {$other.Dispose()}
    # Legacy unicast-response mDNS PTR query to the normal multicast group.
    $bytes=[System.Collections.Generic.List[byte]]::new()
    $bytes.AddRange([byte[]]@(0,0,0,0,0,1,0,0,0,0,0,0))
    foreach($label in @('_cortexcam','_udp','local')) {
        $encoded=[Text.Encoding]::ASCII.GetBytes($label);$bytes.Add([byte]$encoded.Length);$bytes.AddRange($encoded)
    }
    $bytes.AddRange([byte[]]@(0,0,12,128,1))
    $query=[System.Net.Sockets.UdpClient]::new()
    $query.Client.ReceiveTimeout=1000
    $found=$false
    for($attempt=0;$attempt -lt 4 -and !$found;$attempt++) {
        $packet=$bytes.ToArray()
        [void]$query.Send($packet,$packet.Length,'224.0.0.251',5353)
        try {
            $remote=[System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any,0)
            $response=$query.Receive([ref]$remote)
            $text=[Text.Encoding]::ASCII.GetString($response)
            if($response.Length -gt 12 -and ($response[2] -band 128) -and $text.Contains('Cortex-')){$found=$true}
        } catch [System.Net.Sockets.SocketException] { }
    }
    if(!$found){throw 'No mDNS PTR reply observed on this interface; inspect firewall/multicast policy.'}
    Write-Output 'PASS: native mDNS broadcaster answered a multicast service query.'
    if($FFmpegExe) {
        $sender=Start-Process -FilePath $FFmpegExe -ArgumentList @('-hide_banner','-loglevel','warning','-re','-f','lavfi','-i','testsrc2=size=1920x1080:rate=30','-t','4','-an','-c:v','libx264','-profile:v','baseline','-preset','ultrafast','-tune','zerolatency','-bf','0','-g','30','-pix_fmt','yuv420p','-b:v','8M','-payload_type','96','-f','rtp','rtp://127.0.0.1:55004?pkt_size=1200') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $directory 'sender.log') -RedirectStandardError (Join-Path $directory 'sender.err')
        if(!$sender.WaitForExit(10000) -or $sender.ExitCode -ne 0){throw 'Synthetic video sender failed'}
    }
    if(!$process.WaitForExit(20000)){throw 'Receiver shutdown timed out'}
    if($process.ExitCode -ne 0){throw 'Receiver failed; inspect discovery-smoke logs'}
    if($FFmpegExe) {
        $log=Get-Content -LiteralPath (Join-Path $directory 'receiver.log') -Raw
        $frames=([regex]::Matches($log,'frames=(\d+)') | ForEach-Object {[int]$_.Groups[1].Value} | Measure-Object -Maximum).Maximum
        if($frames -lt 60 -or $log -match 'decodeErrors=[1-9]'){throw "Handshake-to-GPU-video failed: $frames frames"}
        Write-Output "PASS: after handshake, $frames frames decoded to GPU NV12 without decode errors."
    }
} finally {
    if($null -ne $client){$client.Dispose()}
    if($null -ne $query){$query.Dispose()}
    if($null -ne $process -and !$process.HasExited){Stop-Process -Id $process.Id}
    if($null -ne $sender -and !$sender.HasExited){Stop-Process -Id $sender.Id}
}
