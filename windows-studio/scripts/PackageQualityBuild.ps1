param(
    [string]$CrtDirectory='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT',
    [switch]$Desktop,
    [string]$PackageName=''
)
$ErrorActionPreference='Stop'
$studio=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$workspace=(Resolve-Path -LiteralPath (Join-Path $studio '..')).Path
$dist=Join-Path $workspace 'dist'
$packageName=if($PackageName){$PackageName}elseif($Desktop){'CortexStudio-Desktop-x64'}else{'CortexStudio-Windows-x64'}
if($packageName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$'){throw 'PackageName must be a simple folder name'}
$package=Join-Path $dist $packageName
$release=Join-Path $studio 'build\Release'
foreach($directory in @($package,"$package\models","$package\assets","$package\scripts","$package\licenses","$package\third_party\unitycapture\Install")){
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
if($Desktop){
    Get-ChildItem -LiteralPath (Join-Path $studio 'build\Desktop') | Copy-Item -Destination $package -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $release 'CortexStudioNative.dll') -Destination $package -Force
    Copy-Item -LiteralPath (Join-Path $release 'cortex-receive.exe') -Destination (Join-Path $package 'CortexReceiver-CLI.exe') -Force
}else{
    Copy-Item -LiteralPath (Join-Path $release 'cortex-receive.exe') -Destination (Join-Path $package 'CortexStudio.exe')
}
# Include only FFmpeg components used by this pipeline and the required GPU AI runtimes.
foreach($name in @('avcodec-63.dll','avformat-63.dll','avutil-61.dll','swresample-7.dll','onnxruntime.dll','onnxruntime_providers_shared.dll','DirectML.dll')){
    Copy-Item -LiteralPath (Join-Path $release $name) -Destination $package
}
Get-ChildItem -LiteralPath $CrtDirectory -Filter '*.dll' -File | Copy-Item -Destination $package
Copy-Item -LiteralPath (Join-Path $studio '.deps\onnx\rvm_mobilenetv3_fp32.onnx') -Destination "$package\models"
Copy-Item -LiteralPath (Join-Path $studio 'scripts\InstallVirtualCamera.ps1') -Destination "$package\scripts"
Copy-Item -LiteralPath (Join-Path $studio 'scripts\EnablePhoneConnection.ps1') -Destination "$package\scripts"
foreach($architecture in @('32','64')){
    Copy-Item -LiteralPath (Join-Path $studio "third_party\unitycapture\Install\UnityCaptureFilter$architecture.dll") -Destination "$package\third_party\unitycapture\Install"
}
$licenseFiles=@{
    'FFmpeg-GPL.txt'='.deps\ffmpeg-9.0.2-full_build-shared\LICENSE'
    'ONNXRuntime-MIT.txt'='.deps\onnx\runtime\LICENSE'
    'ONNXRuntime-ThirdPartyNotices.txt'='.deps\onnx\runtime\ThirdPartyNotices.txt'
    'DirectML-LICENSE.txt'='.deps\onnx\directml\LICENSE.txt'
    'DirectML-LICENSE-CODE.txt'='.deps\onnx\directml\LICENSE-CODE.txt'
    'DirectML-ThirdPartyNotices.txt'='.deps\onnx\directml\ThirdPartyNotices.txt'
    'RVM-GPL.txt'='.deps\onnx\RVM-LICENSE.txt'
    'UnityCapture-LICENSE.txt'='third_party\unitycapture\LICENSE'
    'UnityCapture-Interface-Changes.md'='third_party\unitycapture\README.md'
}
foreach($entry in $licenseFiles.GetEnumerator()){
    Copy-Item -LiteralPath (Join-Path $studio $entry.Value) -Destination (Join-Path "$package\licenses" $entry.Key)
}
@'
Third-party source and package references
FFmpeg 9.0.2 shared build: https://www.gyan.dev/ffmpeg/builds/
FFmpeg source: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
ONNX Runtime DirectML 1.24.4: https://github.com/microsoft/onnxruntime
DirectML 1.15.4: https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4
RVM MobileNetV3 FP32 v1.0.0: https://github.com/PeterL1n/RobustVideoMatting
UnityCapture commit 3ed54c325e0ad71afcf4f246c07e5e17b3d7f2d2: https://github.com/schellingb/UnityCapture
Cortex source is in the adjacent windows-studio and android-sender workspace directories.
'@ | Set-Content -LiteralPath "$package\licenses\SOURCES.txt" -Encoding utf8

$launchers=@{
    'Start-Quality-Test.cmd'='--filter none --fps 30 --model "%~dp0models\rvm_mobilenetv3_fp32.onnx"'
    'Start-Quality-60fps.cmd'='--filter none --fps 60 --model "%~dp0models\rvm_mobilenetv3_fp32.onnx"'
    'Start-Chroma-Key.cmd'='--filter chroma --fps 30 --background-image "%~dp0assets\background.bmp" --model "%~dp0models\rvm_mobilenetv3_fp32.onnx"'
    'Start-AI-Matting.cmd'='--filter ai --fps 30 --background-image "%~dp0assets\background.bmp" --model "%~dp0models\rvm_mobilenetv3_fp32.onnx"'
}
foreach($entry in $launchers.GetEnumerator()){
    $lines=@('@echo off','cd /d "%~dp0"','echo Cortex Studio quality test - press Q to quit.',('"%~dp0CortexStudio.exe" '+$entry.Value),'set "CORTEX_EXIT=%ERRORLEVEL%"','pause','exit /b %CORTEX_EXIT%')
    if($Desktop){$lines=@('@echo off','cd /d "%~dp0"',('start "" "%~dp0CortexStudio.exe" '+$entry.Value),'exit /b 0')}
    Set-Content -LiteralPath (Join-Path $package $entry.Key) -Value $lines -Encoding ascii
}
@('@echo off','cd /d "%~dp0"','powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\InstallVirtualCamera.ps1"','pause') |
    Set-Content -LiteralPath "$package\Install-Virtual-Camera.cmd" -Encoding ascii
@('@echo off','cd /d "%~dp0"','powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\InstallVirtualCamera.ps1" -Uninstall','pause') |
    Set-Content -LiteralPath "$package\Uninstall-Virtual-Camera.cmd" -Encoding ascii

# One-time static BMP asset; live video color conversion stays on GPU.
$width=1280;$height=720;$size=$width*$height*3
$stream=[IO.File]::Create("$package\assets\background.bmp")
$writer=[IO.BinaryWriter]::new($stream)
try{
    $writer.Write([byte[]]@(66,77));$writer.Write([int]($size+54));$writer.Write([int]0);$writer.Write([int]54)
    $writer.Write([int]40);$writer.Write([int]$width);$writer.Write([int]$height);$writer.Write([int16]1);$writer.Write([int16]24)
    $writer.Write([int]0);$writer.Write([int]$size);$writer.Write([int]0);$writer.Write([int]0);$writer.Write([int]0);$writer.Write([int]0)
    for($y=0;$y -lt $height;$y++){
        $row=[byte[]]::new($width*3)
        for($x=0;$x -lt $width;$x++){$row[$x*3]=[byte](64+96*$y/$height);$row[$x*3+1]=[byte](24+40*$y/$height);$row[$x*3+2]=[byte](16+10*$y/$height)}
        $writer.Write($row)
    }
}finally{$writer.Dispose()}

@'
Cortex Webcam — نسخة اختبار الجودة

ويندوز 10/11 إصدار 64-bit، وأندرويد 10 أو أحدث.

1) فك ضغط حزمة ويندوز بالكامل في مجلد ثابت. لا تنقل EXE وحده؛ ملفات DLL بجانبه ضرورية.
2) أول مرة فقط: اضغط بزر الفأرة الأيمن على Install-Virtual-Camera.cmd واختر تشغيل كمسؤول.
   هذه الخطوة تسجّل كاميرا Unity Video Capture لتظهر في برامج الفيديو.
3) شغّل Start-Quality-Test.cmd لاختبار الصورة الأصلية بدون إزالة خلفية، على 1080p/30.
4) ثبّت CortexSender-Android.apk على الهاتف وافتحه واسمح باستخدام الكاميرا.
5) اجعل الهاتف والكمبيوتر على الشبكة المحلية نفسها. يمكن للكمبيوتر الاتصال بالكابل بالراوتر نفسه.
   Auto-connect مفعّل افتراضيًا؛ سيكتشف الهاتف الكمبيوتر ويبدأ الإرسال.
6) افتح OBS أو برنامج كاميرا يدعم الجهاز، واختر Unity Video Capture بدقة 1920×1080.
   أغلق البرنامج وافتحه من جديد إذا لم تتحدث قائمة الكاميرات بعد التثبيت.

نافذة برنامج ويندوز الحالية تعرض عدادات؛ ليست واجهة معاينة رسومية. الصورة تظهر في برنامج الفيديو الذي تختاره.

اختبارات إضافية:
- Start-Quality-60fps.cmd: إخراج 60fps. في الهاتف اضغط Stop، فعّل 60fps ثم أعد Auto-connect.
  يجب أن يدعم الهاتف 1080p/60؛ إذا ظهر خطأ ارجع إلى 30fps.
- Start-Chroma-Key.cmd: إزالة خلفية خضراء مع خلفية تجريبية زرقاء.
- Start-AI-Matting.cmd: إزالة خلفية بالذكاء الاصطناعي بدون شاشة خضراء.
  قد يكون أبطأ من 30fps حسب GPU؛ اختبر الجودة الأصلية أولًا.
- داخل نافذة ويندوز: C للشاشة الخضراء، A لتبديل AI، O لإيقاف الإزالة، T للشفافية، Q للخروج.
- Power Saver على الهاتف يخفي المعاينة مع استمرار الإرسال؛ لمس الشاشة السوداء يعيدها.

إذا لم يبدأ الاتصال تلقائيًا:
- اسمح للبرنامج عبر Windows Firewall على الشبكة الخاصة، UDP 5353 للاكتشاف و5005 للتحكم و5004 للفيديو.
- لا تستخدم شبكة ضيوف تعزل الأجهزة عن بعضها.
- يمكن إيقاف Auto-connect وإدخال IPv4 الكمبيوتر والمنفذ 5004 يدويًا ثم Start streaming.

لإزالة الكاميرا الافتراضية: شغّل Uninstall-Virtual-Camera.cmd كمسؤول قبل نقل/حذف المجلد.
لا يتم تثبيت أو تسجيل أي شيء تلقائيًا بمجرد فك الضغط.

هذه نسخة اختبار محلية: EXE غير موقّع بشهادة نشر، وAPK موقّعة بمفتاح تطوير وغير قابلة للتصحيح.
الواجهة الرسومية والتقاط كاميرا USB الفعلية ليسا ضمن هذه النسخة؛ اختبار الفيديو هنا من هاتف أندرويد.
المتطلبات الواقعية هي GPU يدعم D3D11 video decode، وD3D12/DirectML عند تشغيل AI.
'@ | Set-Content -LiteralPath "$package\ابدأ-هنا.txt" -Encoding utf8BOM
if($Desktop){
@'
Cortex Studio Desktop 0.6 — خلفية مستقرة وتصوير طولي/عرضي

1) فك ضغط الحزمة بالكامل. لا تنقل EXE وحده.
2) شغّل CortexStudio.exe مباشرة. هذا التطبيق يفتح واجهة رسومية ومعاينة حية، بلا نافذة أوامر.
3) ثبّت تحديث Cortex Sender 0.6 على أندرويد، واسمح بالكاميرا، واجعل الهاتف والكمبيوتر على الراوتر نفسه.
   الاستقبال يبدأ وحده في ويندوز. افتح تطبيق الهاتف فقط ليكتشف الكمبيوتر ويبدأ البث.
   إذا سبق إيقاف البث على الهاتف، فتحه مجدداً يعيد الاتصال تلقائياً.
4) عندما يصل الفيديو سيظهر مباشرة داخل PROGRAM PREVIEW، بدون الحاجة إلى OBS أو Zoom.
تحديث 0.6 يحتاج تحديث التطبيقين لاستخدام اتجاه التصوير. Android 0.6 يرسل اتجاه الصورة داخل RTP، وWindows 0.6 يدورها على GPU ويختار 1080×1920 للطولي أو 1920×1080 للعرضي.
تم اختبار هاتف Samsung S24 Ultra فعلياً: 1920×1080، نحو 59 إطاراً/ثانية في الاستقبال، وصفر أخطاء فك ترميز خلال الاختبار.

العدادات:
- INCOMING FPS: عدد إطارات الصورة المفكوكة كل ثانية.
- PACKET GAPS: فجوات تسلسل حزم RTP؛ الصفر يعني عدم رصد فجوات، ولا يكفي وحده لإثبات نجاح فك الترميز.
- العداد أسفل المعاينة: عدد حزم الفيديو المستلمة؛ الإطار الواحد يتوزع عادة على حزم عديدة.
إذا وصل الفيديو ولم يظهر، يسجل logs/receiver-status.log أعداد الحزم والإطارات وسبب رفض المفكك، بدون حفظ صور الكاميرا.

على الهاتف:
- Auto / Portrait / Landscape: اختيار اتجاه التصوير، دون إعادة تشغيل الترميز.
- Home أو فتح تطبيق آخر: يبقى البث بالخلفية؛ المعاينة فقط تتوقف لتقليل عمل GPU.
- Black screen: شاشة سوداء ومعاينة متوقفة مع بقاء الكاميرا والترميز شغالين. المس الشاشة للعودة.
- Pause أو Stop streaming من الإشعار: إيقاف البث بشكل صريح.
- اسمح بالإشعارات حتى تستطيع العودة للتطبيق أو إيقافه وهو بالخلفية.
- مصدر الكاميرا يبقى 1920×1080 عبر Camera2 → MediaCodec Surface مباشرة، دون تحويل بكسلات على CPU.

التحكم من واجهة ويندوز:
- Original: الصورة الأصلية لتقييم الجودة أولًا.
- Green screen: إزالة الأخضر، مع سلايدر قوة المفتاح ونعومة الحواف وإزالة الانعكاس الأخضر.
- AI background removal: إزالة الخلفية بدون شاشة خضراء؛ نموذج RVM موجود داخل الحزمة.
- Choose image / Choose video: اختيار الخلفية من ملفاتك. الفيديو الخلفي المدعوم H.264.
- Transparent background: إبقاء ألفا في مسار المعاينة؛ إخراج برامج الويب كام يُسطّح على الأسود.
- 30fps / 60fps: معدل إخراج الكمبيوتر. يجب أيضًا ضبط الهاتف على 60 وإعادة الاتصال لاختبار 60 حقيقي.
- Connection: عرض IP الكمبيوتر وتغيير منفذي الفيديو والتحكم عند الحاجة.
- Reconnect phone: تحديث الاتصال عند الحاجة؛ لا تحتاج ضغطه للتشغيل، والاستقبال التلقائي يبقى مفعّلاً.

لإرسال الصورة إلى Zoom/Discord/OBS:
- اضغط Install virtual camera داخل الواجهة، ووافق بنفسك على طلب صلاحية المسؤول مرة واحدة.
- فعّل Send to webcam apps، ثم اختر Unity Video Capture داخل برنامج الفيديو.
- المعاينة داخل Cortex لا تحتاج تثبيت الكاميرا الافتراضية.

إذا ظهر Port is busy، أغلق نافذة الاختبار السوداء القديمة قبل تشغيل النسخة الجديدة، أو غيّر المنافذ من Connection.
عند أول تشغيل من هذا المجلد قد يطلب Windows السماح بالشبكة. وافق بنفسك مرة واحدة.
إذا لم يظهر الهاتف، اضغط Allow phone في الشريط الجانبي ووافق على صلاحية المسؤول.
هذا يسمح لاتصال UDP من الشبكة المحلية فقط، ويعمل أيضاً إذا صنّف Windows شبكة المنزل Public.
إذن الكاميرا في الهاتف والسماح بالشبكة في ويندوز لا يمكن للتطبيق تجاوزهما تلقائياً.
لا تستخدم Guest Wi-Fi الذي يعزل الأجهزة عن بعضها. يمكن للكمبيوتر الاتصال بالراوتر نفسه عبر الكابل.
اترك برنامج ويندوز مفتوحاً أو مصغراً. إغلاقه بالكامل يوقف الاستقبال؛ الهاتف لا يشغّل EXE مغلقاً.
إذا انقطع Wi-Fi أو أُعيد فتح البرنامج، يحاول التطبيقان استعادة الاتصال تلقائياً.

هذه حزمة Self-contained ولا تحتاج تثبيت .NET. ملفات DLL والنموذج موجودة معها.
AI قد يكون أبطأ من 30fps حسب GPU؛ ابدأ باختبار Original.
هذه النسخة تدعم مصدر الهاتف؛ مصدر USB الفعلي ليس مدمجًا بعد، ولا توجد أزرار وهمية تدّعي دعمه.
يمكن استخدام CortexReceiver-CLI.exe فقط للتشخيص؛ التشغيل العادي هو CortexStudio.exe.
'@ | Set-Content -LiteralPath "$package\ابدأ-هنا.txt" -Encoding utf8BOM
}

$apk=Join-Path $workspace 'android-sender\app\build\outputs\apk\quality\app-quality.apk'
Copy-Item -LiteralPath $apk -Destination "$dist\CortexSender-Android.apk"
Get-ChildItem -LiteralPath $package -Recurse -File | Where-Object Name -ne 'SHA256SUMS.txt' | ForEach-Object {
    $relative=$_.FullName.Substring($package.Length+1)
    "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $relative"
} | Set-Content -LiteralPath "$package\SHA256SUMS.txt" -Encoding utf8
Compress-Archive -LiteralPath $package -DestinationPath "$dist\$packageName.zip" -CompressionLevel Optimal -Force
Get-Item -LiteralPath "$dist\$packageName.zip","$dist\CortexSender-Android.apk" | Select-Object FullName,Length

