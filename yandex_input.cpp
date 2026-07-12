#include "foobar2000/SDK/foobar2000.h"
#include <map>
#include <mutex>

static std::map<std::string, file_info_impl> g_meta_cache;
static std::mutex g_meta_cache_mutex;

#include "yandex_api.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <regex>

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

std::string resolve_yandex_track_url(const std::string& id_str, const std::wstring& wtoken_wide) {
    std::string direct_url;

        std::wstring path(pfc::stringcvt::string_wide_from_utf8(("/api/v2.1/handlers/track/" + id_str + "/track-download/m?hq=1").c_str()).get_ptr());
        std::string track_json = YandexAPI::HttpRequest(L"music.yandex.ru", path.c_str(), wtoken_wide);

        if (!track_json.empty()) {
            auto j = nlohmann::json::parse(track_json);
            if (!j.contains("host") || !j.contains("path") || !j.contains("ts") || !j.contains("s")) {
                throw exception_io_not_found();
            }

            std::string host = j["host"].get<std::string>();
            std::string path = j["path"].get<std::string>();
            std::string ts = j["ts"].get<std::string>();
            std::string s = j["s"].get<std::string>();

            std::string sign_salt = "XGRlBW9FXlekgbPrRHuAle";
            std::string to_hash = sign_salt + path.substr(1) + s;

            static_api_ptr_t<hasher_md5> hasher;
            hasher_md5_result hash_res = hasher->process_single_string(to_hash.c_str());

            char hex_buf[33];
            for (int i = 0; i < 16; ++i) sprintf(hex_buf + i * 2, "%02x", (unsigned char)hash_res.m_data[i]);
            hex_buf[32] = 0;

            direct_url = "https://" + host + "/get-mp3/" + std::string(hex_buf) + "/" + ts + path;
        } else {
            // Fallback
            std::wstring xmlpath(pfc::stringcvt::string_wide_from_utf8(("/tracks/" + id_str + "/download-info").c_str()).get_ptr());
            std::string json_resp = YandexAPI::HttpRequest(L"api.music.yandex.net", xmlpath.c_str(), wtoken_wide);
            if (json_resp.empty()) throw exception_io_not_found();

            auto j = nlohmann::json::parse(json_resp);
            if (!j.contains("result") || !j["result"].is_array()) throw exception_io_not_found();

            std::string download_url;
            int max_bitrate = 0;

            for (auto& stream : j["result"]) {
                std::string codec = stream["codec"].get<std::string>();
                int bitrate = stream["bitrateInKbps"].get<int>();
                if (codec == "mp3" && bitrate > max_bitrate) {
                    max_bitrate = bitrate;
                    download_url = stream["downloadInfoUrl"].get<std::string>();
                }
            }
            if (download_url.empty()) throw exception_io_not_found();

            // Resolve XML -> direct stream URL
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
            std::string ts   = ym_extract_tag(xml_resp, "ts");
            std::string s    = ym_extract_tag(xml_resp, "s");
            if (host.empty() || path.empty() || ts.empty() || s.empty()) throw exception_io_not_found();

            std::string sign_salt = "XGRlBW9FXlekgbPrRHuAle";
            std::string to_hash = sign_salt + path.substr(1) + s;

            static_api_ptr_t<hasher_md5> hasher;
            hasher_md5_result hash_res = hasher->process_single_string(to_hash.c_str());

            char hex_buf[33];
            for (int i = 0; i < 16; ++i) sprintf(hex_buf + i * 2, "%02x", (unsigned char)hash_res.m_data[i]);
            hex_buf[32] = 0;

            direct_url = "https://" + host + "/get-mp3/" + std::string(hex_buf) + "/" + ts + path;
        }

        
    return direct_url;
}
class yandex_input : public input_stubs {
    service_ptr_t<input_decoder> m_decoder;
    file_info_impl m_info;

public:
    void open(service_ptr_t<file> p_filehint, const char * p_path, t_input_open_reason p_reason, abort_callback & p_abort) {
        std::string path_str = p_path;
        if (path_str.find("yandex://track/") != 0) throw exception_io_not_found();
        std::string id_str = path_str.substr(15);
        size_t dot_pos = id_str.find('.');
        if (dot_pos != std::string::npos) id_str = id_str.substr(0, dot_pos);

        std::string wtoken = cfg_yandex_token.get_ptr();
        std::wstring wtoken_wide(pfc::stringcvt::string_wide_from_utf8(wtoken.c_str()).get_ptr());

        bool have_cached_meta = false;
        {
            std::lock_guard<std::mutex> lock(g_meta_cache_mutex);
            if (g_meta_cache.find(id_str) != g_meta_cache.end()) {
                m_info = g_meta_cache[id_str];
                have_cached_meta = true;
            }
        }

        // --- 1. Fetch track metadata ---
        if (!have_cached_meta) {
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
                            std::string artists_str;
                            for (auto& art : res["artists"]) {
                                if (art.is_object() && art.contains("name") && art["name"].is_string()) {
                                    if (!artists_str.empty()) artists_str += ", ";
                                    artists_str += art["name"].get<std::string>();
                                }
                            }
                            if (!artists_str.empty()) m_info.meta_set("ARTIST", artists_str.c_str());
                        }
                        if (res.contains("albums") && res["albums"].is_array() && res["albums"].size() > 0) {
                            auto& alb = res["albums"][0];
                            if (alb.contains("title") && alb["title"].is_string())
                                m_info.meta_set("ALBUM", alb["title"].get<std::string>().c_str());
                            if (alb.contains("year") && alb["year"].is_number())
                                m_info.meta_set("DATE", std::to_string(alb["year"].get<int>()).c_str());
                            if (alb.contains("trackPosition") && alb["trackPosition"].is_object()) {
                                auto& tp = alb["trackPosition"];
                                if (tp.contains("index") && tp["index"].is_number())
                                    m_info.meta_set("TRACKNUMBER", std::to_string(tp["index"].get<int>()).c_str());
                            }
                            if (alb.contains("genre") && alb["genre"].is_string())
                                m_info.meta_set("GENRE", alb["genre"].get<std::string>().c_str());
                        }
                        if (res.contains("durationMs") && res["durationMs"].is_number())
                            m_info.set_length(res["durationMs"].get<int>() / 1000.0);
                    }
                } catch (...) {}
            }
            m_info.info_set("codec", "FLAC");
            m_info.info_set("bitrate", "900");
            m_info.info_set("samplerate", "44100");
            m_info.info_set("channels", "2");

            {
                std::lock_guard<std::mutex> lock(g_meta_cache_mutex);
                g_meta_cache[id_str] = m_info;
            }
        }

        if (p_reason == input_open_info_write) throw exception_tagging_unsupported();

        if (p_reason == input_open_info_read) {
            // Foobar2000 is only reading info. Don't open the decoder, save network requests!
            return;
        }

        // --- 2 & 3. Resolve direct URL ---
        std::string direct_url = resolve_yandex_track_url(id_str, wtoken_wide);
        if (direct_url.empty()) throw exception_io_not_found();

        try {
            input_entry::g_open_for_decoding(m_decoder, nullptr, direct_url.c_str(), p_abort);
        } catch (...) {
            throw exception_io_not_found();
        }
    }

    void decode_initialize(unsigned p_flags, abort_callback & p_abort) {
        m_decoder->decode_initialize(p_flags, p_abort);
    }

    bool decode_run(audio_chunk & p_chunk, abort_callback & p_abort) {
        return m_decoder->decode_run(p_chunk, p_abort);
    }

    void decode_seek(double p_seconds, abort_callback & p_abort) {
        m_decoder->decode_seek(p_seconds, p_abort);
    }

    bool decode_can_seek() {
        return m_decoder->decode_can_seek();
    }

    bool decode_get_dynamic_info(file_info & p_out, double & p_timestamp_delta) {
        return m_decoder->decode_get_dynamic_info(p_out, p_timestamp_delta);
    }

    bool decode_get_dynamic_info_track(file_info & p_out, double & p_timestamp_delta) {
        return m_decoder->decode_get_dynamic_info_track(p_out, p_timestamp_delta);
    }

    void decode_on_idle(abort_callback & p_abort) {
        m_decoder->decode_on_idle(p_abort);
    }

    void retag(const file_info & p_info, abort_callback & p_abort) {
        throw exception_tagging_unsupported();
    }

    void get_info(file_info & p_info, abort_callback & p_abort) {
        p_info = m_info;
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback & a) {
        return t_filestats2::from_legacy(filestats_invalid);
    }
};

static input_factory_t<yandex_input> g_yandex_input_factory;
