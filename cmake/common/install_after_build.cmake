# Local post-build deployment helpers.

include_guard(GLOBAL)

function(atk_configure_local_post_build_deploy target_name plugin_name)
    if(
        ATK_ENABLE_LOCAL_POST_BUILD_INSTALL
        AND NOT DEFINED
            ENV{CI}
        AND NOT DEFINED
            ENV{GITHUB_ACTIONS}
        AND NOT CMAKE_INSTALL_PREFIX
            MATCHES
            "^/usr"
    )
        add_custom_command(
            TARGET ${target_name}
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG> --component plugin
            COMMENT "Installing plugin after build..."
        )
    endif()

    if(
        ATK_ENABLE_LINUX_HOME_POST_BUILD_COPY
        AND NOT DEFINED
            ENV{CI}
        AND NOT DEFINED
            ENV{GITHUB_ACTIONS}
        AND UNIX
        AND NOT APPLE
    )
        message(STATUS "Configuring post-build copy to ~/.config/obs-studio/plugins/")
        atk_get_obs_arch_dir(_user_arch)

        add_custom_command(
            TARGET ${target_name}
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E make_directory
                "$ENV{HOME}/.config/obs-studio/plugins/${plugin_name}/bin/${_user_arch}"
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target_name}>"
                "$ENV{HOME}/.config/obs-studio/plugins/${plugin_name}/bin/${_user_arch}/"
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target_name}_scanner>"
                "$ENV{HOME}/.config/obs-studio/plugins/${plugin_name}/bin/${_user_arch}/"
            COMMAND
                ${CMAKE_COMMAND} -E make_directory "$ENV{HOME}/.config/obs-studio/plugins/${plugin_name}/data"
            COMMAND
                ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data"
                "$ENV{HOME}/.config/obs-studio/plugins/${plugin_name}/data"
            COMMENT "Copying ${plugin_name} and scanner to user home OBS plugins directory..."
            VERBATIM
        )
    endif()
endfunction()
