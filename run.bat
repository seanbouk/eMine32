@echo off
rem Launch eMine32 in a window, using local dev data from devdata\
rem   run.bat            -> title screen
rem   run.bat E1L1.MAP   -> straight into a map
setlocal
set ROOT=%~dp0
if not exist "%ROOT%run" mkdir "%ROOT%run"
pushd "%ROOT%run"
if "%~1"=="" (
  "%ROOT%engine\eduke32.exe" -nosetup -usecwd -j "%ROOT%devdata\duke3d-sw" -j "%ROOT%game" -mh emine32.def -gamegrp DUKE3D.GRP
) else (
  "%ROOT%engine\eduke32.exe" -nosetup -usecwd -j "%ROOT%devdata\duke3d-sw" -j "%ROOT%game" -mh emine32.def -gamegrp DUKE3D.GRP -map %1 -s1
)
popd
