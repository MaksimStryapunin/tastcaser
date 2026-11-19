cmake_minimum_required(VERSION 3.16)

project(ActionRecorder VERSION 0.0.2 LANGUAGES CXX)

add_executable(ActionRecorder
    main.cpp
)

target_compile_features(ActionRecorder PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(ActionRecorder PRIVATE
        /utf-8
        /source-charset:utf-8
        /execution-charset:utf-8
        /DUNICODE
        /D_UNICODE
    )
else()
    target_compile_options(ActionRecorder PRIVATE -municode)
endif()

if(WIN32)
    target_link_libraries(ActionRecorder PRIVATE ole32 oleaut32 user32)
endif()
