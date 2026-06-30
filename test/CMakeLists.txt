find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Qml)

# Auto-detect the patched QQmlEngine::trimMemory() (guard-pool compaction).
# When building against the patched Qt it's defined; against stock Qt the
# trimEngine() call compiles out to a no-op.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES Qt6::Qml)
check_cxx_source_compiles("
#include <QQmlEngine>
int main() { QQmlEngine e; e.trimMemory(); return 0; }
" HAVE_QQMLENGINE_TRIMMEMORY)
unset(CMAKE_REQUIRED_LIBRARIES)

qt_add_executable(qml-object-track-test
    main.cpp
)

qt_add_qml_module(qml-object-track-test
    URI ObjectTrackTest
    VERSION 1.0
    QML_FILES
        Main.qml
        listview.qml
        repeater.qml
    SOURCES
        memoryhelper.h
)

target_link_libraries(qml-object-track-test PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
)

if(HAVE_QQMLENGINE_TRIMMEMORY)
    target_compile_definitions(qml-object-track-test PRIVATE OBJECTTRACK_HAVE_TRIMMEMORY)
    message(STATUS "QQmlEngine::trimMemory() found -> guard-pool compaction ENABLED")
else()
    message(STATUS "QQmlEngine::trimMemory() not found (stock Qt) -> trimEngine() is a no-op")
endif()

message(STATUS "Run test with:")
message(STATUS "  LD_PRELOAD=${CMAKE_BINARY_DIR}/libqml-object-track.so ${CMAKE_CURRENT_BINARY_DIR}/qml-object-track-test")
