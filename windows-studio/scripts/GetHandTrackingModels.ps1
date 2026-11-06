$ErrorActionPreference='Stop'
$destination=Join-Path $PSScriptRoot '..\.deps\hands'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$commit='48b10fa505aeb51fbd127fc60a5c660040e823b7'
$files=@(
    @{Name='palm_detection_mediapipe_2023feb.onnx'; Folder='palm_detection_mediapipe'; Hash='78ff51c38496b7fc8b8ebdb6cc8c1abb02fa6c38427c6848254cdaba57fcce7c'},
    @{Name='handpose_estimation_mediapipe_2023feb.onnx'; Folder='handpose_estimation_mediapipe'; Hash='db0898ae717b76b075d9bf563af315b29562e11f8df5027a1ef07b02bef6d81c'}
)
foreach($file in $files){
    $path=Join-Path $destination $file.Name
    if(!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -ne $file.Hash){
        Invoke-WebRequest "https://media.githubusercontent.com/media/opencv/opencv_zoo/$commit/models/$($file.Folder)/$($file.Name)" -OutFile $path
    }
    if((Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -ne $file.Hash){throw "Hand model checksum mismatch: $($file.Name)"}
}
Invoke-WebRequest "https://raw.githubusercontent.com/opencv/opencv_zoo/$commit/models/handpose_estimation_mediapipe/LICENSE" -OutFile (Join-Path $destination 'LICENSE')
@{repository='https://github.com/opencv/opencv_zoo';commit=$commit;files=$files}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $destination 'SOURCE.json') -Encoding utf8
Write-Output 'MediaPipe palm and 21-joint hand models verified.'
