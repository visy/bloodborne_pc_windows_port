@echo off
setlocal

set "CURRENT_DIR=%~dp0"
if "%CURRENT_DIR:~-1%"=="\" set "CURRENT_DIR=%CURRENT_DIR:~0,-1%"

if not defined W64DEVKIT_DIR if exist "D:\DEV_CPP\w64devkit" set "W64DEVKIT_DIR=D:\DEV_CPP\w64devkit"
if not defined SDL3_DIR if exist "D:\DEV_CPP\SDL3-3.4.12\x86_64-w64-mingw32" set "SDL3_DIR=D:\DEV_CPP\SDL3-3.4.12\x86_64-w64-mingw32"
if not defined VULKAN_SDK if exist "D:\DEV_CPP\Vulkan-Headers" set "VULKAN_SDK=D:\DEV_CPP\Vulkan-Headers"

rem Check if directory path is deep and could cause MAX_PATH errors (> 70 chars).
rem Uses goto labels instead of nested ( ) blocks so '(' or ')' in paths never breaks cmd.exe.
if "%BB_SUBST_ACTIVE%"=="1" goto subst_done
if not defined CURRENT_DIR goto subst_done
if "%CURRENT_DIR:~70,1%"=="" goto subst_done
subst X: /d >nul 2>nul
subst X: "%CURRENT_DIR%"
if errorlevel 1 goto subst_done
echo [INFO] Path length exceeds safe MAX_PATH threshold. Switching to virtual drive X:\
set "BB_SUBST_ACTIVE=1"
cd /d X:\
call X:\build.bat %*
set "BUILD_EXIT_CODE=%ERRORLEVEL%"
cd /d "%CURRENT_DIR%"
subst X: /d >nul 2>nul
exit /b %BUILD_EXIT_CODE%
:subst_done

cd /d "%~dp0"

if not exist "out" mkdir out
if not exist "out\gpu" mkdir out\gpu

if defined W64DEVKIT_DIR set "PATH=%W64DEVKIT_DIR%\bin;%PATH%"
if defined MINGW_DIR goto mingw_path_set
if exist "C:\msys64\mingw64\bin" set "PATH=C:\msys64\mingw64\bin;%PATH%"
goto mingw_path_done
:mingw_path_set
set "PATH=%MINGW_DIR%\bin;%PATH%"
:mingw_path_done

if exist "C:\Program Files\CMake\bin" set "PATH=C:\Program Files\CMake\bin;%PATH%"

rem Determine parallel job count (defaults to logical CPU count; override with BB_BUILD_JOBS)
if not defined BB_BUILD_JOBS (
    if defined NUMBER_OF_PROCESSORS (
        set "BB_BUILD_JOBS=%NUMBER_OF_PROCESSORS%"
    ) else (
        set "BB_BUILD_JOBS=4"
    )
)

rem Find Python 3.10+ for staging and test scripts
if defined PYTHON goto found_python
for %%P in (
    "C:\Program Files\Python314\python.exe"
    "C:\Program Files\Python313\python.exe"
    "C:\Program Files\Python312\python.exe"
    "C:\Program Files\Python311\python.exe"
    "C:\Program Files\Python310\python.exe"
    "%LocalAppData%\Programs\Python\Python314\python.exe"
    "%LocalAppData%\Programs\Python\Python313\python.exe"
    "%LocalAppData%\Programs\Python\Python312\python.exe"
    "%LocalAppData%\Programs\Python\Python311\python.exe"
    "%LocalAppData%\Programs\Python\Python310\python.exe"
) do if not defined PYTHON if exist %%P set "PYTHON=%%~P"
if defined PYTHON goto found_python
for /f "delims=" %%i in ('where python.exe 2^>nul') do if not "%%~dpi"=="%LocalAppData%\Microsoft\WindowsApps\" if not defined PYTHON set "PYTHON=%%i"
if defined PYTHON goto found_python
for /f "tokens=*" %%i in ('where py.exe 2^>nul') do if not defined PYTHON set "PYTHON=%%i"
if not defined PYTHON set "PYTHON=python"
:found_python

rem Automatically initialize submodules if missing
if exist "gpu\third_party\fsr-vulkan\CMakeLists.txt" goto submodules_ok
git config --local core.longpaths true >nul 2>nul
echo Initializing submodules...
git submodule update --init --recursive
if errorlevel 1 (
    echo Failed to update submodules. Please run: git submodule update --init --recursive
    exit /b 1
)
:submodules_ok

rem Build LibAtrac9 static archive once (skip already-compiled .o files and archive directly)
if exist "out\libatrac9.a" goto atrac9_done
echo Building LibAtrac9...
if not exist "out\atrac9" mkdir out\atrac9
for %%f in (third_party\LibAtrac9\C\src\*.c) do (
    if not exist "out\atrac9\%%~nf.o" (
        gcc -std=c99 -O2 -g -w -c "%%f" -o "out\atrac9\%%~nf.o"
        if errorlevel 1 (
            echo Failed to compile LibAtrac9 source: %%f
            exit /b 1
        )
    )
)
pushd out\atrac9
del /f /q ..\libatrac9.a 2>nul
ar rcs ..\libatrac9.a *.o
set "AR_ERR=%ERRORLEVEL%"
popd
if not "%AR_ERR%"=="0" (
    echo Failed to create out\libatrac9.a
    exit /b 1
)
echo Built out\libatrac9.a
:atrac9_done

rem Configure CMake (Ninja) only when build.ninja is not yet generated
if exist "out\gpu\build.ninja" goto cmake_configured
git config --local core.longpaths true >nul 2>nul
if exist "patches\fsr_vulkan_mingw.patch" if exist "gpu\third_party\fsr-vulkan\.git" (
    git -C gpu\third_party\fsr-vulkan apply --check "..\..\..\patches\fsr_vulkan_mingw.patch" >nul 2>nul
    if not errorlevel 1 (
        echo Applying MinGW compatibility patch to FSR-Vulkan submodule...
        git -C gpu\third_party\fsr-vulkan apply "..\..\..\patches\fsr_vulkan_mingw.patch"
    )
)

echo Configuring CMake (Ninja)...
set "CMAKE_OPTS=-S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
where ninja.exe >nul 2>nul
if errorlevel 1 (
    if defined W64DEVKIT_DIR if exist "%W64DEVKIT_DIR%\bin\ninja.exe" (
        set "CMAKE_OPTS=%CMAKE_OPTS% -DCMAKE_MAKE_PROGRAM=%W64DEVKIT_DIR%/bin/ninja.exe"
    )
)
set "PREFIX_PATHS="
if defined SDL3_DIR set "PREFIX_PATHS=%SDL3_DIR%"
if defined VULKAN_SDK (
    if defined PREFIX_PATHS (
        set "PREFIX_PATHS=%PREFIX_PATHS%;%VULKAN_SDK%"
    ) else (
        set "PREFIX_PATHS=%VULKAN_SDK%"
    )
)
if exist "C:\msys64\mingw64" (
    if defined PREFIX_PATHS (
        set "PREFIX_PATHS=%PREFIX_PATHS%;C:/msys64/mingw64"
    ) else (
        set "PREFIX_PATHS=C:/msys64/mingw64"
    )
)
if defined CMAKE_PREFIX_PATH (
    if defined PREFIX_PATHS (
        set "PREFIX_PATHS=%PREFIX_PATHS%;%CMAKE_PREFIX_PATH%"
    ) else (
        set "PREFIX_PATHS=%CMAKE_PREFIX_PATH%"
    )
)
cmake %CMAKE_OPTS% -DCMAKE_PREFIX_PATH="%PREFIX_PATHS%"
if errorlevel 1 (
    echo CMake configuration failed.
    exit /b 1
)
:cmake_configured

echo Building bbgpu, bbport, and bb-gpu-capabilities (-j %BB_BUILD_JOBS%)...
cmake --build out/gpu --target bbgpu bbport bb-gpu-capabilities -- -j %BB_BUILD_JOBS%
if errorlevel 1 (
    echo Parallel build encountered an error; retrying with -j 1...
    cmake --build out/gpu --target bbgpu bbport bb-gpu-capabilities -- -j 1
    if errorlevel 1 (
        echo Build failed.
        exit /b 1
    )
)

if exist "out\bbport.exe" (
    copy /Y "out\bbport.exe" "out\bb-probe.exe" >nul
)

rem Stage runtime DLLs on first build, when SDL3.dll is missing, or when --stage-dlls is requested.
rem Skipping redundant objdump scans across 110+ DLLs saves 5-15s on every incremental build.
if /i "%~1"=="--stage-dlls" goto run_stage_dlls
if not exist "out\SDL3.dll" goto run_stage_dlls
if not exist "out\.dlls_staged" goto run_stage_dlls
goto stage_dlls_done
:run_stage_dlls
"%PYTHON%" scripts\stage_dlls.py
if not errorlevel 1 echo staged > "out\.dlls_staged"
:stage_dlls_done

echo Build complete: out\bbport.exe

if not "%~1"=="--test" goto build_done
echo Running unit tests...
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -Isrc tests/test_win32_exception.c -o out/win32-exception-test.exe
if errorlevel 1 exit /b 1
out\win32-exception-test.exe
if errorlevel 1 exit /b 1

set "SDL3_INC="
if defined SDL3_DIR set "SDL3_INC=-I%SDL3_DIR%/include"
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -I. -Isrc %SDL3_INC% tests/test_pad.c out/SDL3.dll -o out/pad-test.exe
if errorlevel 1 exit /b 1
out\pad-test.exe
if errorlevel 1 exit /b 1

if exist "out\test_runtime.exe" (
    out\test_runtime.exe
    if errorlevel 1 exit /b 1
)

gcc -std=c11 -O2 -g -Wall -Wextra -Werror -Isrc tests/test_file_mods.c -o out/file-mods-test.exe
if errorlevel 1 exit /b 1
out\file-mods-test.exe
if errorlevel 1 exit /b 1

if exist "out\test_sema.exe" (
    out\test_sema.exe
    if errorlevel 1 exit /b 1
)

if exist "out\content-test.exe" (
    out\content-test.exe
    if errorlevel 1 exit /b 1
)

cmake --build out/gpu --target shader-user-data-test motion-history-test motion-shader-test ui-composition-test upscaler-support-test -- -j %BB_BUILD_JOBS%
if errorlevel 1 exit /b 1

out\gpu\shader-user-data-test.exe
if errorlevel 1 exit /b 1
out\gpu\motion-history-test.exe
if errorlevel 1 exit /b 1
out\gpu\ui-composition-test.exe
if errorlevel 1 exit /b 1
out\gpu\upscaler-support-test.exe
if errorlevel 1 exit /b 1
out\gpu\motion-shader-test.exe
if errorlevel 1 exit /b 1

"%PYTHON%" -c "import glob, subprocess, sys; results = [(f, subprocess.run([sys.executable, f]).returncode) for f in sorted(glob.glob('tests/test_*.py'))]; [print(f, code) for f, code in results]; sys.exit(0 if all(code == 0 for f, code in results) else 1)"
if errorlevel 1 exit /b 1

echo ALL TESTS PASSED!
:build_done
