@echo off
echo Building Windows Sound Tracker in Release mode...
echo.

:: Create release build directory
if not exist "build_release" mkdir build_release
cd build_release

:: Configure CMake for Release
echo Configuring CMake...
cmake .. -DCMAKE_BUILD_TYPE=Release -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo CMake configuration failed!
    pause
    exit /b 1
)

:: Build the project
echo.
echo Building project...
cmake --build . --config Release
if errorlevel 1 (
    echo Build failed!
    pause
    exit /b 1
)

echo.
echo Build successful!
echo Release executable location: build_release\bin\Release\SoundTracker.exe
echo.
pause