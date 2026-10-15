@echo off
echo Building Windows Sound Tracker in Release mode...
echo.

:: Uses the newest installed Visual Studio (2022 or later).
cmake -S . -B build_release -A x64
if errorlevel 1 (
    echo CMake configuration failed!
    pause
    exit /b 1
)

echo.
echo Building project...
cmake --build build_release --config Release --parallel
if errorlevel 1 (
    echo Build failed!
    pause
    exit /b 1
)

echo.
echo Running unit tests...
ctest --test-dir build_release -C Release --output-on-failure
if errorlevel 1 (
    echo Unit tests failed!
    pause
    exit /b 1
)

echo.
echo Build successful!
echo   build_release\bin\Release\SoundTracker.exe
echo   build_release\bin\Release\SoundBeacon.exe  (test tool)
echo.
pause
