#include "foobar2000/SDK/foobar2000.h"
#include <nlohmann/json.hpp>
#include "yandex_api.hpp"
#include <time.h>
#include <iomanip>
#include <sstream>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

extern cfg_string cfg_yandex_token;
extern cfg_bool cfg_yandex_hq;

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

static std::string ym_hmac_sha256_base64(const std::string& key, const std::string& data) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return "";
    BCRYPT_HASH_HANDLE hHash = NULL;
    if (BCryptCreateHash(hAlg, &hHash, NULL, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0) < 0) { BCryptCloseAlgorithmProvider(hAlg, 0); return ""; }
    if (BCryptHashData(hHash, (PUCHAR)data.data(), (ULONG)data.size(), 0) < 0) { BCryptDestroyHash(hHash); BCryptCloseAlgorithmProvider(hAlg, 0); return ""; }
    UCHAR hash[32];
    if (BCryptFinishHash(hHash, hash, sizeof(hash), 0) < 0) { BCryptDestroyHash(hHash); BCryptCloseAlgorithmProvider(hAlg, 0); return ""; }
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
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

class yandex_filesystem : public filesystem {
public:
    bool get_canonical_path(const char * path, pfc::string_base & out) override { out = path; return true; }
    bool is_our_path(const char * path) override { return strncmp(path, "yandex://", 9) == 0; }
    bool get_display_path(const char * path, pfc::string_base & out) override { out = path; return true; }
    void remove(const char * p_path, abort_callback & p_abort) override { throw exception_io_denied(); }
    void move(const char * p_src, const char * p_dst, abort_callback & p_abort) override { throw exception_io_denied(); }
    bool is_remote(const char * p_src) override { return true; }
    bool relative_path_create(const char * file_path, const char * playlist_path, pfc::string_base & out) override { return false; }
    bool relative_path_parse(const char * relative_path, const char * playlist_path, pfc::string_base & out) override { return false; }
    void create_directory(const char * path, abort_callback & p_abort) override { throw exception_io_denied(); }
    void list_directory(const char * p_path, directory_callback & p_out, abort_callback & p_abort) override { throw exception_io_denied(); }
    bool supports_content_types() override { return false; }

    void get_stats(const char * p_path, t_filestats & p_stats, bool & p_is_writeable, abort_callback & p_abort) override {
        p_stats.m_size = filesize_invalid;
        p_stats.m_timestamp = filetimestamp_invalid;
        p_is_writeable = false;
    }

    void open(service_ptr_t<file> & p_out, const char * p_path, t_open_mode p_mode, abort_callback & p_abort) override {
        if (p_mode != open_mode_read) throw exception_io_denied();

        std::string path_str = p_path;
        if (path_str.find("yandex://track/") != 0) throw exception_io_not_found();

        std::string id_str = path_str.substr(15);
        size_t dot_pos = id_str.find('.');
        if (dot_pos != std::string::npos) id_str = id_str.substr(0, dot_pos);

        pfc::string8 wtoken_str;
        cfg_yandex_token.get(wtoken_str);
        std::string wtoken = wtoken_str.c_str();
        std::wstring wtoken_wide(pfc::stringcvt::string_wide_from_utf8(wtoken.c_str()).get_ptr());

        bool want_hq = cfg_yandex_hq.get();
        std::string direct_url;

        if (want_hq) {
            std::string ts = std::to_string((long long)time(NULL));
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
                                std::string replaced_url = direct_url;
                                size_t pos = 0;
                                while((pos = replaced_url.find(",", pos)) != std::string::npos) {
                                    replaced_url.replace(pos, 1, "%2C");
                                    pos += 3;
                                }
                                direct_url = replaced_url;
                            } else if (di.contains("url") && di["url"].is_string()) {
                                direct_url = di["url"].get<std::string>();
                            }
                        }
                    } catch (...) {}
                }
            }
        }

        if (direct_url.empty()) {
            std::wstring wpath(pfc::stringcvt::string_wide_from_utf8(("/tracks/" + id_str + "/download-info").c_str()).get_ptr());
            std::string info_resp = YandexAPI::HttpRequest(L"api.music.yandex.net", wpath.c_str(), wtoken_wide);
            if (info_resp.empty()) throw exception_io_not_found();

            try {
                auto j = nlohmann::json::parse(info_resp);
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
                if (download_url.find("https://") != 0) throw exception_io_not_found();
                size_t host_end = download_url.find("/", 8);
                if (host_end == std::string::npos) throw exception_io_not_found();
                std::string req_host = download_url.substr(8, host_end - 8);
                std::string req_path = download_url.substr(host_end);

                std::wstring wxmlhost(pfc::stringcvt::string_wide_from_utf8(req_host.c_str()).get_ptr());
                std::wstring wxmlpath(pfc::stringcvt::string_wide_from_utf8(req_path.c_str()).get_ptr());
                std::string xml_resp = YandexAPI::HttpRequest(wxmlhost.c_str(), wxmlpath.c_str(), L"");

                std::string host = ym_extract_tag(xml_resp, "host");
                std::string path = ym_extract_tag(xml_resp, "path");
                std::string s    = ym_extract_tag(xml_resp, "s");
                std::string ts   = ym_extract_tag(xml_resp, "ts");
                
                if (host.empty() || path.empty() || s.empty()) throw exception_io_not_found();

                std::string sign_str = "XGRlBW9FXlekgbPrRHuAle" + path.substr(1) + s;
                hasher_md5_state state;
                auto hasher = hasher_md5::get();
                hasher->initialize(state);
                hasher->process(state, sign_str.c_str(), sign_str.length());
                hasher_md5_result hash = hasher->get_result(state);
                
                char md5_hex[33];
                for (int i = 0; i < 16; i++) snprintf(md5_hex + i * 2, 3, "%02x", hash.m_data[i]);
                
                direct_url = "https://" + host + "/get-mp3/" + std::string(md5_hex) + "/" + ts + path;
            } catch (...) {
                throw exception_io_not_found();
            }
        }

        if (direct_url.empty()) throw exception_io_not_found();
        filesystem::g_open(p_out, direct_url.c_str(), p_mode, p_abort);
    }
};

static service_factory_single_t<yandex_filesystem> g_yandex_filesystem_factory;

void force_link_yandex_filesystem() {}
