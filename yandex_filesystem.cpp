#include "foobar2000/SDK/foobar2000.h"
#include "yandex_api.hpp"
#include <nlohmann/json.hpp>
#include <string>

extern cfg_string cfg_yandex_token;
extern cfg_bool cfg_yandex_hq;

// =====================================================
// Filesystem: makes yandex:// URLs valid in playlists
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
        // The input handler will do the actual work.
        // This just needs to not crash — the filesystem exists so yandex:// URLs are recognized.
        throw exception_io_unsupported_format();
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

// =====================================================
// Input: handles yandex:// decoding with proper metadata
// =====================================================

static std::string ym_extract_tag(const std::string& xml, const std::string& tag) {
    std::string start_tag = "<" + tag + ">";
    std::string end_tag = "</" + tag + ">";
    size_t start = xml.find(start_tag);
    if (start == std::string::npos) return "";
    start += start_tag.length();
    size_t end = xml.find(end_tag, start);
    if (end == std::string::npos) return "";
    return xml.substr(start, end - start);
}

class yandex_input : public input_stubs {
    service_ptr_t<input_decoder> m_decoder;
    file_info_impl m_info;
    bool m_has_decoder;

public:
    yandex_input() : m_has_decoder(false) {}

    void open(service_ptr_t<file> p_filehint, const char * p_path, t_input_open_reason p_reason, abort_callback & p_abort) {
        std::string path_str = p_path;
        if (path_str.find("yandex://track/") != 0) throw exception_io_not_found();
        std::string id_str = path_str.substr(15);
        size_t dot_pos = id_str.find('.');
        if (dot_pos != std::string::npos) id_str = id_str.substr(0, dot_pos);

        std::string wtoken = cfg_yandex_token.get_ptr();
        std::wstring wtoken_wide(pfc::stringcvt::string_wide_from_utf8(wtoken.c_str()).get_ptr());

        // 1. Fetch track metadata from API
        std::wstring meta_path(pfc::stringcvt::string_wide_from_utf8(("/tracks/" + id_str).c_str()).get_ptr());
        std::string track_info_json = YandexAPI::HttpRequest(L"api.music.yandex.net", meta_path.c_str(), wtoken_wide);
        if (!track_info_json.empty()) {
            try {
                auto track_j = nlohmann::json::parse(track_info_json);
                if (track_j.contains("result") && track_j["result"].is_array() && track_j["result"].size() > 0) {
                    auto& res = track_j["result"][0];
                    if (res.contains("title") && res["title"].is_string())
                        m_info.meta_set("TITLE", res["title"].get<std::string>().c_str());
                    if (res.contains("artists") && res["artists"].is_array() && res["artists"].size() > 0) {
                        if (res["artists"][0].contains("name") && res["artists"][0]["name"].is_string())
                            m_info.meta_set("ARTIST", res["artists"][0]["name"].get<std::string>().c_str());
                    }
                    if (res.contains("albums") && res["albums"].is_array() && res["albums"].size() > 0) {
                        if (res["albums"][0].contains("title") && res["albums"][0]["title"].is_string())
                            m_info.meta_set("ALBUM", res["albums"][0]["title"].get<std::string>().c_str());
                    }
                    if (res.contains("durationMs") && res["durationMs"].is_number())
                        m_info.set_length(res["durationMs"].get<int>() / 1000.0);
                }
            } catch (...) {}
        }

        if (p_reason == input_open_info_write) throw exception_tagging_unsupported();

        // 2. Fetch download info
        std::wstring wpath(pfc::stringcvt::string_wide_from_utf8(("/tracks/" + id_str + "/download-info").c_str()).get_ptr());
        std::string info_resp = YandexAPI::HttpRequest(L"api.music.yandex.net", wpath.c_str(), wtoken_wide);
        if (info_resp.empty()) throw exception_io_not_found();

        auto j = nlohmann::json::parse(info_resp);
        if (!j.contains("result")) throw exception_io_not_found();

        std::string download_url = "";
        int max_bitrate = 0;
        bool want_hq = cfg_yandex_hq.get();

        for (auto& stream : j["result"]) {
            std::string codec = stream["codec"].get<std::string>();
            int bitrate = stream["bitrateInKbps"].get<int>();
            
            if (want_hq && codec == "flac") {
                download_url = stream["downloadInfoUrl"].get<std::string>();
                break;
            }
            if (codec == "mp3") {
                if (bitrate > max_bitrate) {
                    max_bitrate = bitrate;
                    download_url = stream["downloadInfoUrl"].get<std::string>();
                }
            }
        }

        if (download_url.empty()) throw exception_io_not_found();

        // 3. Resolve the XML to get the actual stream URL
        if (download_url.find("https://") != 0) throw exception_io_not_found();
        size_t host_end = download_url.find("/", 8);
        if (host_end == std::string::npos) throw exception_io_not_found();
        std::string req_host = download_url.substr(8, host_end - 8);
        std::string req_path = download_url.substr(host_end);

        std::wstring wxmlhost(pfc::stringcvt::string_wide_from_utf8(req_host.c_str()).get_ptr());
        std::wstring wxmlpath(pfc::stringcvt::string_wide_from_utf8(req_path.c_str()).get_ptr());
        std::string xml_resp = YandexAPI::HttpRequest(wxmlhost.c_str(), wxmlpath.c_str(), wtoken_wide);

        std::string host = ym_extract_tag(xml_resp, "host");
        std::string path = ym_extract_tag(xml_resp, "path");
        std::string ts = ym_extract_tag(xml_resp, "ts");
        std::string s = ym_extract_tag(xml_resp, "s");

        if (host.empty() || path.empty() || ts.empty() || s.empty()) throw exception_io_not_found();

        // 4. Build signed URL
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

        // 5. Open an inner decoder for the real HTTP URL
        if (p_reason == input_open_decode || p_reason == input_open_info_read) {
            input_entry::g_open_for_decoding(m_decoder, nullptr, direct_url.c_str(), p_abort);
            m_has_decoder = true;
        }
    }

    void get_info(file_info & p_info, abort_callback & p_abort) {
        p_info = m_info;
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback & p_abort) {
        t_filestats2 ret = t_filestats2::from_legacy(filestats_invalid);
        return ret;
    }

    void decode_initialize(unsigned p_flags, abort_callback & p_abort) {
        if (m_has_decoder) m_decoder->initialize(0, p_flags, p_abort);
    }

    bool decode_run(audio_chunk & p_chunk, abort_callback & p_abort) {
        if (m_has_decoder) return m_decoder->run(p_chunk, p_abort);
        return false;
    }

    void decode_seek(double p_seconds, abort_callback & p_abort) {
        if (m_has_decoder) m_decoder->seek(p_seconds, p_abort);
    }

    bool decode_can_seek() {
        if (m_has_decoder) return m_decoder->can_seek();
        return false;
    }

    void retag(const file_info & p_info, abort_callback & p_abort) {
        throw exception_tagging_unsupported();
    }

    static bool g_is_our_content_type(const char * p_content_type) { return false; }
    static bool g_is_our_path(const char * p_path, const char * p_extension) {
        return strncmp(p_path, "yandex://", 9) == 0;
    }
    static GUID g_get_guid() {
        // {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
        static const GUID guid = { 0xa1b2c3d4, 0xe5f6, 0x7890, { 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90 } };
        return guid;
    }
    static const char * g_get_name() { return "Yandex Music Input"; }
};

static input_singletrack_factory_t<yandex_input> g_yandex_input_factory;
