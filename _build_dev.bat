@echo off
REM Leaves cores free and runs at low priority, so the machine stays usable
REM while it builds. Roughly 20 percent slower, and you can keep working.
REM Raise JOBS if you want it faster and do not mind the slowdown.
set JOBS=5
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
if errorlevel 1 (echo VSDEV FAILED & exit /b 1)
cmake -B build-dev -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DUSE_DX9=OFF -DUSE_DX11=OFF
if errorlevel 1 (echo CMAKE CONFIGURE FAILED & exit /b 1)
REM Delete first, so "it exists" cannot be satisfied by a stale binary from a previous run.
if exist build-dev\flycast.exe del build-dev\flycast.exe
start /LOW /WAIT /B cmake --build build-dev --target flycast -- -j %JOBS%
REM `start /WAIT` does NOT propagate the exit code - a failed link exits 0 and looks like a
REM successful build. The existence of a FRESH exe is what decides.
if not exist build-dev\flycast.exe (echo BUILD FAILED: no flycast.exe was produced & exit /b 1)
echo BUILT: build-dev\flycast.exe
