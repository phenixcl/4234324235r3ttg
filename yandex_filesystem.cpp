#include "stdafx.h"
#include <foobar2000/SDK/foobar2000.h>
#include <nlohmann/json.hpp>
#include "yandex_api.hpp"

extern cfg_string cfg_yandex_token;
extern cfg_bool cfg_yandex_hq;

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

        std::string track_id = path_str.substr(15);
        std::wstring wpath = pfc::stringcvt::string_wide_from_utf8(("/tracks/" + track_id + "/download-info").c_str()).get_ptr();
        std::wstring wtoken = pfc::stringcvt::string_wide_from_utf8(cfg_yandex_token.get_ptr()).get_ptr();

        std::string info_resp = YandexAPI::HttpRequest(L"api.music.yandex.net", wpath, wtoken);
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
                if (want_hq && bitrate == 320) {
                    download_url = stream["downloadInfoUrl"].get<std::string>();
                    break;
                }
                if (bitrate > max_bitrate) {
                    max_bitrate = bitrate;
                    download_url = stream["downloadInfoUrl"].get<std::string>();
                }
            }
        }

        if (download_url.empty()) throw exception_io_not_found();

        std::string prefix = "https://api.music.yandex.net";
        if (download_url.find(prefix) != 0) throw exception_io_not_found();

        std::wstring wxmlpath = pfc::stringcvt::string_wide_from_utf8(download_url.substr(prefix.length()).c_str()).get_ptr();
        std::string xml_resp = YandexAPI::HttpRequest(L"api.music.yandex.net", wxmlpath, wtoken);

        std::string host = extract_tag(xml_resp, "host");
        std::string path = extract_tag(xml_resp, "path");
        std::string ts = extract_tag(xml_resp, "ts");
        std::string s = extract_tag(xml_resp, "s");

        if (host.empty() || path.empty() || ts.empty() || s.empty()) throw exception_io_not_found();

        std::string sign_salt = "XGRlBW9FXlekgbPrRHuAle";
        std::string to_hash = sign_salt + path.substr(1) + s;

        static_api_ptr_t<hasher_md5> hasher;
        hasher_md5_result hash_res = hasher->process_single_string(to_hash.c_str());

        char hex_buf[33];
        for (int i = 0; i < 16; ++i) {
            sprintf(hex_buf + i * 2, "%02x", hash_res.m_data[i]);
        }
        hex_buf[32] = 0;

        std::string direct_url = "https://" + host + "/get-" + final_codec + "/" + std::string(hex_buf) + "/" + ts + path;

        filesystem::g_open(p_out, direct_url.c_str(), p_mode, p_abort);
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


