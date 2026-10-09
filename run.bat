@echo off
setlocal

cd /d "%~dp0"

REM Find Python 3
if "%PYTHON%"=="" (
    for %%P in (
        "C:\Program Files\Python313\python.exe"
        "C:\Program Files\Python312\python.exe"
        "C:\Program Files\Python311\python.exe"
        "C:\Program Files\Python310\python.exe"
        "%LocalAppData%\Programs\Python\Python313\python.exe"
        "%LocalAppData%\Programs\Python\Python312\python.exe"
        "%LocalAppData%\Programs\Python\Python311\python.exe"
        "%LocalAppData%\Programs\Python\Python310\python.exe"
    ) do (
        if not defined PYTHON if exist %%P set "PYTHON=%%~P"
    )
)
if "%PYTHON%"=="" (
    for /f "delims=" %%i in ('where python.exe 2^>nul') do (
        set "pycand=%%i"
        if not "%%~dpi"=="%LocalAppData%\Microsoft\WindowsApps\" (
            if "%PYTHON%"=="" set "PYTHON=%%i"
        )
    )
)
if "%PYTHON%"=="" (
    for /f "tokens=*" %%i in ('where py.exe 2^>nul') do (
        if "%PYTHON%"=="" set "PYTHON=%%i"
    )
)
if "%PYTHON%"=="" set "PYTHON=python"

REM Started without the launcher: apply its last saved settings (launcher_settings.json).
REM A game path passed on the command line still wins. BB_NO_LAUNCHER_SETTINGS=1 skips this.
if not "%BB_FROM_LAUNCHER%"=="1" if not "%BB_NO_LAUNCHER_SETTINGS%"=="1" if exist "launcher_settings.json" (
    del "%TEMP%\bbport_launcher_env.bat" >nul 2>nul
    "%PYTHON%" launcher.py --write-env "%TEMP%\bbport_launcher_env.bat"
    if exist "%TEMP%\bbport_launcher_env.bat" (
        call "%TEMP%\bbport_launcher_env.bat"
        del "%TEMP%\bbport_launcher_env.bat" >nul 2>nul
        echo Using launcher settings from launcher_settings.json
    )
)

REM Writable directory for generated files (out/), saves (user/) and bbport.ini
if "%BB_DATA_DIR%"=="" set "BB_DATA_DIR=."
set "out=%BB_DATA_DIR%\out"
if not exist "%out%" mkdir "%out%"
if "%BB_CONFIG%"=="" set "BB_CONFIG=%BB_DATA_DIR%\bbport.ini"

REM BB_SAVE_LOG=1 (launcher: "Save frame statistics to logs\"): this run's per-frame and readback
REM statistics go to %BB_DATA_DIR%\logs\<time>.frames.csv and .readbacks.csv (as run.sh). The
REM console output itself is in launcher.log when started from the launcher.
if not "%BB_SAVE_LOG%"=="1" goto save_log_done
if not exist "%BB_DATA_DIR%\logs" mkdir "%BB_DATA_DIR%\logs"
"%PYTHON%" -c "import time; print(time.strftime('%%Y%%m%%d_%%H%%M%%S'))" > "%out%\log_stamp.txt"
set "stamp="
if exist "%out%\log_stamp.txt" for /f "usebackq delims=" %%t in ("%out%\log_stamp.txt") do set "stamp=%%t"
if "%stamp%"=="" set "stamp=latest"
set "BB_FRAME_STATS=1"
if "%BB_FRAME_LOG%"=="" set "BB_FRAME_LOG=%BB_DATA_DIR%\logs\%stamp%.frames.csv"
if "%BB_READBACK_LOG%"=="" set "BB_READBACK_LOG=%BB_DATA_DIR%\logs\%stamp%.readbacks.csv"
echo Frame statistics: %BB_FRAME_LOG%
:save_log_done

REM Game directory containing eboot.bin
set "EXTRA_ARGS="
:arg_loop
if "%~1"=="" goto arg_done
if exist "%~1\eboot.bin" (
    set "BB_GAME_DIR=%~1"
    shift
    goto arg_loop
)
if "%~nx1"=="eboot.bin" if exist "%~1" (
    set "BB_GAME_DIR=%~dp1"
    shift
    goto arg_loop
)
set EXTRA_ARGS=%EXTRA_ARGS% %1
shift
goto arg_loop
:arg_done

if "%BB_GAME_DIR%"=="" set "BB_GAME_DIR=..\CUSA03173"
set "game=%BB_GAME_DIR%"
REM Strip trailing backslash if present
if "%game:~-1%"=="\" set "game=%game:~0,-1%"
if not exist "%game%\eboot.bin" (
    echo No eboot.bin in %game% ^(set BB_GAME_DIR or pass path to run.bat^). >&2
    exit /b 1
)

set "mods_dir=%BB_MODS_DIR%"
if "%mods_dir%"=="" set "mods_dir=%BB_DATA_DIR%\mods"
set "mods_config=%BB_MODS_CONFIG%"
if "%mods_config%"=="" set "mods_config=%BB_DATA_DIR%\mods.json"
set "mods_enabled=%BB_MODS_ENABLED%"
if "%mods_enabled%"=="" set "mods_enabled=1"

REM NOTE: for /f ('"exe" args "more args"') trips cmd's quote-stripping rule and prints
REM "The filename, directory name, or volume label syntax is incorrect." So write the
REM output to a temp file and read that instead (last line wins, same as before).
set "mods_result=%out%\mods_game.txt"
if exist "%mods_result%" del "%mods_result%"
"%PYTHON%" scripts\mods.py "%game%" --out "%out%" --mods-dir "%mods_dir%" --config "%mods_config%" --enabled "%mods_enabled%" > "%mods_result%"
if exist "%mods_result%" for /f "usebackq delims=" %%g in ("%mods_result%") do set "game=%%g"

REM prepare.py also checks the game files (scripts\game_check.py): exit code 2 = not the 1.09
REM executable (BB_SKIP_GAME_CHECK=1 starts anyway).
"%PYTHON%" scripts\prepare.py "%game%" --out "%out%"
if errorlevel 1 exit /b %ERRORLEVEL%

"%PYTHON%" scripts\link_libc.py "%game%" --out "%out%"
if errorlevel 1 exit /b 1

"%PYTHON%" scripts\link_modules.py "%game%" --out "%out%"
if errorlevel 1 exit /b 1

set "sku=%BB_CONTENT_SKU%"
if "%sku%"=="" set "sku=full"
"%PYTHON%" scripts\content_profile.py "%game%" --out "%out%" --sku "%sku%"
if errorlevel 1 exit /b 1

if "%BB_AUTO_RENDER_RES%"=="1" (
    set "BB_RENDER_RES="
    set "BB_OUTPUT_RES="
    set "BB_AUTO_RENDER_RES="
)

if "%BB_FPS%"=="" set "BB_FPS=uncap"
set "fps=%BB_FPS%"

set "scaled_render="
set "scaled_output="
if "%BB_RENDER_RES%"=="" (
    for /f "tokens=1,2" %%a in ('"%PYTHON%" scripts\patches.py --print-scaled --settings "%BB_CONFIG%" 2^>nul') do (
        set "scaled_render=%%a"
        set "scaled_output=%%b"
    )
)

set "live=0"
if "%scaled_output%"=="" goto live_done
if not "%BB_LIVE_RES%"=="" set "live=%BB_LIVE_RES%"
if not "%live%"=="0" goto live_chosen
if not exist "%BB_CONFIG%" goto live_chosen
for /f "usebackq tokens=1,2 delims== " %%a in ("%BB_CONFIG%") do (
    if "%%a"=="live_resolution" set "live=%%b"
)
:live_chosen
if not "%live%"=="auto" goto live_checked
set "live=0"
if exist "out\bb-gpu-capabilities.exe" (
    out\bb-gpu-capabilities.exe --live-resolution > "%out%\live_resolution.txt" 2>nul
    for /f "usebackq delims=" %%c in ("%out%\live_resolution.txt") do set "live=%%c"
)
:live_checked
if not "%live%"=="1" set "live=0"
if "%live%"=="1" (
    echo Output %scaled_output%: live resolution changes ^(live_resolution=0: startup patch^)
    goto live_done
)
set "BB_RENDER_RES=%scaled_render%"
set "BB_OUTPUT_RES=%scaled_output%"
set "BB_AUTO_RENDER_RES=1"
if "%BB_DMEM_MB%"=="" set "BB_DMEM_MB=9152"
echo Output %scaled_output%: scene %scaled_render%, direct memory %BB_DMEM_MB% MiB ^(live_resolution=1: live changes^)
:live_done

REM Explicit launcher resolutions skip scaled_output above, but patches.py still adds
REM Increased Graphics Heap Sizes above 1080p. Match that patch's direct-memory budget.
set "render_pixels=0"
if defined BB_RENDER_RES for /f "tokens=1,2 delims=xX" %%a in ("%BB_RENDER_RES%") do set /a "render_pixels=%%a*%%b" >nul 2>nul
if %render_pixels% GTR 2073600 if not defined BB_DMEM_MB set "BB_DMEM_MB=9152"
if %render_pixels% GTR 2073600 echo Render %BB_RENDER_RES%: direct memory %BB_DMEM_MB% MiB

set "patches_dir=%BB_PATCHES_DIR%"
if "%patches_dir%"=="" set "patches_dir=%BB_DATA_DIR%\patches"
set "patches_config=%BB_PATCHES_CONFIG%"
if "%patches_config%"=="" set "patches_config=%BB_DATA_DIR%\patches.json"

"%PYTHON%" scripts\patches.py --out "%out%" --fps "%fps%" --extra "%BB_PATCHES%" --settings "%BB_CONFIG%" --game-dir "%game%" --render-res "%BB_RENDER_RES%" --output-res "%BB_OUTPUT_RES%" --patches-dir "%patches_dir%" --patches-config "%patches_config%"
if errorlevel 1 exit /b 1

REM GPU memory and command processing defaults, as run.sh sets them:
REM BB_PREUPLOAD: background upload of the game's GPU memory into VRAM ahead of use (1 = only
REM   memory already in VRAM that the game rewrote, 2 = all of it, ~3 GB more VRAM, 0 = off).
REM BB_PC_MODEL=1: upstream's experimental new memory and translation model (off by default;
REM   developed on Linux). BB_GUEST_IN_PLACE set by hand overrides it.
REM BB_AS_0_3=1: synchronisation and memory as released in 0.3, for comparisons.
REM BB_COPY_GPU_BUFFERS: command buffers are copied when submitted and decoded from the copy.
REM BB_GPU_WRITE_TWINS: guest writes next to small GPU outputs do not wait for the GPU.
if "%BB_PREUPLOAD%"=="" set "BB_PREUPLOAD=1"
if "%BB_AS_0_3%"=="1" (
    set "BB_GUEST_IN_PLACE=0"
    set "BB_HOST_COPY_WAITS=all"
    set "BB_PRODUCER_CHECK=1"
)
if "%BB_PC_MODEL%"=="" set "BB_PC_MODEL=0"
if "%BB_GUEST_IN_PLACE%"=="" set "BB_GUEST_IN_PLACE=%BB_PC_MODEL%"
if "%BB_COPY_GPU_BUFFERS%"=="" set "BB_COPY_GPU_BUFFERS=1"
if "%BB_GPU_WRITE_TWINS%"=="" set "BB_GPU_WRITE_TWINS=1"
if "%BB_GPU_WRITE_TWINS_MAX%"=="" set "BB_GPU_WRITE_TWINS_MAX=65536"

REM Frame rate: uncap = delta-time patch, vblank 480 Hz (frames shown at once); 60/90 fixed.
if "%BB_VBLANK_HZ%"=="" (
    if "%fps%"=="uncap" (
        set "BB_VBLANK_HZ=480"
    ) else if "%fps%"=="90" (
        set "BB_VBLANK_HZ=90"
    ) else (
        set "BB_VBLANK_HZ=60"
    )
)

if not exist "out\bbport.exe" (
    if exist "build.bat" (
        call build.bat
        if errorlevel 1 exit /b 1
    ) else (
        echo [ERROR] out\bbport.exe not found. >&2
        exit /b 1
    )
)

REM BB_PROBE: another executable (e.g. a debugger wrapper) instead of out\bbport.exe.
set "probe=%BB_PROBE%"
if "%probe%"=="" set "probe=out\bbport.exe"
set "user_dir=%BB_USER_DIR%"
if "%user_dir%"=="" set "user_dir=%BB_DATA_DIR%\user"
REM The Old Hunters: its data ships in patch 1.09 (maps m34-m36), the game only needs the add-on
REM reported as installed. A fresh install gets the entitlement folder automatically when the
REM DLC maps are present and no add-on folder exists yet. BB_AUTO_DLC=0 skips this.
if not "%BB_AUTO_DLC%"=="0" if exist "%game%\dvdroot_ps4\map\m34" if not exist "%user_dir%\addcont" (
    mkdir "%user_dir%\addcont\CUSA03173\SPEXPANSIONDLC03" >nul 2>nul
    echo The Old Hunters: add-on enabled ^(%user_dir%\addcont^)
)
set "timeout=%BB_TIMEOUT%"
if "%timeout%"=="" set "timeout=0"

echo Launching Bloodborne PC (%probe%)...
"%probe%" "%out%\boot-linked.bin" --content-profile "%out%\content.bin" --patches "%out%\patches.bin" --app0 "%game%" --user "%user_dir%" --timeout "%timeout%" %EXTRA_ARGS%
exit /b %ERRORLEVEL%
