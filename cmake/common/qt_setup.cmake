# Qt setup helpers for platform-specific dependency layouts.

include_guard(GLOBAL)

function(atk_configure_qt_paths qt6_version deps_dir)
    if(NOT WIN32)
        return()
    endif()

    if(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
        set(_atk_qt_target_arch "ARM64")
        set(_atk_qt_host_arch "x64")
    else()
        set(_atk_qt_target_arch "x64")
        set(_atk_qt_host_arch "x64")
    endif()

    set(_atk_qt_root
        "${CMAKE_SOURCE_DIR}/_deps/${_atk_qt_target_arch}/obs-deps-qt6-${qt6_version}-${_atk_qt_target_arch}"
    )
    if(
        NOT EXISTS
            "${_atk_qt_root}/lib/cmake/Qt6/Qt6Config.cmake"
        AND EXISTS
            "${deps_dir}/obs-deps-qt6-${qt6_version}-${_atk_qt_target_arch}/lib/cmake/Qt6/Qt6Config.cmake"
    )
        set(_atk_qt_root "${deps_dir}/obs-deps-qt6-${qt6_version}-${_atk_qt_target_arch}")
    endif()

    set(Qt6_DIR "${_atk_qt_root}/lib/cmake/Qt6" CACHE STRING "Qt6 CMake directory" FORCE)
    message(STATUS "Using Qt6 package from: ${Qt6_DIR}")

    if(NOT CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
        return()
    endif()

    # ARM64 cross-compilation requires host Qt tools from the x64 Qt package.
    set(QT_HOST_PATH "${CMAKE_SOURCE_DIR}/_deps/${_atk_qt_host_arch}/obs-deps-qt6-${qt6_version}-${_atk_qt_host_arch}")

    message(STATUS "ARM64 cross-compilation detected")
    message(STATUS "QT_HOST_PATH: ${QT_HOST_PATH}")

    if(NOT EXISTS "${QT_HOST_PATH}/bin/moc.exe")
        message(
            FATAL_ERROR
            "x64 Qt host tools not found at: ${QT_HOST_PATH}. ARM64 cross-compilation requires both x64 and ARM64 Qt dependencies."
        )
    endif()

    set(Qt6CoreTools_DIR "${QT_HOST_PATH}/lib/cmake/Qt6CoreTools" CACHE STRING "Qt6 x64 Core Tools" FORCE)
    set(Qt6GuiTools_DIR "${QT_HOST_PATH}/lib/cmake/Qt6GuiTools" CACHE STRING "Qt6 x64 GUI Tools" FORCE)
    set(Qt6WidgetsTools_DIR "${QT_HOST_PATH}/lib/cmake/Qt6WidgetsTools" CACHE STRING "Qt6 x64 Widget Tools" FORCE)

    set(CMAKE_AUTOMOC_MOC_EXECUTABLE
        "${QT_HOST_PATH}/bin/moc.exe"
        CACHE FILEPATH
        "MOC executable for ARM64 cross-compilation"
        FORCE
    )
    set(CMAKE_AUTOUIC_UIC_EXECUTABLE
        "${QT_HOST_PATH}/bin/uic.exe"
        CACHE FILEPATH
        "UIC executable for ARM64 cross-compilation"
        FORCE
    )
    set(CMAKE_AUTORCC_RCC_EXECUTABLE
        "${QT_HOST_PATH}/bin/rcc.exe"
        CACHE FILEPATH
        "RCC executable for ARM64 cross-compilation"
        FORCE
    )

    message(STATUS "Set CMAKE_AUTOMOC_MOC_EXECUTABLE to: ${CMAKE_AUTOMOC_MOC_EXECUTABLE}")
endfunction()
