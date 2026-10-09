@echo off
setlocal

set "CURRENT_DIR=%~dp0"
if "%CURRENT_DIR:~-1%"=="\" set "CURRENT_DIR=%CURRENT_DIR:~0,-1%"

rem Check if directory path is deep and could cause MAX_PATH errors (> 70 chars)
rem If running directly from a deep path without a virtual root drive, mount and re-exec from X:
if not "%BB_SUBST_ACTIVE%"=="1" (
    set "BB_PATH_CHECK=%CURRENT_DIR%"
    if defined CURRENT_DIR (
        if "%CURRENT_DIR:~70,1%" neq "" (
            subst X: /d >nul 2>nul
            subst X: "%CURRENT_DIR%"
            if not errorlevel 1 (
                echo [INFO] Path length exceeds safe MAX_PATH threshold. Switching to virtual drive X:\
                set "BB_SUBST_ACTIVE=1"
                cd /d X:\
                call X:\build.bat %*
                set "BUILD_EXIT_CODE=%ERRORLEVEL%"
                cd /d "%CURRENT_DIR%"
                subst X: /d >nul 2>nul
                exit /b %BUILD_EXIT_CODE%
            )
        )
    )
)

cd /d "%~dp0"

if not exist "out" mkdir out
if not exist "out\gpu" mkdir out\gpu

if defined W64DEVKIT_DIR (
    set "PATH=%W64DEVKIT_DIR%\bin;%PATH%"
)
if defined MINGW_DIR (
    set "PATH=%MINGW_DIR%\bin;%PATH%"
) else if exist "C:\msys64\mingw64\bin" (
    set "PATH=C:\msys64\mingw64\bin;%PATH%"
)

if exist "C:\Program Files\CMake\bin" (
    set "PATH=C:\Program Files\CMake\bin;%PATH%"
)

rem Ensure Git handles long paths on Windows without failing on FidelityFX headers
git config --local core.longpaths true >nul 2>nul

rem Automatically initialize and update submodules if missing
if not exist "gpu\third_party\fsr-vulkan\CMakeLists.txt" (
    echo Initializing submodules...
    git submodule update --init --recursive
    if errorlevel 1 (
        echo Failed to update submodules. Please run: git submodule update --init --recursive
        exit /b 1
    )
)

if not exist "out\libatrac9.a" (
    echo Building LibAtrac9
    if not exist "out\atrac9" mkdir out\atrac9
    for %%f in (third_party\LibAtrac9\C\src\*.c) do (
        gcc -std=c99 -O2 -g -w -c "%%f" -o "out\atrac9\%%~nf.o"
        if errorlevel 1 (
            echo Failed to compile LibAtrac9 source: %%f
            exit /b 1
        )
    )
    pushd out\atrac9
    del /f /q ..\libatrac9.a 2>nul
    set "OBJS="
    for %%o in (*.o) do call set "OBJS=%%OBJS%% %%o"
    ar rcs ..\libatrac9.a %OBJS%
    popd
    if errorlevel 1 (
        echo Failed to create out\libatrac9.a
        exit /b 1
    )
    echo Built out\libatrac9.a
)

rem Upstream FSR-Vulkan patches first (as build.sh), then the MinGW compatibility patch
if exist "gpu\third_party\fsr-vulkan\.git" for %%p in (gpu\patches\fsr-vulkan\*.patch) do (
    git -C gpu\third_party\fsr-vulkan apply --reverse --check "%CD%\%%p" >nul 2>nul
    if errorlevel 1 (
        echo Applying %%~nxp to FSR-Vulkan submodule...
        git -C gpu\third_party\fsr-vulkan apply "%CD%\%%p"
    )
)

if exist "patches\fsr_vulkan_mingw.patch" if exist "gpu\third_party\fsr-vulkan\.git" (
    git -C gpu\third_party\fsr-vulkan apply --check "..\..\..\patches\fsr_vulkan_mingw.patch" >nul 2>nul
    if not errorlevel 1 (
        echo Applying MinGW compatibility patch to FSR-Vulkan submodule...
        git -C gpu\third_party\fsr-vulkan apply "..\..\..\patches\fsr_vulkan_mingw.patch"
    )
)

if not exist "out\gpu\build.ninja" (
    echo Configuring CMake (Ninja)
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
)

echo Building bbgpu, bbport, and bb-gpu-capabilities
cmake --build out/gpu --target bbgpu bbport bb-gpu-capabilities -- -j 1
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

if exist "out\bbport.exe" (
    copy /Y "out\bbport.exe" "out\bb-probe.exe" >nul
)

python scripts\stage_dlls.py

rem Optional DLSS (NVIDIA RTX): DLSS_SDK_ROOT=<github.com/NVIDIA/DLSS checkout> builds the bridge
rem (gpu/dlss_bridge, the only code using NVIDIA's SDK; MSVC only, since the SDK's library is) and
rem puts bbport_dlss.dll and NVIDIA's nvngx_dlss.dll next to bbport.exe. Without them the DLSS
rem upscaler is listed as unavailable.
if not defined DLSS_SDK_ROOT goto dlss_done
set "DLSS_VK_INC="
if defined VULKAN_SDK set DLSS_VK_INC="-DVULKAN_INCLUDE=%VULKAN_SDK%/Include"
if not defined VULKAN_SDK if exist "C:\msys64\mingw64\include\vulkan\vulkan.h" set DLSS_VK_INC="-DVULKAN_INCLUDE=C:/msys64/mingw64/include"
echo Building the DLSS bridge (Visual Studio 2022)
cmake -S gpu/dlss_bridge -B out/dlss-bridge -G "Visual Studio 17 2022" -A x64 "-DDLSS_SDK_ROOT=%DLSS_SDK_ROOT%" %DLSS_VK_INC%
if errorlevel 1 goto dlss_failed
cmake --build out/dlss-bridge --config Release
if errorlevel 1 goto dlss_failed
copy /Y "out\dlss-bridge\Release\bbport_dlss.dll" "out\" >nul
if errorlevel 1 goto dlss_failed
copy /Y "%DLSS_SDK_ROOT%\lib\Windows_x86_64\rel\nvngx_dlss.dll" "out\" >nul
if errorlevel 1 goto dlss_failed
if exist "%DLSS_SDK_ROOT%\LICENSE.txt" copy /Y "%DLSS_SDK_ROOT%\LICENSE.txt" "out\NVIDIA-DLSS-LICENSE.txt" >nul
echo DLSS bridge: out\bbport_dlss.dll
goto dlss_done
:dlss_failed
echo DLSS bridge build failed: it needs Visual Studio 2022 and DLSS_SDK_ROOT pointing at the NVIDIA DLSS SDK.
exit /b 1
:dlss_done

echo Build complete: out\bbport.exe

if "%~1"=="--test" (
    echo Running unit tests
    gcc -std=c11 -O2 -g -Wall -Wextra -Werror -Isrc tests/test_win32_exception.c -o out/win32-exception-test.exe
    if errorlevel 1 exit /b 1
    out\win32-exception-test.exe
    if errorlevel 1 exit /b 1

    set "SDL3_INC="
    if defined SDL3_DIR (
        set "SDL3_INC=-I%SDL3_DIR%/include"
    ) else if exist "C:\msys64\mingw64\include\SDL3" (
        set "SDL3_INC=-IC:/msys64/mingw64/include"
    )
    gcc -std=c11 -O2 -g -Wall -Wextra -Werror -I. -Isrc %SDL3_INC% tests/test_pad.c out/SDL3.dll -o out/pad-test.exe
    if errorlevel 1 exit /b 1
    out\pad-test.exe
    if errorlevel 1 exit /b 1

    out\test_runtime.exe
    if errorlevel 1 exit /b 1

    gcc -std=c11 -O2 -g -Wall -Wextra -Werror -Isrc tests/test_file_mods.c -o out/file-mods-test.exe
    if errorlevel 1 exit /b 1
    out\file-mods-test.exe
    if errorlevel 1 exit /b 1

    out\test_sema.exe
    if errorlevel 1 exit /b 1

    out\content-test.exe
    if errorlevel 1 exit /b 1

    cmake --build out/gpu --target shader-user-data-test motion-history-test motion-shader-test ui-composition-test upscaler-support-test
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

    python -c "import glob, subprocess, sys; results = [(f, subprocess.run([sys.executable, f]).returncode) for f in sorted(glob.glob('tests/test_*.py'))]; [print(f, code) for f, code in results]; sys.exit(0 if all(code == 0 for f, code in results) else 1)"
    if errorlevel 1 exit /b 1

    echo ALL TESTS PASSED!
)
