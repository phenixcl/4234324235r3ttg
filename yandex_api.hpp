#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <regex>
#include <nlohmann/json.hpp>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

class YandexAPI {
public:
    static std::string HttpRequest(const std::wstring& host, const std::wstring& path, const std::wstring& auth_token) {
        HINTERNET hSession = WinHttpOpen(L"foo_yandex_music/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return "";

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

        if (!auth_token.empty()) {
            std::wstring header = L"Authorization: OAuth " + auth_token + L"\r\n";
            WinHttpAddRequestHeaders(hRequest, header.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        WinHttpReceiveResponse(hRequest, NULL);

        std::string result;
        DWORD bytesAvailable = 0, bytesRead = 0;
        do {
            WinHttpQueryDataAvailable(hRequest, &bytesAvailable);
            if (bytesAvailable > 0) {
                char* buffer = new char[bytesAvailable + 1];
                WinHttpReadData(hRequest, buffer, bytesAvailable, &bytesRead);
                buffer[bytesRead] = 0;
                result.append(buffer, bytesRead);
                delete[] buffer;
            }
        } while (bytesAvailable > 0);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        return result;
    }

public:
    static std::string GetDirectTrackUrl(const std::string& track_id, const std::string& token) {
        std::wstring wtoken(token.begin(), token.end());
        std::wstring wpath = L"/tracks/" + std::wstring(track_id.begin(), track_id.end()) + L"/download-info";
        
        std::string json_resp = HttpRequest(L"api.music.yandex.net", wpath, wtoken);
        if (json_resp.empty()) return "";

        auto j = json::parse(json_resp);
        if (!j.contains("result") || j["result"].empty()) return "";

        std::string download_info_url;
        for (const auto& item : j["result"]) {
            if (item["codec"] == "flac" || (item["codec"] == "mp3" && item["bitrateInKbps"] == 320)) {
                download_info_url = item["downloadInfoUrl"];
                break;
            }
        }
        
        if (download_info_url.empty()) {
            download_info_url = j["result"][0]["downloadInfoUrl"];
        }

        std::regex url_regex(R"(https?://([^/]+)(/.*))");
        std::smatch url_match;
        if (!std::regex_search(download_info_url, url_match, url_regex)) return "";
        
        std::wstring whost(url_match[1].str().begin(), url_match[1].str().end());
        std::wstring wxml_path(url_match[2].str().begin(), url_match[2].str().end());
        
        std::string xml_resp = HttpRequest(whost, wxml_path, wtoken);

        std::smatch match;
        std::string host, path, s, ts;
        
        if (std::regex_search(xml_resp, match, std::regex("<host>(.+?)</host>"))) host = match[1].str();
        if (std::regex_search(xml_resp, match, std::regex("<path>(.+?)</path>"))) path = match[1].str();
        if (std::regex_search(xml_resp, match, std::regex("<s>(.+?)</s>"))) s = match[1].str();
        if (std::regex_search(xml_resp, match, std::regex("<ts>(.+?)</ts>"))) ts = match[1].str();

        std::string sign_str = "XGRlBW9FXlekgbPrRHuAle" + path.substr(1) + s;
        
        hasher_md5_state state;
        auto hasher = hasher_md5::get();
        hasher->initialize(state);
        hasher->process(state, sign_str.c_str(), sign_str.length());
        hasher_md5_result hash = hasher->get_result(state);
        
        char md5_hex[33];
        for (int i = 0; i < 16; i++) snprintf(md5_hex + i * 2, 3, "%02x", hash.m_data[i]);
        
        return "https://" + host + "/get-mp3/" + std::string(md5_hex) + "/" + ts + path;
    }
};
