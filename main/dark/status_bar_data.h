/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "systems/phone/widgets/status_bar/esp_brookesia_status_bar.hpp"
#include "systems/phone/assets/esp_brookesia_phone_assets.h"

namespace esp_brookesia::systems::phone {

constexpr StatusBar::AreaData STYLESHEET_410_502_DARK_STATUS_BAR_AREA_DATA(int w_percent, StatusBar::AreaAlign align, int pad_offset)
{
    return {
        .size = gui::StyleSize::RECT_PERCENT(w_percent, 100),
        .layout_column_align = align,
        .layout_column_start_offset = pad_offset,
        .layout_column_pad = 6,
    };
}

constexpr StatusBar::Data STYLESHEET_410_502_DARK_STATUS_BAR_DATA = {
    .main = {
        .size = gui::StyleSize::RECT_W_PERCENT(100, 36),
        .background_color = gui::StyleColor::COLOR(0x000000),
        .text_font = gui::StyleFont::SIZE(18),
        .text_color = gui::StyleColor::COLOR(0xFFFFFF),
    },
    .area = {
        .num = 2,
        .data = {
            STYLESHEET_410_502_DARK_STATUS_BAR_AREA_DATA(50, StatusBar::AreaAlign::START, 55), // Left: Clock (deep inside flat display area)
            STYLESHEET_410_502_DARK_STATUS_BAR_AREA_DATA(50, StatusBar::AreaAlign::END, 70),   // Right: Wi-Fi & Battery (deep inside flat display area)
        },
    },
    .icon_common_size = gui::StyleSize::SQUARE(22),
    .battery = {
        .area_index = 1,
        .icon_data = {
            .icon = {
                .image_num = 5,
                .images = {
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_battery_level1_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_battery_level2_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_battery_level3_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_battery_level4_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_battery_charge_24_24),
                },
            },
        },
    },
    .wifi = {
        .area_index = 1,
        .icon_data = {
            .icon = {
                .image_num = 4,
                .images = {
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_wifi_close_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_wifi_level1_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_wifi_level2_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_wifi_level3_24_24),
                },
            },
        },
    },
    .clock = {
        .area_index = 0,
    },
    .flags = {
        .enable_battery_icon = 1,
        .enable_battery_icon_common_size = 1,
        .enable_battery_label = 1,
        .enable_wifi_icon = 1,
        .enable_wifi_icon_common_size = 1,
        .enable_clock = 1,
    },
};

} // namespace esp_brookesia::systems::phone
