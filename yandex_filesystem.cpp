#include "foobar2000/SDK/foobar2000.h"
#include "yandex_api.hpp"
#include <nlohmann/json.hpp>
#include <string>

extern cfg_string cfg_yandex_token;
extern cfg_bool cfg_yandex_hq;

class yandex_input {
    service_ptr_t<input_decoder> m_decoder;
    file_info_impl m_info;
    t_filestats m_stats;

public:
    void open(service_ptr_t<file> p_filehint, const char * p_path, t_input_open_reason p_reason, abort_callback & p_abort) {
        std::string path_str = p_path;
        if (path_str.find("yandex://track/") != 0) throw exception_io_not_found();
        std::string id_str = path_str.substr(15);
        size_t dot_pos = id_str.find('.');
        if (dot_pos != std::string::npos) id_str = id_str.substr(0, dot_pos);

        std::string wtoken = cfg_yandex_token.get_ptr();
        std::wstring wtoken_wide = pfc::stringcvt::string_wide_from_utf8(wtoken.c_str()).get_ptr();

        // 1. Fetch metadata
        std::string track_info_json = YandexAPI::HttpRequest(L"api.music.yandex.net", pfc::stringcvt::string_wide_from_utf8(("/tracks/" + id_str).c_str()).get_ptr(), wtoken_wide);
        if (track_info_json.empty()) throw exception_io_not_found();
        
        try {
            auto track_j = nlohmann::json::parse(track_info_json);
            if (track_j.contains("result") && track_j["result"].is_array() && track_j["result"].size() > 0) {
                auto& res = track_j["result"][0];
                if (res.contains("title") && res["title"].is_string()) m_info.meta_set("TITLE", res["title"].get<std::string>().c_str());
                if (res.contains("artists") && res["artists"].is_array() && res["artists"].size() > 0) {
                    if (res["artists"][0].contains("name") && res["artists"][0]["name"].is_string()) m_info.meta_set("ARTIST", res["artists"][0]["name"].get<std::string>().c_str());
                }
                if (res.contains("albums") && res["albums"].is_array() && res["albums"].size() > 0) {
                    if (res["albums"][0].contains("title") && res["albums"][0]["title"].is_string()) m_info.meta_set("ALBUM", res["albums"][0]["title"].get<std::string>().c_str());
                }
                if (res.contains("durationMs") && res["durationMs"].is_number()) m_info.set_length(res["durationMs"].get<int>() / 1000.0);
            }
        } catch (...) {
            throw exception_io_not_found();
        }

        // 2. Fetch download info (direct URL)
        std::wstring wpath = pfc::stringcvt::string_wide_from_utf8(("/tracks/" + id_str + "/download-info").c_str()).get_ptr();
        std::string info_resp = YandexAPI::HttpRequest(L"api.music.yandex.net", wpath, wtoken_wide);
        if (info_resp.empty()) throw exception_io_not_found();

        std::string download_url = "";
        try {
            auto j = nlohmann::json::parse(info_resp);
            if (!j.contains("result") || j["result"].empty()) throw exception_io_not_found();
            
            bool want_hq = cfg_yandex_hq.get();
            for (const auto& item : j["result"]) {
                std::string codec = item["codec"].get<std::string>();
                if (want_hq && codec == "flac") {
                    download_url = item["downloadInfoUrl"].get<std::string>();
                    break;
                }
                if (!want_hq && codec == "mp3") {
                    download_url = item["downloadInfoUrl"].get<std::string>();
                    break;
                }
            }
            if (download_url.empty()) {
                download_url = j["result"][0]["downloadInfoUrl"].get<std::string>();
            }
        } catch (...) {
            throw exception_io_not_found();
        }

        // 3. Fetch XML to get the direct stream URL
        std::string xml_resp = YandexAPI::HttpRequest(L"", pfc::stringcvt::string_wide_from_utf8(download_url.c_str()).get_ptr(), L"");
        if (xml_resp.empty()) throw exception_io_not_found();
        
        std::string host, path, s;
        size_t host_pos = xml_resp.find("<host>");
        if (host_pos != std::string::npos) {
            size_t host_end = xml_resp.find("</host>", host_pos);
            if (host_end != std::string::npos) host = xml_resp.substr(host_pos + 6, host_end - host_pos - 6);
        }
        size_t path_pos = xml_resp.find("<path>");
        if (path_pos != std::string::npos) {
            size_t path_end = xml_resp.find("</path>", path_pos);
            if (path_end != std::string::npos) path = xml_resp.substr(path_pos + 6, path_end - path_pos - 6);
        }
        size_t s_pos = xml_resp.find("<s>");
        if (s_pos != std::string::npos) {
            size_t s_end = xml_resp.find("</s>", s_pos);
            if (s_end != std::string::npos) s = xml_resp.substr(s_pos + 3, s_end - s_pos - 3);
        }
        if (host.empty() || path.empty() || s.empty()) throw exception_io_not_found();
        
        std::string direct_url = "https://" + host + "/get-mp3/" + s + path;

        // 4. Open underlying decoder for the direct URL
        if (p_reason == input_open_info_read) {
            // We just needed metadata, no need to open decoder!
            m_stats.m_size = filesize_invalid;
            m_stats.m_timestamp = filetimestamp_invalid;
        } else {
            // Open for decoding
            input_manager::get()->open_for_decoding(m_decoder, nullptr, direct_url.c_str(), p_abort);
            m_stats = m_decoder->get_file_stats(p_abort);
        }
    }

    void get_info(file_info & p_info, abort_callback & p_abort) {
        p_info = m_info;
    }

    t_filestats get_file_stats(abort_callback & p_abort) {
        return m_stats;
    }

    void decode_initialize(unsigned p_flags, abort_callback & p_abort) {
        if (m_decoder.is_valid()) m_decoder->decode_initialize(p_flags, p_abort);
    }

    bool decode_run(audio_chunk & p_chunk, abort_callback & p_abort) {
        if (m_decoder.is_valid()) return m_decoder->decode_run(p_chunk, p_abort);
        return false;
    }

    void decode_seek(double p_seconds, abort_callback & p_abort) {
        if (m_decoder.is_valid()) m_decoder->decode_seek(p_seconds, p_abort);
    }

    bool decode_can_seek() {
        if (m_decoder.is_valid()) return m_decoder->decode_can_seek();
        return false;
    }

    bool decode_get_dynamic_info(file_info & p_out, double & p_timestamp_delta) {
        if (m_decoder.is_valid()) return m_decoder->decode_get_dynamic_info(p_out, p_timestamp_delta);
        return false;
    }

    bool decode_get_dynamic_info_track(file_info & p_out, double & p_timestamp_delta) {
        if (m_decoder.is_valid()) return m_decoder->decode_get_dynamic_info_track(p_out, p_timestamp_delta);
        return false;
    }

    void decode_on_idle(abort_callback & p_abort) {
        if (m_decoder.is_valid()) m_decoder->decode_on_idle(p_abort);
    }

    void retag_set_info(t_uint32 p_subsong, const file_info & p_info, abort_callback & p_abort) {
        throw exception_io_denied();
    }

    void retag_commit(abort_callback & p_abort) {
        throw exception_io_denied();
    }

    static bool g_is_our_content_type(const char * p_content_type) { return false; }
    static bool g_is_our_path(const char * p_path, const char * p_extension) {
        return strncmp(p_path, "yandex://", 9) == 0;
    }
};

static input_singletrack_factory_t<yandex_input> g_yandex_input_factory;
