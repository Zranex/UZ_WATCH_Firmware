idf_component_register(
    SRCS "main.cpp" "user_wallpaper.c" "app_lockscreen.cpp" "lockscreen_img.c" "font_cinzel_bold_160.c" "font_cinzel_bold_54.c" "font_cinzel_bold_36.c" "app_calibration.cpp" "app_media_player.cpp" "app_weather.cpp" "app_notifications_custom.cpp" "ui_notification_banner.cpp" "app_activity.cpp" "app_smart_home.cpp" "app_local_music.cpp" "app_voice_recorder.cpp" "app_find_phone.cpp" "app_call_manager.cpp" "app_flashlight.cpp" "icon_activity.c" "icon_smarthome.c" "icon_flashlight.c" "icon_calibration.c" "icon_local_music.c" "icon_notifications.c" "icon_voice_recorder.c" "icon_find_phone.c" "icon_media_player.c" "icon_weather.c"
    INCLUDE_DIRS "."
    REQUIRES display_manager bsp_extra brookesia_app_calculator brookesia_app_game_2048 brookesia_app_settings app_watchface
    )

target_compile_options(${COMPONENT_LIB} PUBLIC -Wno-missing-field-initializers)

if(CONFIG_ESP_BROOKESIA_MEMORY_USE_CUSTOM)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "brookesia_core" OR COMPONENT MATCHES "espressif__brookesia_core")
            set(TARGET_COMPONENT ${COMPONENT})
            break()
        endif()
    endforeach()

    if(TARGET_COMPONENT STREQUAL "")
        message(FATAL_ERROR "Component 'brookesia_core' not found.")
    else()
        idf_component_get_property(ESP_BROOKESIA_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
    endif()

    target_compile_options(
        ${ESP_BROOKESIA_LIB}
        PUBLIC
            "-DESP_BROOKESIA_MEMORY_CUSTOM_MALLOC(x)=heap_caps_aligned_alloc(1, x, MALLOC_CAP_SPIRAM)"  "-DESP_BROOKESIA_MEMORY_CUSTOM_FREE=free"
    )
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions")

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
