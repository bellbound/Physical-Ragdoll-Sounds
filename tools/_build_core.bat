@echo off
rem Scratch helper: ninja with MSVC on PATH. vcvars64 is not on PATH here, so a
rem bare `ninja` fails at `cl` not being found. Safe to delete.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "C:\games\skyrim\papyrus\reference\SpellWheelVR 1.5.5 Source\skse\Physical-Ragoll-Sounds\testbench\build\testbench"
ninja %*
