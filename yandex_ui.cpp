#include "stdafx.h"
#include "resource.h"
#include <helpers/atl-misc.h>
#include <helpers/foobar2000+atl.h>
#include <helpers/BumpableElem.h>
#include "yandex_api.hpp"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <thread>
#include <sstream>
#include <iomanip>

// Configuration string for Yandex Token
static const GUID guid_cfg_yandex_token = { 0x5a1b32f1, 0xc8a4, 0x4f12, { 0x9e, 0x21, 0x1d, 0x5c, 0xa1, 0x4f, 0x12, 0x3d } };
cfg_string cfg_yandex_token(guid_cfg_yandex_token, "");

static const GUID guid_cfg_yandex_hq = { 0x5a1b32f2, 0xc8a4, 0x4f12, { 0x9e, 0x21, 0x1d, 0x5c, 0xa1, 0x4f, 0x12, 0x3e } };
cfg_bool cfg_yandex_hq(guid_cfg_yandex_hq, true);

// URL Encoding helper
std::string url_encode(const std::string &value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << std::uppercase << '%' << std::setw(2) << int((unsigned char)c) << std::nouppercase;
        }
    }
    return escaped.str();
}

// -----------------------------------------------------------------------------
// Preferences Page (Auth)
// -----------------------------------------------------------------------------
class CYandexPreferences : public CDialogImpl<CYandexPreferences>, public preferences_page_instance {
public:
    CYandexPreferences(preferences_page_callback::ptr callback) : m_callback(callback) {}
    
    enum { IDD = IDD_YANDEX_PREFS };
    
    BEGIN_MSG_MAP_EX(CYandexPreferences)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_HANDLER_EX(IDC_YANDEX_TOKEN, EN_CHANGE, OnEditChange)
        COMMAND_HANDLER_EX(IDC_YANDEX_HQ, BN_CLICKED, OnCheckChange)
        COMMAND_ID_HANDLER_EX(IDC_YANDEX_LOGIN_BTN, OnLoginBtn)
    END_MSG_MAP()
    
    BOOL OnInitDialog(CWindow, LPARAM) {
        uSetDlgItemText(m_hWnd, IDC_YANDEX_TOKEN, cfg_yandex_token.get_ptr());
        CheckDlgButton(IDC_YANDEX_HQ, cfg_yandex_hq.get() ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    
    void OnEditChange(UINT, int, CWindow) {
        m_callback->on_state_changed();
    }
    
    void OnCheckChange(UINT, int, CWindow) {
        m_callback->on_state_changed();
    }
    
    void OnLoginBtn(UINT, int, CWindow) {
        MessageBox(_T("Token saved!\nYou can get your token using Yandex Music API extractors."), _T("Yandex Music"), MB_OK | MB_ICONINFORMATION);
    }
    
    uint32_t get_state() override {
        uint32_t state = preferences_state::resettable;
        pfc::string8 current_token = uGetDlgItemText(m_hWnd, IDC_YANDEX_TOKEN);
        if (current_token != cfg_yandex_token.get_ptr()) state |= preferences_state::changed;
        if ((IsDlgButtonChecked(IDC_YANDEX_HQ) == BST_CHECKED) != cfg_yandex_hq.get()) state |= preferences_state::changed;
        return state;
    }
    
    void apply() override {
        cfg_yandex_token = uGetDlgItemText(m_hWnd, IDC_YANDEX_TOKEN).get_ptr();
        cfg_yandex_hq = (IsDlgButtonChecked(IDC_YANDEX_HQ) == BST_CHECKED);
    }
    
    void reset() override {
        uSetDlgItemText(m_hWnd, IDC_YANDEX_TOKEN, "");
    }
    
private:
    const preferences_page_callback::ptr m_callback;
};

class preferences_page_yandeximpl : public preferences_page_impl<CYandexPreferences> {
public:
    const char * get_name() { return "Yandex Music"; }
    GUID get_guid() {
        static const GUID guid = { 0xa1b2c3d4, 0x1234, 0x4321, { 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff } };
        return guid;
    }
    GUID get_parent_guid() { return preferences_page::guid_tools; }
};
static preferences_page_factory_t<preferences_page_yandeximpl> g_preferences_page_yandeximpl_factory;


// -----------------------------------------------------------------------------
// UI Element (Search Panel)
// -----------------------------------------------------------------------------
struct SearchResult {
    std::string url;
    std::string id;
    std::string title;
    std::string artist;
    bool is_track;
};

class CYandexUI : public CDialogImpl<CYandexUI>, public ui_element_instance {
public:
    CYandexUI(ui_element_config::ptr cfg, ui_element_instance_callback::ptr cb) : m_callback(cb) {}
    
    enum { IDD = IDD_YANDEX_UI };

    BEGIN_MSG_MAP_EX(CYandexUI)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_HANDLER_EX(IDC_YANDEX_SEARCH_BTN, BN_CLICKED, OnSearchBtn)
        NOTIFY_HANDLER_EX(IDC_YANDEX_RESULTS_LIST, NM_DBLCLK, OnListDblClk)
        MSG_WM_SIZE(OnSize)
    END_MSG_MAP()
    
    void initialize_window(HWND parent) { WIN32_OP(Create(parent) != NULL); }
    HWND get_wnd() { return m_hWnd; }
    void set_configuration(ui_element_config::ptr config) {}
    ui_element_config::ptr get_configuration() { return ui_element_config::g_create_empty(g_get_guid()); }
    
    static GUID g_get_guid() {
        static const GUID guid = { 0xb1b2c3d4, 0x2234, 0x5321, { 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff } };
        return guid;
    }
    static GUID g_get_subclass() { return ui_element_subclass_utility; }
    static void g_get_name(pfc::string_base & out) { out = "Yandex Music Search"; }
    static ui_element_config::ptr g_get_default_configuration() { return ui_element_config::g_create_empty(g_get_guid()); }
    static const char * g_get_description() { return "Search tracks and albums on Yandex Music."; }

    BOOL OnInitDialog(CWindow, LPARAM) {
        CheckRadioButton(IDC_YANDEX_RADIO_TRACK, IDC_YANDEX_RADIO_ALBUM, IDC_YANDEX_RADIO_TRACK);
        CListViewCtrl list(GetDlgItem(IDC_YANDEX_RESULTS_LIST));
        list.InsertColumn(0, _T("Title"), LVCF_FMT | LVCF_WIDTH, 150, 0);
        list.InsertColumn(1, _T("Artist"), LVCF_FMT | LVCF_WIDTH, 150, 1);
        list.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        return TRUE;
    }
    
    void OnSize(UINT nType, CSize size) {
        CWindow list(GetDlgItem(IDC_YANDEX_RESULTS_LIST));
        if (list.m_hWnd) {
            CRect rcList;
            list.GetWindowRect(&rcList);
            ScreenToClient(&rcList);
            rcList.right = size.cx - 4;
            rcList.bottom = size.cy - 4;
            list.MoveWindow(&rcList);
        }
        CWindow searchEdit(GetDlgItem(IDC_YANDEX_SEARCH_EDIT));
        CWindow searchBtn(GetDlgItem(IDC_YANDEX_SEARCH_BTN));
        if (searchEdit.m_hWnd && searchBtn.m_hWnd) {
            CRect rcBtn;
            searchBtn.GetWindowRect(&rcBtn);
            ScreenToClient(&rcBtn);
            rcBtn.left = size.cx - rcBtn.Width() - 4;
            rcBtn.right = size.cx - 4;
            searchBtn.MoveWindow(&rcBtn);
            
            CRect rcEdit;
            searchEdit.GetWindowRect(&rcEdit);
            ScreenToClient(&rcEdit);
            rcEdit.right = rcBtn.left - 4;
            searchEdit.MoveWindow(&rcEdit);
        }
    }
    
    void OnSearchBtn(UINT, int, CWindow) {
        pfc::string8 query = uGetDlgItemText(m_hWnd, IDC_YANDEX_SEARCH_EDIT);
        if (query.is_empty()) return;
        
        bool isAlbum = IsDlgButtonChecked(IDC_YANDEX_RADIO_ALBUM) == BST_CHECKED;
        
        CListViewCtrl list(GetDlgItem(IDC_YANDEX_RESULTS_LIST));
        list.DeleteAllItems();
        m_results.clear();
        
        try {
            std::string type = isAlbum ? "album" : "track";
            std::string encoded_query = url_encode(query.get_ptr());
            std::wstring wpath = pfc::stringcvt::string_wide_from_utf8(("/search?type=" + type + "&page=0&text=" + encoded_query).c_str()).get_ptr();
            std::wstring wtoken = pfc::stringcvt::string_wide_from_utf8(cfg_yandex_token.get_ptr()).get_ptr();
            
            std::string response = YandexAPI::HttpRequest(L"api.music.yandex.net", wpath, wtoken);
            if (response.empty()) {
                MessageBox(_T("Empty response from API"), _T("Search Error"), MB_OK | MB_ICONERROR);
                return;
            }
            
            auto j = nlohmann::json::parse(response);
            
            if (j.contains("error")) {
                std::string err = j["error"].is_string() ? j["error"].get<std::string>() : (j["error"].contains("name") ? j["error"]["name"].get<std::string>() : "Unknown API error");
                MessageBox(pfc::stringcvt::string_os_from_utf8(err.c_str()), _T("Yandex API Error"), MB_OK | MB_ICONERROR);
                return;
            }
            
            if (isAlbum) {
                if(j.contains("result") && j["result"].contains("albums") && j["result"].at("albums").contains("results")) {
                    for (auto& item : j["result"]["albums"]["results"]) {
                        std::string title = item["title"].get<std::string>();
                        std::string artist = "Unknown Artist";
                        if (!item["artists"].empty()) {
                            artist = item["artists"][0]["name"].get<std::string>();
                        }
                        std::string id = item["id"].is_string() ? item["id"].get<std::string>() : (item["id"].is_number() ? std::to_string(item["id"].get<int>()) : "");
                        
                        int idx = list.InsertItem(list.GetItemCount(), pfc::stringcvt::string_os_from_utf8(title.c_str()));
                        list.SetItemText(idx, 1, pfc::stringcvt::string_os_from_utf8(artist.c_str()));
                        SearchResult r;
                        r.url = "yandex://album/" + id;
                        r.id = id;
                        r.title = title;
                        r.artist = artist;
                        r.is_track = false;
                        m_results.push_back(r);
                    }
                }
            } else {
                auto tracks = j["result"]["tracks"]["results"];
                for (auto& track : tracks) {
                    if (track.contains("title") && track.contains("id")) {
                        std::string title = track["title"].get<std::string>();
                        std::string id = track["id"].is_number() ? std::to_string(track["id"].get<int>()) : track["id"].get<std::string>();
                        std::string artist = "";
                        if (track.contains("artists") && track["artists"].is_array() && track["artists"].size() > 0) {
                            artist = track["artists"][0]["name"].get<std::string>();
                        }
                        
                        int idx = list.InsertItem(list.GetItemCount(), pfc::stringcvt::string_os_from_utf8(title.c_str()));
                        list.SetItemText(idx, 1, pfc::stringcvt::string_os_from_utf8(artist.c_str()));
                        SearchResult r;
                        r.url = "yandex://track/" + id;
                        r.id = id;
                        r.title = title;
                        r.artist = artist;
                        r.is_track = true;
                        m_results.push_back(r);
                    }
                }
            }
        } catch(const std::exception& e) {
            MessageBox(pfc::stringcvt::string_os_from_utf8(e.what()), _T("Search Error"), MB_OK | MB_ICONERROR);
        }
    }
    
    LRESULT OnListDblClk(LPNMHDR pnmh) {
          LPNMITEMACTIVATE pnmia = (LPNMITEMACTIVATE)pnmh;
          if (pnmia->iItem >= 0 && (size_t)pnmia->iItem < m_results.size()) {
              const SearchResult& res = m_results[pnmia->iItem];
              std::string final_url = res.url;
              
              if (res.is_track) {
                  std::string wtoken = cfg_yandex_token.get_ptr();
                  std::wstring wtoken_wide = pfc::stringcvt::string_wide_from_utf8(wtoken.c_str()).get_ptr();
                  std::wstring wpath = pfc::stringcvt::string_wide_from_utf8(("/tracks/" + res.id + "/download-info").c_str()).get_ptr();
                  std::string info_resp = YandexAPI::HttpRequest(L"api.music.yandex.net", wpath, wtoken_wide);
                  std::string final_codec = "mp3";
                  bool want_hq = cfg_yandex_hq.get();
                  if (!info_resp.empty()) {
                      try {
                          auto j = nlohmann::json::parse(info_resp);
                          for (auto& stream : j["result"]) {
                              std::string codec = stream["codec"].get<std::string>();
                              if (want_hq && codec == "flac") {
                                  final_codec = "flac";
                                  break;
                              }
                          }
                      } catch (...) {}
                  }
                  final_url += "." + final_codec;
              }

              const char* p_url = final_url.c_str();
              pfc::list_single_ref_t<const char*> url_list(p_url);
              static_api_ptr_t<playlist_manager> pm;
              pm->activeplaylist_add_locations(url_list, false, core_api::get_main_window());
              
              if (res.is_track) {
                  // Metadata is now fetched automatically by yandex_input
              }
          }
          return 0;
      }
    
    void notify(const GUID & p_what, t_size p_param1, const void * p_param2, t_size p_param2size) {}

    const ui_element_instance_callback::ptr m_callback;

private:
    std::vector<SearchResult> m_results;
};

static service_factory_single_t<ui_element_impl_withpopup<CYandexUI>> g_yandex_ui_factory;

void force_link_yandex_ui() {}
