#include <helpers/foobar2000+atl.h>
#include "yandex_api.hpp"

DECLARE_COMPONENT_VERSION(
    "Yandex Music Streamer",
    "1.0",
    "Streaming HQ audio from Yandex.Music"
);

static const GUID guid_yandex_token = { 0x5a1b32d1, 0x9b4c, 0x4f12, { 0x8a, 0x11, 0xd9, 0xc1, 0x12, 0xa3, 0x4b, 0x77 } };
advconfig_string_factory cfg_yandex_token("Yandex Music OAuth Token", guid_yandex_token, advconfig_branch::guid_branch_tools, 0, "");


