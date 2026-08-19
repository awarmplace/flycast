@echo off
REM Baseline build: the serial bridge is KEPT (needed to reach the online menu),
REM but core/hw/sh4/interpr/sh4_interpreter.cpp is reverted to upstream, so the
REM odd-address check is gated on !mmu_enabled() again. One variable.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
if errorlevel 1 (echo VSDEV FAILED & exit /b 1)
cmake -B build-baseline -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DUSE_DX9=OFF -DUSE_DX11=OFF
if errorlevel 1 (echo CMAKE CONFIGURE FAILED & exit /b 1)
cmake --build build-baseline --target flycast
