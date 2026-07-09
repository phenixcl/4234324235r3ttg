#include <helpers/foobar2000+atl.h>
#include "yandex_api.hpp"

DECLARE_COMPONENT_VERSION(
    "Yandex Music Streamer",
    "1.0",
    "Streaming HQ audio from Yandex.Music"
);

static const GUID guid_yandex_token = { 0x5a1b32d1, 0x9b4c, 0x4f12, { 0x8a, 0x11, 0xd9, 0xc1, 0x12, 0xa3, 0x4b, 0x77 } };
advconfig_string_factory cfg_yandex_token("Yandex Music OAuth Token", guid_yandex_token, advconfig_branch::guid_branch_tools, 0, "");

class yandex_filesystem : public filesystem {
public:
    bool get_canonical_path(const char * p_path, pfc::string_base & p_out) override {
        p_out = p_path;
        return true;
    }
    
    bool is_our_path(const char * p_path) override {
        return strncmp(p_path, "yandex://", 9) == 0;
    }
    
    bool get_display_path(const char * p_path, pfc::string_base & p_out) override {
        p_out = p_path;
        return true;
    }
    
    void open(service_ptr_t<file> & p_out, const char * p_path, t_open_mode p_mode, abort_callback & p_abort) override {
        if (p_mode != open_mode_read) throw exception_io_denied();
        
        pfc::string8 track_id = p_path + 9;
        pfc::string8 token;
        cfg_yandex_token.get(token);
        
        if (token.is_empty()) {
            throw exception_io("Yandex Music token is not set in Advanced Preferences");
        }
        
        std::string real_url = YandexAPI::GetDirectTrackUrl(track_id.c_str(), token.c_str());
        if (real_url.empty()) {
            throw exception_io_not_found();
        }
        
        filesystem::g_open(p_out, real_url.c_str(), p_mode, p_abort);
    }
    
    void remove(const char * p_path, abort_callback & p_abort) override { throw exception_io_denied(); }
    void move(const char * p_src, const char * p_dst, abort_callback & p_abort) override { throw exception_io_denied(); }
    bool is_remote(const char * p_path) override { return true; }
    bool get_relative_path(const char * p_path, const char * p_base, pfc::string_base & p_out) override { return false; }
    t_filestats get_stats(const char * p_path, abort_callback & p_abort) override {
        return filestats_invalid;
    }
    void abort() override {}
    bool supports_content_types() override { return true; }
};

static service_factory_single_t<yandex_filesystem> g_yandex_filesystem_factory;
