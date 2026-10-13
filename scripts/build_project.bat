@echo off
setlocal EnableExtensions

rem -----------------------------------------------------------------------------
rem Rebuild RFSoC_COM with Vitis 2022.2 / XSCT
rem -----------------------------------------------------------------------------

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"
rem Keep the generated workspace SHORT. Vitis 2022.2 bundles an old CMake
rem whose libmetal build can fail near the Windows 260-character path limit.
rem The repository itself may live anywhere; only generated Vitis output goes here.
if defined RFSOC_WORKSPACE (
    set "WORKSPACE=%RFSOC_WORKSPACE%"
) else (
    set "WORKSPACE=%USERPROFILE%\vws\rfsoc_com"
)
set "TCL_SCRIPT=%SCRIPT_DIR%create_vitis_project.tcl"

if not exist "%TCL_SCRIPT%" (
    echo ERROR: Cannot find:
    echo   %TCL_SCRIPT%
    exit /b 1
)

rem -----------------------------------------------------------------------------
rem Initialize Vitis.  Calling bin\xsct.bat directly from an ordinary cmd.exe
rem is not sufficient on Vitis 2022.2; settings64.bat sets XILINX_VITIS,
rem XILINX_VIVADO, RDI_APPROOT and the tool PATHs used by XSCT.
rem -----------------------------------------------------------------------------

set "VITIS_SETTINGS="

rem If this shell is already configured, use that installation.
if defined XILINX_VITIS if exist "%XILINX_VITIS%\settings64.bat" (
    set "VITIS_SETTINGS=%XILINX_VITIS%\settings64.bat"
)

rem Standard Xilinx/AMD install locations.
if not defined VITIS_SETTINGS if exist "C:\Xilinx\Vitis\2022.2\settings64.bat" (
    set "VITIS_SETTINGS=C:\Xilinx\Vitis\2022.2\settings64.bat"
)
if not defined VITIS_SETTINGS if exist "C:\AMD\Vitis\2022.2\settings64.bat" (
    set "VITIS_SETTINGS=C:\AMD\Vitis\2022.2\settings64.bat"
)

rem Optional override. Example:
rem   set VITIS_2022_2=C:\Xilinx\Vitis\2022.2
rem   scripts\build_project.bat
if not defined VITIS_SETTINGS if defined VITIS_2022_2 if exist "%VITIS_2022_2%\settings64.bat" (
    set "VITIS_SETTINGS=%VITIS_2022_2%\settings64.bat"
)

if defined VITIS_SETTINGS (
    echo Initializing Vitis 2022.2:
    echo   %VITIS_SETTINGS%
    call "%VITIS_SETTINGS%"
    if errorlevel 1 (
        echo ERROR: Vitis environment setup failed.
        exit /b 1
    )
) else (
    rem It is still valid to continue if xsct is already on PATH, for example
    rem when this script is run from an initialized Vitis 2022.2 command shell.
    where xsct.bat >nul 2>nul
    if errorlevel 1 (
        where xsct.exe >nul 2>nul
        if errorlevel 1 (
            echo ERROR: Vitis 2022.2 environment was not found.
            echo.
            echo Expected one of:
            echo   C:\Xilinx\Vitis\2022.2\settings64.bat
            echo   C:\AMD\Vitis\2022.2\settings64.bat
            echo.
            echo If Vitis is installed elsewhere, run:
            echo   set VITIS_2022_2=C:\path\to\Vitis\2022.2
            echo   scripts\build_project.bat
            echo.
            echo Or first run the Vitis settings script manually:
            echo   call C:\path\to\Vitis\2022.2\settings64.bat
            echo   scripts\build_project.bat
            exit /b 1
        )
    )
)

rem -----------------------------------------------------------------------------
rem Verify the initialized tool is actually XSCT 2022.2.
rem -----------------------------------------------------------------------------
where xsct.bat >nul 2>nul
if not errorlevel 1 (
    set "XSCT_CMD=xsct.bat"
) else (
    where xsct.exe >nul 2>nul
    if errorlevel 1 (
        echo ERROR: xsct is still not available after Vitis initialization.
        exit /b 1
    )
    set "XSCT_CMD=xsct.exe"
)

echo.
echo Repository : %REPO_ROOT%
echo Workspace  : %WORKSPACE%
echo XSCT       : %XSCT_CMD%
echo.

rem CALL is intentional because xsct.bat is itself a batch file on Windows.
call %XSCT_CMD% "%TCL_SCRIPT%" "%WORKSPACE%"
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
    echo.
    echo ERROR: RFSoC_COM rebuild failed with exit code %RC%.
    exit /b %RC%
)

echo.
echo ============================================================
echo Build complete.
echo Workspace:
echo   %WORKSPACE%
echo Expected ELF:
echo   %WORKSPACE%\RFSoC_COM\Debug\RFSoC_COM.elf
echo ============================================================

endlocal
exit /b 0
