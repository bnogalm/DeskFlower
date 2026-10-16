@echo off
rem Builds a release, installs a self-contained copy (executables + Qt + OpenSSL + MSVC runtime) into
rem dist\Deskflower and wraps it into dist\Deskflower-<version>-Windows-x64.msi with WiX 3 (downloaded once
rem into build\tools\wix, nothing is installed on the machine).
rem Update signing (README, "Signed updates"): the key pair lives in %DESKFLOWER_SIGNING_DIR%, by default
rem release-signing-key\ at the repository root (git-ignored, never packaged). It is created on the
rem first run; the build embeds its public key and the MSI gets a signed manifest (<msi>.manifest +
rem .manifest.sig) for paired-device updates. BACK UP THAT FOLDER: without it no later release can
rem update the installed devices.
rem Usage: scripts\package-windows.bat
setlocal
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "DEST=%ROOT%\dist\Deskflower"
set "WIX=%ROOT%\build\tools\wix"
set "WORK=%ROOT%\build\installer"
set "KEYDIR=%DESKFLOWER_SIGNING_DIR%"
if "%KEYDIR%"=="" set "KEYDIR=%ROOT%\release-signing-key"
set "SIGN=%ROOT%\build\win-release\deskflower-sign.exe"
set "NEWKEY=no"

if exist "%KEYDIR%\update-signing.key" goto havekey
rem First run: build the signing tool, then create the key pair.
call "%~dp0build-windows.bat" win-release || exit /b 1
set "PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%"
"%SIGN%" keygen "%KEYDIR%" || exit /b 1
(
    echo DESKFLOWER RELEASE SIGNING KEY - BACK UP THIS FOLDER
    echo.
    echo update-signing.key is the SECRET key that signs Deskflower installers. Installed devices
    echo only accept an update from a paired device when its package is signed with this key.
    echo update-signing.pub is the matching public key, built into every release.
    echo.
    echo - Copy this whole folder somewhere safe ^(password manager, encrypted drive^). Never commit
    echo   it, share it or put it into an installer. The repository's .gitignore excludes it.
    echo - To package on another machine, copy the folder to release-signing-key\ at the repository
    echo   root ^(or point DESKFLOWER_SIGNING_DIR at it^).
    echo - If it is lost, the next package run creates a new key and every device has to be
    echo   reinstalled by hand once: the releases already installed do not trust the new key.
) > "%KEYDIR%\README-BACKUP.txt"
set "NEWKEY=yes"

:havekey
if not exist "%KEYDIR%\update-signing.pub" (
    echo %KEYDIR%\update-signing.pub is missing: restore the whole key folder from your backup.
    exit /b 1
)
rem The release accepts updates signed with this key (CMake reads the variable when it is set).
set /p DESKFLOWER_UPDATE_PUBKEY=<"%KEYDIR%\update-signing.pub"
call "%~dp0build-windows.bat" win-release || exit /b 1
set "PATH=C:\Qt\Tools\CMake_64\bin;%PATH%"
if exist "%DEST%" rmdir /s /q "%DEST%"
cmake --install "%ROOT%\build\win-release" --prefix "%DEST%" || exit /b 1

rem Version from the CMake project (written at configure time).
set /p VERSION=<"%ROOT%\build\win-release\version.txt"

if not exist "%WIX%\candle.exe" (
    echo Downloading WiX 3.14 binaries...
    if not exist "%WIX%" mkdir "%WIX%"
    curl -sfL -o "%WIX%\wix.zip" https://github.com/wixtoolset/wix3/releases/download/wix3141rtm/wix314-binaries.zip || exit /b 1
    powershell -NoProfile -Command "Expand-Archive -Force '%WIX%\wix.zip' '%WIX%'" || exit /b 1
)

if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%WORK%"
"%WIX%\heat.exe" dir "%DEST%" -nologo -cg AppFiles -dr INSTALLFOLDER -srd -ag -sreg -scom -var var.SourceDir -out "%WORK%\files.wxs" || exit /b 1
"%WIX%\candle.exe" -nologo -arch x64 -ext WixUtilExtension -dVersion=%VERSION% -dSourceDir="%DEST%" -dIconFile="%ROOT%\resources\icons\deskflower.ico" -out "%WORK%\\" "%ROOT%\installer\windows\deskflower.wxs" "%WORK%\files.wxs" || exit /b 1
rem ICE38/43/57 expect per-user shortcuts; this is a per-machine package (ALLUSERS=1), where the
rem Start menu folder is the common one and an HKLM key path is the right choice.
if exist "%ROOT%\dist\Deskflower-%VERSION%-Windows-x64.msi" del "%ROOT%\dist\Deskflower-%VERSION%-Windows-x64.msi"
"%WIX%\light.exe" -nologo -ext WixUtilExtension -spdb -sice:ICE38 -sice:ICE43 -sice:ICE57 -out "%ROOT%\dist\Deskflower-%VERSION%-Windows-x64.msi" "%WORK%\deskflower.wixobj" "%WORK%\files.wixobj" || exit /b 1

rem Signed manifest for updates between paired devices. The signing tool is not shipped; it runs
rem with the Qt and MSVC runtime DLLs of the packaged copy.
set "MSI=%ROOT%\dist\Deskflower-%VERSION%-Windows-x64.msi"
if exist "%MSI%.manifest" del "%MSI%.manifest"
if exist "%MSI%.manifest.sig" del "%MSI%.manifest.sig"
set "PATH=%DEST%\bin;%PATH%"
"%SIGN%" manifest "%MSI%" %VERSION% || exit /b 1
"%SIGN%" sign "%KEYDIR%\update-signing.key" "%MSI%.manifest" || exit /b 1
"%SIGN%" verify --compiled "%MSI%.manifest" "%MSI%.manifest.sig" || goto keymismatch

echo.
echo Installer: dist\Deskflower-%VERSION%-Windows-x64.msi
echo Signed update manifest: dist\Deskflower-%VERSION%-Windows-x64.msi.manifest + .manifest.sig
echo Portable copy: dist\Deskflower\bin
echo Update signing key: %KEYDIR%
if "%NEWKEY%"=="no" goto :eof
echo.
echo ****************************************************************************************
echo   NEW UPDATE SIGNING KEY CREATED IN:
echo     %KEYDIR%
echo   BACK UP THIS FOLDER NOW. It is not in the repository; without it, later releases
echo   cannot update the devices you install from this MSI. See README-BACKUP.txt inside.
echo ****************************************************************************************
goto :eof

:keymismatch
echo The signing key does not match DESKFLOWER_UPDATE_PUBKEY in build\win-release: delete the manifest
echo or rebuild with the right key.
del "%MSI%.manifest" "%MSI%.manifest.sig"
exit /b 1
