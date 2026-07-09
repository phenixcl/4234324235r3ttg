#include <helpers/foobar2000+atl.h>
#include "yandex_api.hpp"

DECLARE_COMPONENT_VERSION(
    "Yandex Music Streamer",
    "1.0",
    "Streaming HQ audio from Yandex.Music"
);

static const GUID guid_yandex_token = { 0x5a1b32d1, 0x9b4c, 0x4f12, { 0x8a, 0x11, 0xd9, 0xc1, 0x12, 0xa3, 0x4b, 0x77 } };
advconfig_string_factory cfg_yandex_token("Yandex Music OAuth Token", guid_yandex_token, advconfig_branch::guid_branch_tools, 0, "");


extern void force_link_yandex_filesystem();
extern void force_link_yandex_ui();

class yandex_initquit : public initquit {
public:
    void on_init() override {
        force_link_yandex_filesystem();
        force_link_yandex_ui();
    }
    void on_quit() override {}
};
static initquit_factory_t<yandex_initquit> g_yandex_initquit_factory;
