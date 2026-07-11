#include "foobar2000/SDK/foobar2000.h"
#include <string>

extern std::string resolve_yandex_track_url(const std::string& id_str, const std::wstring& wtoken_wide);
extern cfg_string cfg_yandex_token;

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

    void remove(const char * p_path, abort_callback & p_abort) override { throw exception_io_denied(); }
    void move(const char * p_src, const char * p_dst, abort_callback & p_abort) override { throw exception_io_denied(); }
    bool is_remote(const char * p_src) override { return true; }
    
    bool relative_path_create(const char * file_path, const char * playlist_path, pfc::string_base & out) override { return false; }
    bool relative_path_parse(const char * relative_path, const char * playlist_path, pfc::string_base & out) override { return false; }
    
    void get_stats(const char * p_path, t_filestats & p_stats, bool & p_is_writeable, abort_callback & p_abort) override {
        p_stats.m_size = filesize_invalid;
        p_stats.m_timestamp = filetimestamp_invalid;
        p_is_writeable = false;
    }

    void create_directory(const char * p_path, abort_callback & p_abort) override {
        throw exception_io_denied();
    }

    void list_directory(const char * p_path, directory_callback & p_callback, abort_callback & p_abort) override {
        throw exception_io_not_found();
    }

    bool supports_content_types() override { return false; }

    void open(service_ptr_t<file> & p_out, const char * p_path, t_open_mode p_mode, abort_callback & p_abort) override {
        if (p_mode != open_mode_read) throw exception_io_denied();
        
        std::string path_str = p_path;
        if (path_str.find("yandex://track/") != 0) throw exception_io_not_found();
        
        std::string id_str = path_str.substr(15);
        size_t dot_pos = id_str.find('.');
        if (dot_pos != std::string::npos) id_str = id_str.substr(0, dot_pos);

        std::string wtoken = cfg_yandex_token.get_ptr();
        std::wstring wtoken_wide(pfc::stringcvt::string_wide_from_utf8(wtoken.c_str()).get_ptr());

        std::string direct_url = resolve_yandex_track_url(id_str, wtoken_wide);
        if (direct_url.empty()) throw exception_io_not_found();

        filesystem::g_open(p_out, direct_url.c_str(), p_mode, p_abort);
    }
};

static service_factory_single_t<yandex_filesystem> g_yandex_filesystem_factory;

void force_link_yandex_filesystem() {}
