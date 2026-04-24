find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2)

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

message(STATUS "Run test with:")
message(STATUS "  LD_PRELOAD=${CMAKE_BINARY_DIR}/libqml-object-track.so ${CMAKE_CURRENT_BINARY_DIR}/qml-object-track-test")
