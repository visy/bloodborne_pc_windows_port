@echo off
setlocal
cd /d "%~dp0"

set "PYTHON_EXE="
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
    if not defined PYTHON_EXE if exist %%P set "PYTHON_EXE=%%~P"
)

rem Any python.exe on PATH that really runs Python 3.10+. This covers the Microsoft Store install
rem (%LocalAppData%\Microsoft\WindowsApps\python.exe is an app execution alias, not a real file)
rem and rejects the Store's placeholder alias that exists when Python is not installed.
if not defined PYTHON_EXE (
    for /f "delims=" %%I in ('where python.exe 2^>nul') do (
        if not defined PYTHON_EXE (
            "%%~I" -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>nul
            if not errorlevel 1 set "PYTHON_EXE=%%~I"
        )
    )
)

if not defined PYTHON_EXE (
    echo [ERROR] Python 3.10+ not found. Install it from python.org or the Microsoft Store.
    pause
    exit /b 1
)

start "" "%PYTHON_EXE%" launcher.py
