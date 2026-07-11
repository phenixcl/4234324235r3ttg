#include "foobar2000/SDK/foobar2000.h"
#include <string>

// =====================================================
// Filesystem: makes yandex:// URLs valid in playlists.
// Does NOT open files – the input handler does that.
// =====================================================

class yandex_filesystem : public filesystem {
public:
    bool get_canonical_path(const char * path, pfc::string_base & out) override {
        out = path;
        return true;
    }

    bool is_our_path(const char * path) override {
        return strncmp(path, "yandex://", 9) == 0;
    }

    bool get_display_path(const char * path, pfc::string_base & out) override {
        out = path;
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
        // Throw exception_io_no_handler_for_path so that default decoders (like MP3)
        // silently ignore "yandex://...mp3" paths without causing a fatal playback error.
        // This allows foobar2000 to continue iterating its input handlers until it reaches
        // our native yandex_input which handles the path directly.
        throw exception_io_no_handler_for_path();
    }
};

static service_factory_single_t<yandex_filesystem> g_yandex_filesystem_factory;

void force_link_yandex_filesystem() {}
