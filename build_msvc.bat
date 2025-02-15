@echo off
setlocal enabledelayedexpansion

set BUILD_CACHE=%~dp0\_build_cache.cmd

if exist "!BUILD_CACHE!" (
  rem cache file exists, so call it to set env variables very fast
  call "!BUILD_CACHE!"
) else (
  set __VSCMD_ARG_NO_LOGO=1
  for /f "tokens=*" %%i in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath') do set VS=%%i
  if "!VS!" equ "" (
    echo ERROR: Visual Studio installation not found
    exit /b 1
  )
  call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" amd64 || exit /b 1

  echo set PATH=!PATH!> "!BUILD_CACHE!"
  echo set INCLUDE=!INCLUDE!>> "!BUILD_CACHE!"
  echo set LIB=!LIB!>> "!BUILD_CACHE!"
  echo set VSCMD_ARG_TGT_ARCH=!VSCMD_ARG_TGT_ARCH!>> "!BUILD_CACHE!"

  rem Depending on whether you are build .NET or other stuff, there are more
  rem env variables you might want to add to cache, like:
  rem Platform, FrameworkDir, NETFXSDKDir, WindowsSdkDir, WindowsSDKVersion, VCINSTALLDIR, ...
)

rem put your build commands here

if "%VSCMD_ARG_TGT_ARCH%" neq "x64" (
  echo ERROR: please run this from MSVC x64 native tools command prompt, 32-bit target is not supported!
  exit /b 1
)

rem for getting date and time to stamp the pdb files so that we can ensure that debuggers won't load the wrong one if it was overwritten
rem due to how debuggers locate pdb by blindly loading up an absolute path stored in the PE header. This may
rem no longer be necessary since we may be able to specify a relative path instead using the link.exe /PDBALTPATH option.

rem for /f "delims=" %%i in ('powershell -nologo -Command "[System.DateTime]::Now.ToString(\"yyyy-MM-dd_HH-mm-ss_fff\")"') do (
rem     set "datetime_stamp=%%i"
rem )

set CL=/W4 /WX /Zi /Od /Gm- /diagnostics:caret /options:strict /DWIN32 /D_CRT_SECURE_NO_WARNINGS
rem set CL=%CL% /fsanitize=address

set LINK=/INCREMENTAL:NO
rem set "LINK=%LINK% /SUBSYSTEM:WINDOWS"
set "LINK=%LINK% kernel32.lib"
set "LINK=%LINK% d3d11.lib d3dcompiler.lib"


rem
rem barerg
rem

set "BARERG_DEV_FLAGS="
rem set "BARERG_DEV_FLAGS=%BARERG_DEV_FLAGS% /O2 /arch:AVX2"
rem set "BARERG_DEV_FLAGS=%BARERG_DEV_FLAGS% /DNDEBUG"
rem set "BARERG_DEV_FLAGS=%BARERG_DEV_FLAGS% /DTRACY_ENABLE"

set "IMGUI_SRC_FILE_LIST=imgui\imgui.cpp imgui\imgui_demo.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp imgui\backends\imgui_impl_dx11.cpp imgui\backends\imgui_impl_win32.cpp"

del barerg.pdb >nul 2>&1 & cl.exe /nologo /MP %BARERG_DEV_FLAGS% /Iimgui /Iimgui\backends barerg.cpp %IMGUI_SRC_FILE_LIST% /Febarerg.exe /link


del *.obj *.res >nul
