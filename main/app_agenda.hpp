#pragma once
#include "esp_brookesia.hpp"
#include <vector>
#include <string>

using namespace esp_brookesia;

class AppAgenda : public esp_brookesia::systems::phone::App {
public:
    AppAgenda() : esp_brookesia::systems::phone::App("Ajanda & Gorevler", nullptr, true) {
    }
    virtual ~AppAgenda() {}
    
    virtual bool run() override;
    virtual bool back() override;
    virtual bool close() override;
    virtual bool init() override;

    void add_item(int id, const char* text);
    void clear_items();
    void render_items();

private:
    lv_obj_t* _bg_obj = nullptr;
    lv_obj_t* _list = nullptr;
    lv_obj_t* _title_label = nullptr;
    
    struct AgendaItem {
        int id;
        std::string text;
        bool is_task;
    };
    std::vector<AgendaItem> _items;
};

extern "C" void app_agenda_update_from_ble(const char* payload);
extern "C" void app_agenda_clear();
extern "C" void app_agenda_add_item(int id, const char* text);
