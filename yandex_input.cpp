#include "foobar2000/SDK/foobar2000.h"
#include "yandex_api.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <windows.h>
#include <bcrypt.h>
#include <sstream>
#include <iomanip>
#include <time.h>

#pragma comment(lib, "bcrypt.lib")

extern cfg_string cfg_yandex_token;
extern cfg_bool cfg_yandex_hq;

// =====================================================
// Helpers
// =====================================================

static std::string url_encode(const std::string &value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (char c : value) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) << int((unsigned char)c);
            escaped << std::nouppercase;
        }
    }
    return escaped.str();
}

// HMAC-SHA256 → Base64 (no padding), using Windows CNG
static std::string ym_hmac_sha256_base64(const std::string& key, const std::string& data) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return "";

    BCRYPT_HASH_HANDLE hHash = NULL;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, NULL, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0))) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "";
    }

    if (!BCRYPT_SUCCESS(BCryptHashData(hHash, (PUCHAR)data.data(), (ULONG)data.size(), 0))) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "";
    }

    UCHAR hash[32];
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hHash, hash, sizeof(hash), 0))) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "";
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    // Standard base64 encode via pfc helper, then strip trailing '='
    pfc::string8 b64;
    pfc::base64_encode(b64, hash, sizeof(hash));
    std::string result = b64.get_ptr();
    while (!result.empty() && result.back() == '=') result.pop_back();
    return result;
}

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

// =====================================================
// Input: decodes yandex://track/<id> with metadata + FLAC
// =====================================================

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

        // --- 1. Fetch track metadata ---
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
                        auto& alb = res["albums"][0];
                        if (alb.contains("title") && alb["title"].is_string())
                            m_info.meta_set("ALBUM", alb["title"].get<std::string>().c_str());
                        if (alb.contains("year") && alb["year"].is_number())
                            m_info.meta_set("DATE", std::to_string(alb["year"].get<int>()).c_str());
                        if (alb.contains("trackPosition")) {
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

        if (p_reason == input_open_info_write) throw exception_tagging_unsupported();

        // --- 2. Try FLAC / lossless via /get-file-info ---
        bool want_hq = cfg_yandex_hq.get();
        std::string direct_url;

        console::printf("Yandex Music: Opening track %s (HQ: %d)", id_str.c_str(), want_hq);

        if (want_hq) {
            std::string ts = std::to_string((long long)time(NULL));
            // Codecs for sign computation (concatenated without commas)
            std::string codecs_sign = "flacaache-aacmp3flac-mp4aac-mp4he-aac-mp4";
            std::string codecs_query = "flac,aac,he-aac,mp3,flac-mp4,aac-mp4,he-aac-mp4";
            std::string sign_data = ts + id_str + "lossless" + codecs_sign + "raw";
            std::string sign = ym_hmac_sha256_base64("7tvSmFbyf5hJnIHhCimDDD", sign_data);

            if (!sign.empty()) {
                std::string flac_path = "/get-file-info?ts=" + ts
                    + "&trackId=" + id_str
                    + "&quality=lossless"
                    + "&codecs=" + url_encode(codecs_query)
                    + "&transports=raw"
                    + "&sign=" + url_encode(sign);
                std::wstring wpath(pfc::stringcvt::string_wide_from_utf8(flac_path.c_str()).get_ptr());

                std::string info_resp = YandexAPI::HttpRequest(L"api.music.yandex.net", wpath.c_str(), wtoken_wide);
                if (!info_resp.empty()) {
                    try {
                        auto j = nlohmann::json::parse(info_resp);
                        if (j.contains("result") && j["result"].contains("downloadInfo")) {
                            auto& di = j["result"]["downloadInfo"];
                            if (di.contains("urls") && di["urls"].is_array() && di["urls"].size() > 0) {
                                direct_url = di["urls"][0].get<std::string>();
                                
                                // Append fragment to force foobar2000 to use the correct decoder and bypass sniffing
                                std::string res_codec = di["codec"].get<std::string>();
                                if (res_codec == "flac-mp4" || res_codec == "aac-mp4" || res_codec == "he-aac-mp4") {
                                    direct_url += "#.m4a";
                                } else if (res_codec == "mp3") {
                                    direct_url += "#.mp3";
                                } else if (res_codec == "flac") {
                                    direct_url += "#.flac";
                                }

                                console::printf("Yandex Music: direct_url (HQ) = %s", direct_url.c_str());
                            } else if (di.contains("url") && di["url"].is_string()) {
                                direct_url = di["url"].get<std::string>();
                            }
                        }
                    } catch (...) {}
                }
            }
        }

        // --- 3. Fallback to MP3 via /download-info ---
        if (direct_url.empty()) {
            std::wstring wpath(pfc::stringcvt::string_wide_from_utf8(("/tracks/" + id_str + "/download-info").c_str()).get_ptr());
            std::string info_resp = YandexAPI::HttpRequest(L"api.music.yandex.net", wpath.c_str(), wtoken_wide);
            console::printf("Yandex Music: download-info response (length: %zu)", info_resp.length());
            if (info_resp.empty()) {
                console::printf("Yandex Music: Empty download-info response!");
                throw exception_io_not_found();
            }

            try {
                auto j = nlohmann::json::parse(info_resp);
                if (!j.contains("result") || !j["result"].is_array()) {
                    console::printf("Yandex Music: JSON result missing or not array!");
                    throw exception_io_not_found();
                }

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
                
                console::printf("Yandex Music: MP3 download_url = %s", download_url.c_str());
                if (download_url.empty()) throw exception_io_not_found();

                // Resolve XML → direct stream URL
                if (download_url.find("https://") != 0) throw exception_io_not_found();
                size_t host_end = download_url.find("/", 8);
                if (host_end == std::string::npos) throw exception_io_not_found();
                std::string req_host = download_url.substr(8, host_end - 8);
                std::string req_path = download_url.substr(host_end);

                std::wstring wxmlhost(pfc::stringcvt::string_wide_from_utf8(req_host.c_str()).get_ptr());
                std::wstring wxmlpath(pfc::stringcvt::string_wide_from_utf8(req_path.c_str()).get_ptr());
                std::string xml_resp = YandexAPI::HttpRequest(wxmlhost.c_str(), wxmlpath.c_str(), L"");

                console::printf("Yandex Music: XML response: %s", xml_resp.c_str());

                std::string host = ym_extract_tag(xml_resp, "host");
                std::string path = ym_extract_tag(xml_resp, "path");
                std::string ts   = ym_extract_tag(xml_resp, "ts");
                std::string s    = ym_extract_tag(xml_resp, "s");
                
                if (host.empty() || path.empty() || ts.empty() || s.empty()) {
                    console::printf("Yandex Music: Failed to extract tags from XML!");
                    throw exception_io_not_found();
                }

                std::string sign_salt = "XGRlBW9FXlekgbPrRHuAle";
                std::string to_hash = sign_salt + path.substr(1) + s;

                static_api_ptr_t<hasher_md5> hasher;
                hasher_md5_result hash_res = hasher->process_single_string(to_hash.c_str());

                char hex_buf[33];
                for (int i = 0; i < 16; ++i) sprintf(hex_buf + i * 2, "%02x", (unsigned char)hash_res.m_data[i]);
                hex_buf[32] = 0;

                direct_url = "https://" + host + "/get-mp3/" + std::string(hex_buf) + "/" + ts + path + "#.mp3";
                console::printf("Yandex Music: direct_url (MP3 fallback) = %s", direct_url.c_str());
            } catch (const std::exception& e) {
                console::printf("Yandex Music: Exception while parsing MP3 fallback: %s", e.what());
                throw exception_io_not_found();
            }
        }

        // --- 4. Open the inner decoder for the real HTTP(S) URL ---
        if (p_reason == input_open_info_read) {
            // We already have metadata from the API – no need to open a decoder
        } else {
            console::printf("Yandex Music: Opening inner decoder for URL: %s", direct_url.c_str());
            try {
                input_entry::g_open_for_decoding(m_decoder, nullptr, direct_url.c_str(), p_abort);
            } catch (const std::exception& e) {
                console::printf("Yandex Music: g_open_for_decoding failed with exception: %s", e.what());
                throw;
            }
        }
    }

    void get_info(file_info & p_info, abort_callback & p_abort) {
        p_info = m_info;
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback & p_abort) {
        return t_filestats2::from_legacy(filestats_invalid);
    }

    void decode_initialize(unsigned p_flags, abort_callback & p_abort) {
        if (m_decoder.is_valid()) m_decoder->initialize(0, p_flags, p_abort);
    }

    bool decode_run(audio_chunk & p_chunk, abort_callback & p_abort) {
        if (m_decoder.is_valid()) return m_decoder->run(p_chunk, p_abort);
        return false;
    }

    void decode_seek(double p_seconds, abort_callback & p_abort) {
        if (m_decoder.is_valid()) m_decoder->seek(p_seconds, p_abort);
    }

    bool decode_can_seek() {
        if (m_decoder.is_valid()) return m_decoder->can_seek();
        return false;
    }

    bool decode_get_dynamic_info(file_info & p_out, double & p_timestamp_delta) {
        if (m_decoder.is_valid()) return m_decoder->get_dynamic_info(p_out, p_timestamp_delta);
        return false;
    }

    bool decode_get_dynamic_info_track(file_info & p_out, double & p_timestamp_delta) {
        if (m_decoder.is_valid()) return m_decoder->get_dynamic_info_track(p_out, p_timestamp_delta);
        return false;
    }

    void decode_on_idle(abort_callback & p_abort) {
        if (m_decoder.is_valid()) m_decoder->on_idle(p_abort);
    }

    void retag(const file_info & p_info, abort_callback & p_abort) {
        throw exception_tagging_unsupported();
    }

    void remove_tags(abort_callback & p_abort) {
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

