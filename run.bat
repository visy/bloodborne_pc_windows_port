@echo off
setlocal
REM BB_PARTY off values (unset, empty, 0, off, false, no; any case, blanks ignored) mean party
REM mode off: cleared here so the game, scripts\patches.py and the restart loop below all see it
REM unset (the same rule as gpu\party_env.h and scripts\party_env.py).
if not defined BB_PARTY goto party_env_done
set "party_check=%BB_PARTY: =%"
if "%party_check%"=="" set "BB_PARTY="
for %%v in (0 off false no) do if /i "%party_check%"=="%%v" set "BB_PARTY="
set "party_check="
:party_env_done

cd /d "%~dp0"
REM Startup timeline (docs/STARTUP.md): the clock time here and at the launch below.
echo Boot: run.bat started at %TIME%

REM Find Python 3. The release's bundled python\ folder comes first (no installed Python needed);
REM a PYTHON the launcher passes is that same one when the launcher runs on it.
REM A partly extracted or quarantined folder fails a quick import check and is skipped.
if defined PYTHON goto bundle_done
if not exist "%~dp0python\python.exe" goto bundle_done
"%~dp0python\python.exe" -I -B -c "import encodings, zipfile, xml.etree.ElementTree" >nul 2>nul
if errorlevel 1 goto bundle_broken
set "PYTHON=%~dp0python\python.exe"
goto bundle_done
:bundle_broken
echo bundled Python is incomplete (re-extract the zip); using an installed Python
:bundle_done
REM Never write __pycache__ into the bundle, and ignore a system Python's PYTHONHOME / PYTHONPATH.
if /i "%PYTHON%"=="%~dp0python\python.exe" set "PYTHONDONTWRITEBYTECODE=1" & set "PYTHONHOME=" & set "PYTHONPATH="
if "%PYTHON%"=="" (
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
    ) do (
        if not defined PYTHON if exist %%P set "PYTHON=%%~P"
    )
)
if "%PYTHON%"=="" (
    for /f "delims=" %%i in ('where python.exe 2^>nul') do (
        REM Real Python 3.10+ only: also finds the Microsoft Store install, rejects its placeholder alias.
        if "%PYTHON%"=="" (
            "%%i" -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>nul
            if not errorlevel 1 set "PYTHON=%%i"
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
REM An in-game restart or a crash can leave the old bbport.exe alive with gigabytes of RAM and
REM VRAM. Give it a few seconds to exit, then terminate it (the guard never fails the launch).
if exist scripts\bb_guard.py "%PYTHON%" scripts\bb_guard.py --exe-dir out --wait 10

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
REM No ( ) block around a path: an unquoted ')' in it (Program Files (x86)) ends the block early.
if exist "%game%\eboot.bin" goto found_eboot
echo No eboot.bin in "%game%" ^(set BB_GAME_DIR or pass path to run.bat^). >&2
exit /b 1
:found_eboot

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

REM The boot image (scripts\boot_image.py: prepare.py, link_libc.py, link_modules.py in one process)
REM is built once and reused while the game files and scripts stay the same (docs\STARTUP.md;
REM BB_BOOT_CACHE=0 rebuilds). prepare.py also checks the game files (scripts\game_check.py): exit
REM code 2 = not the 1.09 executable, or a damaged extraction whose shaders do not unpack (issue
REM #81; the game would hang while loading). BB_SKIP_GAME_CHECK=1 starts anyway. Every file:
REM python scripts\game_check.py GAME_DIR
set "sku=%BB_CONTENT_SKU%"
if "%sku%"=="" set "sku=full"
"%PYTHON%" scripts\boot_image.py "%game%" --out "%out%" --sku "%sku%"
if errorlevel 1 exit /b %ERRORLEVEL%
REM Legacy CPU profile (scripts\cpu_compat.py, from Supermedo/bloodborne_pc): bb-probe's CPU check lets
REM a CPU without BMI1, LZCNT or MOVBE start when the image was translated.
set "BB_CPU_COMPAT="
if exist "%out%\cpu_compat.txt" for /f "usebackq delims=" %%c in ("%out%\cpu_compat.txt") do set "BB_CPU_COMPAT=%%c"

if "%BB_AUTO_RENDER_RES%"=="1" (
    set "BB_RENDER_RES="
    set "BB_OUTPUT_RES="
    set "BB_AUTO_RENDER_RES="
)

if "%BB_FPS%"=="" set "BB_FPS=uncap"
set "fps=%BB_FPS%"

REM The scaled sizes go through a file, like mods.py above: read with for /f ('"%PYTHON%" ...
REM "%BB_CONFIG%"') the command lost its quotes and printed nothing, so an output other than
REM 1080p never got its startup patch (or its direct memory) and always ran live.
set "scaled_render="
set "scaled_output="
set "scaled_hud="
set "BB_UI_RES="
set "scaled_result=%out%\scaled_sizes.txt"
if exist "%scaled_result%" del "%scaled_result%"
if "%BB_RENDER_RES%"=="" "%PYTHON%" scripts\patches.py --print-scaled --settings "%BB_CONFIG%" > "%scaled_result%" 2>nul
if exist "%scaled_result%" for /f "usebackq tokens=1,2,3" %%a in ("%scaled_result%") do (
    set "scaled_render=%%a"
    set "scaled_output=%%b"
    set "scaled_hud=%%c"
)

REM One statement per line: inside a ( ) block %live% keeps the value it had before the block,
REM which turned live_resolution=1 and auto from bbport.ini back into 0.
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
REM Ultrawide outputs (21:9, 32:9; from Supermedo/bloodborne_pc PR #70): the camera and the HUD
REM canvas need the startup patch, never live changes. The HUD canvas is wider than 1920x1080
REM (patches.py hud_canvas); the GPU library scales it to the output (BB_UI_RES).
if "%scaled_hud%"=="" goto ultrawide_done
if "%scaled_hud%"=="1920x1080" goto ultrawide_done
set "live=0"
set "BB_UI_RES=%scaled_hud%"
echo Output %scaled_output%: ultrawide, HUD %scaled_hud%
:ultrawide_done
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

REM The party flow's restart (exit code 75, below) comes back here: party mode changes the patches.
:prepare_patches
set "patches_dir=%BB_PATCHES_DIR%"
if "%patches_dir%"=="" set "patches_dir=%BB_DATA_DIR%\patches"
set "patches_config=%BB_PATCHES_CONFIG%"
if "%patches_config%"=="" set "patches_config=%BB_DATA_DIR%\patches.json"

"%PYTHON%" scripts\patches.py --out "%out%" --fps "%fps%" --extra "%BB_PATCHES%" --settings "%BB_CONFIG%" --game-dir "%game%" --render-res "%BB_RENDER_RES%" --output-res "%BB_OUTPUT_RES%" --patches-dir "%patches_dir%" --patches-config "%patches_config%"
if errorlevel 1 exit /b 1

REM GPU memory and command processing defaults, as run.sh sets them:
REM BB_PREUPLOAD: background upload of the game's GPU memory into VRAM ahead of use (1 = only
REM   memory already in VRAM that the game rewrote, 2 = all of it, ~3 GB more VRAM, 0 = off).
REM BB_PC_MODEL=1: upstream's new memory and translation model. Upstream 0.5 makes it the default
REM   on AMD GPUs under Linux (run.sh asks bb-gpu-capabilities --device); it needs the game's
REM   direct memory in Vulkan allocations (dma-buf/memfd chunks), which the Windows runtime's
REM   pagefile section cannot provide, so here unset means 0: the 0.3 model with every fix since.
REM   BB_GUEST_IN_PLACE set by hand overrides it.
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
REM GPU caches (shader cache) go to the same user directory: the GPU library reads
REM BB_GPU_USER_DIR when it starts, before --user reaches it (as run.sh).
if "%BB_GPU_USER_DIR%"=="" set "BB_GPU_USER_DIR=%user_dir%"
if not exist "%BB_GPU_USER_DIR%" mkdir "%BB_GPU_USER_DIR%"
REM The Old Hunters: its data ships in patch 1.09 (maps m34-m36), the game only needs the add-on
REM reported as installed. A fresh install gets the entitlement folder automatically when the
REM DLC maps are present and no add-on folder exists yet. BB_AUTO_DLC=0 skips this.
if not "%BB_AUTO_DLC%"=="0" if exist "%game%\dvdroot_ps4\map\m34" if not exist "%user_dir%\addcont" (
    mkdir "%user_dir%\addcont\CUSA03173\SPEXPANSIONDLC03" >nul 2>nul
    echo The Old Hunters: add-on enabled ^("%user_dir%\addcont"^)
)
set "timeout=%BB_TIMEOUT%"
if "%timeout%"=="" set "timeout=0"

REM Party crash recovery: with BB_PARTY set, a non-zero exit (a crash) restarts the game with the
REM same environment after a short delay and BB_PARTY_RESTARTED=1, so it rejoins the party (the
REM host keeps the slot for 60 s). At most BB_PARTY_RESTART_MAX (default 5) restarts per 10 minutes;
REM BB_PARTY_RESTART=0 disables. A BB_TIMEOUT run keeps its overall deadline across restarts.
set "party_restarts=0"
set "party_window=0"
set "run_start=0"
if not "%BB_PARTY%"=="" "%PYTHON%" -c "import time; print(int(time.time()))" > "%out%\party_time.txt"
if not "%BB_PARTY%"=="" for /f "usebackq delims=" %%t in ("%out%\party_time.txt") do set "run_start=%%t"
if "%BB_PARTY_RESTART_MAX%"=="" set "BB_PARTY_RESTART_MAX=5"

:launch
REM A build relinking the exe leaves it empty for a while: never start that (Windows would show a
REM dialog), and never restart into it.
for %%F in ("%probe%") do if %%~zF LSS 1048576 (
    echo [ERROR] "%probe%" is incomplete ^(%%~zF bytes; a build writing it?^); not starting. >&2
    exit /b 1
)
echo Boot: launching at %TIME%
echo Launching Bloodborne PC (%probe%)...
REM BB_RELAUNCH_LOOP=1: the game may end with exit code 75 to be started again (the party flow:
REM System ^> Party in the game's menu, docs\party\ingame_flow.md). It leaves the new environment in
REM %user_dir%\party_relaunch.bat (party mode on/off, role, Continue on its own).
set "BB_RELAUNCH_LOOP=1"
"%probe%" "%out%\boot-linked.bin" --content-profile "%out%\content.bin" --patches "%out%\patches.bin" --app0 "%game%" --user "%user_dir%" --timeout "%timeout%" %EXTRA_ARGS%
set "rc=%ERRORLEVEL%"
if "%rc%"=="75" goto party_relaunch
if "%BB_PARTY%"=="" exit /b %rc%
if "%rc%"=="0" exit /b 0
if "%BB_PARTY_RESTART%"=="0" exit /b %rc%
REM Launch failures (access denied, bad image, not found) are not crashes: no restart.
for %%c in (5 193 216 9009) do if "%rc%"=="%%c" exit /b %rc%
set "now=0"
"%PYTHON%" -c "import time; print(int(time.time()))" > "%out%\party_time.txt"
for /f "usebackq delims=" %%t in ("%out%\party_time.txt") do set "now=%%t"
set /a "since_window=now-party_window"
if %since_window% GTR 600 (
    set "party_window=%now%"
    set "party_restarts=0"
)
set /a "party_restarts+=1"
if %party_restarts% GTR %BB_PARTY_RESTART_MAX% (
    echo Party: the game exited with %rc% again; %BB_PARTY_RESTART_MAX% restarts in 10 minutes, giving up. >&2
    exit /b %rc%
)
if not "%timeout%"=="0" if not "%run_start%"=="0" set /a "timeout=BB_TIMEOUT-(now-run_start)"
if not "%timeout%"=="0" if %timeout% LSS 10 (
    echo Party: the game exited with %rc%; no time left in BB_TIMEOUT, not restarting.
    exit /b %rc%
)
echo Party: the game exited with %rc% ^(a crash^); restart %party_restarts% of %BB_PARTY_RESTART_MAX% in 1 s to rejoin the party
set "BB_PARTY_RESTARTED=1"
"%PYTHON%" -c "import time; time.sleep(1)"
goto launch

:party_relaunch
REM The party flow ended the game to start it again in party mode, in the other role or back in
REM single-player (System > Party in the game's menu). Its environment, then the patches again.
if exist "%user_dir%\party_relaunch.bat" (
    call "%user_dir%\party_relaunch.bat"
    del "%user_dir%\party_relaunch.bat" >nul 2>nul
) else (
    echo Party: exit code 75 without "%user_dir%\party_relaunch.bat"; starting again as before
)
if "%BB_PARTY%"=="" (echo Party: starting the single-player game) else (echo Party: starting the party game ^(%BB_PARTY%^))
goto prepare_patches
