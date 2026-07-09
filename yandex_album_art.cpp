#include "stdafx.h"
#include <foobar2000/SDK/foobar2000.h>
#include <foobar2000/SDK/album_art_helpers.h>
#include <nlohmann/json.hpp>
#include "yandex_api.hpp"

extern cfg_string cfg_yandex_token;

class YandexAlbumArtExtractorInstance : public album_art_extractor_instance {
public:
    YandexAlbumArtExtractorInstance(const char* p_path) : m_path(p_path) {}

    album_art_data_ptr query(const GUID& p_what, abort_callback& p_abort) override {
        if (p_what != album_art_ids::cover_front) throw exception_album_art_not_found();

        std::string path_str = m_path;
        if (path_str.find("yandex://track/") != 0) throw exception_album_art_not_found();
        std::string id_str = path_str.substr(15);

        std::string wtoken = cfg_yandex_token.get_ptr();
        std::wstring wtoken_wide = pfc::stringcvt::string_wide_from_utf8(wtoken.c_str()).get_ptr();

        std::string track_info_json = YandexAPI::HttpRequest(L"api.music.yandex.net", pfc::stringcvt::string_wide_from_utf8(("/tracks/" + id_str).c_str()).get_ptr(), wtoken_wide);
        if (track_info_json.empty()) throw exception_album_art_not_found();

        std::string coverUri;
        try {
            auto track_j = nlohmann::json::parse(track_info_json);
            if (track_j.contains("result") && track_j["result"].is_array() && track_j["result"].size() > 0) {
                auto& res = track_j["result"][0];
                if (res.contains("albums") && res["albums"].is_array() && res["albums"].size() > 0) {
                    if (res["albums"][0].contains("coverUri") && res["albums"][0]["coverUri"].is_string()) {
                        coverUri = res["albums"][0]["coverUri"].get<std::string>();
                    }
                }
            }
        } catch (...) {
            throw exception_album_art_not_found();
        }

        if (coverUri.empty()) throw exception_album_art_not_found();

        // coverUri is "avatars.yandex.net/get-music-content/235654/xxx/%%"
        size_t pos = coverUri.find("%%");
        if (pos != std::string::npos) {
            coverUri.replace(pos, 2, "400x400"); // Request 400x400 size
        }

        pos = coverUri.find('/');
        if (pos == std::string::npos) throw exception_album_art_not_found();
        std::string host = coverUri.substr(0, pos);
        std::string path = "/" + coverUri.substr(pos + 1); // Ensure leading slash

        std::wstring whost = pfc::stringcvt::string_wide_from_utf8(host.c_str()).get_ptr();
        std::wstring wpath = pfc::stringcvt::string_wide_from_utf8(path.c_str()).get_ptr();

        // Yandex Image API does not require authorization token
        std::string image_data = YandexAPI::HttpRequest(whost, wpath, L"");
        if (image_data.empty()) throw exception_album_art_not_found();

        return album_art_data_impl::g_create(image_data.data(), image_data.size());
    }

private:
    std::string m_path;
};

class YandexAlbumArtExtractor : public album_art_extractor {
public:
    bool is_our_path(const char* p_path, const char* p_extension) override {
        return strncmp(p_path, "yandex://", 9) == 0;
    }

    album_art_extractor_instance_ptr open(file_ptr p_filehint, const char* p_path, abort_callback& p_abort) override {
        if (!is_our_path(p_path, "")) throw exception_album_art_not_found();
        return new service_impl_t<YandexAlbumArtExtractorInstance>(p_path);
    }
};

static service_factory_single_t<YandexAlbumArtExtractor> g_yandex_album_art_extractor_factory;
