set_property(
    DIRECTORY
    APPEND
    PROPERTY
        CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/buildspec.json"
)
set(ATK_PLUGIN_MANIFEST "${CMAKE_BINARY_DIR}/plugin-resources/manifest.json")
configure_file("${CMAKE_CURRENT_LIST_DIR}/manifest.json.in" "${ATK_PLUGIN_MANIFEST}" @ONLY)

if(APPLE)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${ATK_PLUGIN_MANIFEST}")
    set_source_files_properties(
        "${ATK_PLUGIN_MANIFEST}"
        PROPERTIES
            MACOSX_PACKAGE_LOCATION
                Resources
    )
endif()
