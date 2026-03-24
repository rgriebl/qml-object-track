cmake_minimum_required(VERSION 3.16)
project(qml-object-track
    VERSION     1.0.0
    LANGUAGES   C CXX
    DESCRIPTION "Qml component creation heap mem track"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS
    Core
)

qt_standard_project_setup(REQUIRES 6.8)

add_library(qml-object-track SHARED
    src/objecttrack.cpp
)

target_link_libraries(qml-object-track PRIVATE
    Qt::Core
    dl
)

# --------------------------------------------------------------------------
# Install
# --------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS qml-object-track
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# --------------------------------------------------------------------------
# Usage hint
# --------------------------------------------------------------------------
message(STATUS "Build with:  cmake -B build && cmake --build build")
message(STATUS "Inject with: LD_PRELOAD=\${CMAKE_BINARY_DIR}/libqml-object-track.so ./your-qt-app")
