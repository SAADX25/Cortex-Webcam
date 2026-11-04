$ErrorActionPreference = 'Stop'
$destination = Join-Path $PSScriptRoot '..\.deps\upper-body'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$commit = '48b10fa505aeb51fbd127fc60a5c660040e823b7'
$files = @(
    @{ Name='person_detection_mediapipe_2023mar.onnx'; Source='models/person_detection_mediapipe/person_detection_mediapipe_2023mar.onnx'; Hash='47fd5599d6fa17608f03e0eb0ae230baa6e597d7e8a2c8199fe00abea55a701f' },
    @{ Name='pose_estimation_mediapipe_2023mar.onnx'; Source='models/pose_estimation_mediapipe/pose_estimation_mediapipe_2023mar.onnx'; Hash='9d89c599319a18fb7d2e28451a883476164543182bafca5f09eb2cf767ed2f3f' }
)
foreach ($file in $files) {
    $path = Join-Path $destination $file.Name
    if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Hash) {
        Invoke-WebRequest -Uri "https://media.githubusercontent.com/media/opencv/opencv_zoo/$commit/$($file.Source)" -OutFile $path
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Hash) { throw "Body model checksum mismatch: $($file.Name)" }
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\third_party\MediaPipePose\LICENSE') -Destination (Join-Path $destination 'LICENSE') -Force
@{ repository='https://github.com/opencv/opencv_zoo'; commit=$commit; files=$files } | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $destination 'SOURCE.json') -Encoding utf8
Write-Output 'MediaPipe upper-body models verified.'
