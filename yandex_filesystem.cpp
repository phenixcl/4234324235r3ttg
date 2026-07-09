#include "stdafx.h"
#include <foobar2000/SDK/foobar2000.h>
#include <nlohmann/json.hpp>
#include "yandex_api.hpp"

extern cfg_string cfg_yandex_token;
extern cfg_bool cfg_yandex_hq;

class YandexFileWrapper : public file_dynamicinfo {
public:
    YandexFileWrapper(file::ptr underlying, const std::string& title, const std::string& artist, const std::string& album, double duration)
        : m_file(underlying), m_title(title), m_artist(artist), m_album(album), m_duration(duration) {}

    t_size read(void* p_buffer, t_size p_bytes, abort_callback& p_abort) override { return m_file->read(p_buffer, p_bytes, p_abort); }
    void write(const void* p_buffer, t_size p_bytes, abort_callback& p_abort) override { m_file->write(p_buffer, p_bytes, p_abort); }
    t_filesize get_size(abort_callback& p_abort) override { return m_file->get_size(p_abort); }
    t_filesize get_position(abort_callback& p_abort) override { return m_file->get_position(p_abort); }
    void resize(t_filesize p_size, abort_callback& p_abort) override { m_file->resize(p_size, p_abort); }
    void seek(t_filesize p_position, abort_callback& p_abort) override { m_file->seek(p_position, p_abort); }
    bool can_seek() override { return m_file->can_seek(); }
    bool get_content_type(pfc::string_base& p_out) override { return m_file->get_content_type(p_out); }
    void reopen(abort_callback& p_abort) override { m_file->reopen(p_abort); }
    bool is_remote() override { return m_file->is_remote(); }

    bool get_static_info(file_info& p_out) override {
        if (!m_title.empty()) p_out.meta_set("TITLE", m_title.c_str());
        if (!m_artist.empty()) p_out.meta_set("ARTIST", m_artist.c_str());
        if (!m_album.empty()) p_out.meta_set("ALBUM", m_album.c_str());
        if (m_duration > 0) p_out.set_length(m_duration);
        return true;
    }

    bool is_dynamic_info_enabled() override { return false; }
    bool get_dynamic_info(file_info& p_out) override { return false; }

private:
    file::ptr m_file;
    std::string m_title;
    std::string m_artist;
    std::string m_album;
    double m_duration;
};

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
    
    bool supports_content_types() override { return true; }
    void open(service_ptr_t<file> & p_out, const char * p_path, t_open_mode p_mode, abort_callback & p_abort) override {
        if (p_mode != open_mode_read) throw exception_io_denied();

        std::string path_str = p_path;
        if (path_str.find("yandex://track/") != 0) throw exception_io_not_found();
        std::string id_str = path_str.substr(15);

        std::string wtoken = cfg_yandex_token.get_ptr();
        std::wstring wtoken_wide = pfc::stringcvt::string_wide_from_utf8(wtoken.c_str()).get_ptr();

        std::string m_title, m_artist, m_album;
        double m_duration = 0;
        try {
            std::string track_info_json = YandexAPI::HttpRequest(L"api.music.yandex.net", pfc::stringcvt::string_wide_from_utf8(("/tracks/" + id_str).c_str()).get_ptr(), wtoken_wide);
            auto track_j = nlohmann::json::parse(track_info_json);
            if (track_j.contains("result") && track_j["result"].is_array() && track_j["result"].size() > 0) {
                auto& res = track_j["result"][0];
                if (res.contains("title") && res["title"].is_string()) m_title = res["title"].get<std::string>();
                if (res.contains("artists") && res["artists"].is_array() && res["artists"].size() > 0) {
                    if (res["artists"][0].contains("name") && res["artists"][0]["name"].is_string()) m_artist = res["artists"][0]["name"].get<std::string>();
                }
                if (res.contains("albums") && res["albums"].is_array() && res["albums"].size() > 0) {
                    if (res["albums"][0].contains("title") && res["albums"][0]["title"].is_string()) m_album = res["albums"][0]["title"].get<std::string>();
                }
                if (res.contains("durationMs") && res["durationMs"].is_number()) m_duration = res["durationMs"].get<int>() / 1000.0;
            }
        } catch (...) {}

        std::wstring wpath = pfc::stringcvt::string_wide_from_utf8(("/tracks/" + id_str + "/download-info").c_str()).get_ptr();

        std::string info_resp = YandexAPI::HttpRequest(L"api.music.yandex.net", wpath, wtoken_wide);
        if (info_resp.empty()) throw exception_io_not_found();

        auto j = nlohmann::json::parse(info_resp);
        if (!j.contains("result")) throw exception_io_not_found();

        std::string download_url = "";
        int max_bitrate = 0;
        bool want_hq = cfg_yandex_hq.get();
        std::string final_codec = "mp3";

        for (auto& stream : j["result"]) {
            std::string codec = stream["codec"].get<std::string>();
            int bitrate = stream["bitrateInKbps"].get<int>();
            
            if (want_hq && codec == "flac") {
                download_url = stream["downloadInfoUrl"].get<std::string>();
                final_codec = "flac";
                break;
            }
            if (codec == "mp3") {
                if (bitrate > max_bitrate) {
                    max_bitrate = bitrate;
                    if (final_codec != "flac") {
                        download_url = stream["downloadInfoUrl"].get<std::string>();
                        final_codec = "mp3";
                    }
                }
            }
        }

        if (download_url.empty()) throw exception_io_not_found();

        if (download_url.find("https://") != 0) throw exception_io_not_found();
        size_t host_end = download_url.find("/", 8);
        if (host_end == std::string::npos) throw exception_io_not_found();
        std::string req_host = download_url.substr(8, host_end - 8);
        std::string req_path = download_url.substr(host_end);

        std::wstring wxmlhost = pfc::stringcvt::string_wide_from_utf8(req_host.c_str()).get_ptr();
        std::wstring wxmlpath = pfc::stringcvt::string_wide_from_utf8(req_path.c_str()).get_ptr();
        std::string xml_resp = YandexAPI::HttpRequest(wxmlhost, wxmlpath, wtoken_wide);

        std::string host = extract_tag(xml_resp, "host");
        std::string path = extract_tag(xml_resp, "path");
        std::string ts = extract_tag(xml_resp, "ts");
        std::string s = extract_tag(xml_resp, "s");

        if (host.empty() || path.empty() || ts.empty() || s.empty()) throw exception_io_not_found();

        std::string sign_salt = "XGRlBW9FXlekgbPrRHuSiA";
        std::string to_hash = sign_salt + path.substr(1) + s;

        static_api_ptr_t<hasher_md5> hasher;
        hasher_md5_result hash_res = hasher->process_single_string(to_hash.c_str());

        char hex_buf[33];
        for (int i = 0; i < 16; ++i) {
            sprintf(hex_buf + i * 2, "%02x", (unsigned char)hash_res.m_data[i]);
        }
        hex_buf[32] = 0;

        std::string direct_url = "https://" + host + "/get-mp3/" + std::string(hex_buf) + "/" + ts + path;

        filesystem::g_open(p_out, direct_url.c_str(), p_mode, p_abort);

        // Wrap the HTTP stream to provide metadata
        p_out = new service_impl_t<YandexFileWrapper>(p_out, m_title, m_artist, m_album, m_duration);
    }
    
    std::string extract_tag(const std::string& xml, const std::string& tag) {
        std::string start_tag = "<" + tag + ">";
        std::string end_tag = "</" + tag + ">";
        size_t start = xml.find(start_tag);
        if (start == std::string::npos) return "";
        start += start_tag.length();
        size_t end = xml.find(end_tag, start);
        if (end == std::string::npos) return "";
        return xml.substr(start, end - start);
    }
};

static service_factory_single_t<yandex_filesystem> g_yandex_filesystem_factory;

void force_link_yandex_filesystem() {}


