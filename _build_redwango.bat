@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
if errorlevel 1 (echo VSDEV FAILED & exit /b 1)
cmake -B build-redwango -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DUSE_DX9=OFF -DUSE_DX11=OFF
if errorlevel 1 (echo CMAKE CONFIGURE FAILED & exit /b 1)
cmake --build build-redwango --target flycast
