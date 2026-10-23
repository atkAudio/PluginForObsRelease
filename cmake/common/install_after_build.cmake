# Local post-build deployment helpers.

include_guard(GLOBAL)

function(atk_configure_local_post_build_deploy target_name plugin_name)
    # Local builds must always land in the OBS plugins directory, otherwise OBS keeps
    # loading a stale binary. On Linux the system prefix (/usr) requires root, so skip
    # this step there in favor of the unconditional user-home copy below.
    if(NOT DEFINED ENV{CI} AND NOT DEFINED ENV{GITHUB_ACTIONS} AND NOT (UNIX AND NOT APPLE))
        message(STATUS "Configuring post-build install to ${CMAKE_INSTALL_PREFIX}")

        add_custom_command(
            TARGET ${target_name}
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG> --component plugin
            COMMENT "Installing plugin after build..."
        )
    endif()

    if(NOT DEFINED ENV{CI} AND NOT DEFINED ENV{GITHUB_ACTIONS} AND UNIX AND NOT APPLE)
        if(DEFINED ENV{XDG_DATA_HOME} AND NOT "$ENV{XDG_DATA_HOME}" STREQUAL "")
            set(_user_data_home "$ENV{XDG_DATA_HOME}")
        else()
            set(_user_data_home "$ENV{HOME}/.local/share")
        endif()
        if(NOT IS_ABSOLUTE "${_user_data_home}")
            message(FATAL_ERROR "OBS plugin deployment requires an absolute XDG data directory")
        endif()
        set(_user_plugin "${_user_data_home}/obs-studio/plugins/${plugin_name}")
        message(STATUS "Configuring post-build copy to ${_user_plugin}")

        add_custom_command(
            TARGET ${target_name}
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E make_directory "${_user_plugin}"
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target_name}>"
                "$<TARGET_FILE:${target_name}_scanner>" "${_user_plugin}"
            COMMAND
                ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "${_user_plugin}/data"
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different "${ATK_PLUGIN_MANIFEST}" "${_user_plugin}/data/manifest.json"
            COMMENT "Installing ${plugin_name} to the OBS 33 user plugin directory..."
            VERBATIM
        )
    endif()
endfunction()
