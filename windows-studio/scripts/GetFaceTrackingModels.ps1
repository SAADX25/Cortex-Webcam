$ErrorActionPreference = 'Stop'
$destination = Join-Path $PSScriptRoot '..\.deps\face-tracking'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$commit = '50c09741897010faad721a218b2bbe14ecad6251'
$files = @(
    @{ Name='lm_model1_opt.onnx'; Source='models/lm_model1_opt.onnx'; Hash='5bec42b298a24142cdb249a7256d65bc3fc0fbc673fa1752a64f4d7164719c9f' },
    @{ Name='mnv3_detection_opt.onnx'; Source='models/mnv3_detection_opt.onnx'; Hash='0e8e4806766d85ab067a52c7af0dcb59eb7f9dfe580b44f20a8e6ab712d89809' },
    @{ Name='LICENSE'; Source='LICENSE'; Hash='28612834d7ca038a9009550e3869a67e6be3a87c238d997f58c0907e08744146' }
)
foreach ($file in $files) {
    $path = Join-Path $destination $file.Name
    if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Hash) {
        Invoke-WebRequest -Uri "https://raw.githubusercontent.com/emilianavt/OpenSeeFace/$commit/$($file.Source)" -OutFile $path
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Hash) { throw "Face model checksum mismatch: $($file.Name)" }
}
@{ repository='https://github.com/emilianavt/OpenSeeFace'; commit=$commit; files=$files } | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $destination 'SOURCE.json') -Encoding utf8
Write-Output 'Face detection and 66-landmark models verified (OpenSeeFace, BSD-2-Clause).'
