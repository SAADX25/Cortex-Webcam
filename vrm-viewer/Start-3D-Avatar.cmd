@echo off
setlocal
cd /d "%~dp0"

:: Close Cortex Studio if running so ports 5004/5005 are free for cortex-motion
taskkill /F /IM CortexStudio.exe >nul 2>&1
taskkill /F /IM cortex-motion.exe >nul 2>&1
taskkill /F /IM CortexVRM.exe >nul 2>&1

set "VRM_PATH=%~dp0CortexVRM-Viewer\CortexVRM_Data\StreamingAssets\MaleAvatar.vrm"

echo Starting Cortex 3D VRM Viewer (Ren inspired 3D)...
if "%~1"=="" (
    start "" /D "%~dp0CortexVRM-Viewer" "CortexVRM.exe" --vrm "%VRM_PATH%" --appearance ren
) else (
    start "" /D "%~dp0CortexVRM-Viewer" "CortexVRM.exe" %*
)

echo Starting Cortex Motion Tracker (Phone UDP 5004/5005 -^> VRM 127.0.0.1:39539)...
echo Close the 3D Viewer window or press Ctrl+C here to stop.
echo.
pushd "%~dp0CortexMotion-x64"
"cortex-motion.exe"
popd

taskkill /F /IM CortexVRM.exe >nul 2>&1
pause
