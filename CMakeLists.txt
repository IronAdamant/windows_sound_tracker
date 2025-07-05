cmake_minimum_required(VERSION 3.20)
project(WindowsSoundTracker)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set Windows specific flags
if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
    
    # Force static runtime linking for standalone executable
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
    
    # Remove any default runtime library flags
    string(REGEX REPLACE "/MD" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/MDd" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REGEX REPLACE "/MD" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    
    # Explicitly set static runtime flags
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
endif()

# GUI version only

# Common source files
set(COMMON_SOURCES
    src/SoundTracker.cpp
    src/Logger.cpp
)

# Common header files
set(COMMON_HEADERS
    include/AudioEvent.h
    include/SoundTracker.h
    include/Logger.h
)

# GUI version
set(SOURCES
    src/main_gui.cpp
    src/SoundTrackerGUI.cpp
    ${COMMON_SOURCES}
)

set(HEADERS
    include/SoundTrackerGUI.h
    ${COMMON_HEADERS}
)

# Add resource file
set(RESOURCES
    SoundTracker.rc
    resource.h
)

# Create GUI executable
add_executable(SoundTracker WIN32 ${SOURCES} ${HEADERS} ${RESOURCES})

# GUI-specific libraries
target_link_libraries(SoundTracker
    comctl32
    comdlg32
    shell32
)

# Include directories
target_include_directories(SoundTracker PRIVATE include)

# Common Windows libraries
if(WIN32)
    target_link_libraries(SoundTracker
        ole32
        winmm
        psapi
        kernel32
        user32
        gdi32
        winspool
        advapi32
        uuid
        oleaut32
    )
    
    # Add manifest file for admin privileges
    if(MSVC)
        set_target_properties(SoundTracker PROPERTIES
            LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\" /SUBSYSTEM:WINDOWS"
        )
        
        # Embed manifest
        add_custom_command(TARGET SoundTracker POST_BUILD
            COMMAND mt.exe -manifest "${CMAKE_CURRENT_SOURCE_DIR}/SoundTracker.manifest" 
                           -outputresource:"$<TARGET_FILE:SoundTracker>" -nologo
            COMMENT "Embedding manifest for admin privileges"
        )
    endif()
endif()

# Set output directory
set_target_properties(SoundTracker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)