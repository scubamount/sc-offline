// discord: "Playing sc-offline" on the player's Discord profile, as a plugin on the sco SDK.
//
// A plain SDK consumer, like plugins/creative: it includes only the SDK zip's headers and the
// Windows SDK, and does nothing the SDK does not offer any other plugin. It replaces the
// launcher's own Rich Presence (0.7.0-0.7.4), which hard-coded the version it was built with.
//
// What it shows, every value read at run time:
//   - details: where you are (teleport.spatial's zone name, "Daymar"), or "Star Citizen offline mod"
//   - state:   the ship you're aboard (game.vehicles + game.entities, "AEGS Gladius"), or the version
//   - large image text: the host's own version string, api->host_version() ("sc-offline v0.7.5")
// Settings (plugin.ini [settings], changed in the plugin's menu page): show_location, show_ship,
// show_buttons. Turning the plugin off (the launcher's Plugins page, or the `disabled` file in
// data/plugins/discord) clears the status.
//
// Threads: the game is read on the game thread (a tick, at most every kReadEveryMs); the Discord
// pipe is written on a thread of this plugin, which OnUnload stops. The two share one Snapshot
// under a mutex. Discord's local pipe (\\.\pipe\discord-ipc-N) is the only thing it talks to:
// nothing goes over the network. Without the Discord app (or under Wine, where the pipe usually
// isn't there) it retries every 15 s and logs once.
#include "scosdk/scosdk.hpp"
#include "scosdk/settings.hpp"
#include "scosdk/service.hpp"
#include "scosdk/game/vehicles.hpp"
#include "scosdk/game/entities.hpp"
#include "sc_spatial.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace {

constexpr const char* kAppId = "1557514943308242955";   // public; the sc-offline app's images live there
constexpr const char* kInvite = "https://discord.gg/NJKeVfYCCC";
constexpr const char* kDownload = "https://github.com/scubamount/sc-offline/releases/latest";
constexpr uint64_t kReadEveryMs = 5000;    // how often the tick reads the game
constexpr DWORD kRetryMs = 15000;          // Discord not there: try again this often

// "OOC_Stanton_2b_Daymar" -> "Daymar"; "" stays "".
std::string ZoneLabel(const char* zone) {
    std::string z = zone ? zone : "";
    const size_t cut = z.find_last_of('_');
    if (cut != std::string::npos && cut + 1 < z.size()) z = z.substr(cut + 1);
    return z;
}

// "AEGS_Gladius" -> "AEGS Gladius".
std::string ShipLabel(std::string cls) {
    for (char& c : cls) if (c == '_') c = ' ';
    return cls;
}

// Discord's JSON: quotes, backslashes and control characters escaped; 128 bytes at most (its limit).
std::string Json(const std::string& s) {
    std::string out;
    for (unsigned char c : s.substr(0, 120)) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += static_cast<char>(c);
    }
    return out;
}

struct Snapshot {
    std::string version;    // host_version()
    std::string place;      // "" until known
    std::string ship;       // "" when on foot
    bool showPlace = true, showShip = true, showButtons = true;
    bool operator==(const Snapshot&) const = default;
};

// ---- Discord's local pipe: frames of { int32 op, int32 length, JSON } ----------------------

bool PipeWrite(HANDLE pipe, uint32_t op, const std::string& json) {
    std::string frame(8, '\0');
    const uint32_t len = static_cast<uint32_t>(json.size());
    std::memcpy(&frame[0], &op, 4);
    std::memcpy(&frame[4], &len, 4);
    frame += json;
    DWORD put = 0;
    return WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &put, nullptr) && put == frame.size();
}

// One frame, giving up after timeoutMs so a stuck Discord can't hold the thread.
bool PipeRead(HANDLE pipe, std::string& json, DWORD timeoutMs) {
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    auto readN = [&](char* buf, DWORD n) {
        DWORD have = 0;
        while (have < n) {
            DWORD avail = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) return false;
            if (!avail) { if (GetTickCount64() > until) return false; Sleep(20); continue; }
            DWORD got = 0;
            const DWORD want = (avail < n - have) ? avail : n - have;
            if (!ReadFile(pipe, buf + have, want, &got, nullptr) || !got) return false;
            have += got;
        }
        return true;
    };
    char head[8];
    if (!readN(head, 8)) return false;
    uint32_t len = 0;
    std::memcpy(&len, head + 4, 4);
    if (len > 65536) return false;
    json.assign(len, '\0');
    return len == 0 || readN(&json[0], len);
}

HANDLE Connect() {
    for (int i = 0; i < 10; ++i) {
        const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return INVALID_HANDLE_VALUE;
}

std::string Activity(const Snapshot& s, long long start, uint64_t nonce) {
    const std::string details = (s.showPlace && !s.place.empty()) ? s.place : "Star Citizen offline mod";
    const std::string state = (s.showShip && !s.ship.empty()) ? "Aboard " + s.ship : s.version + " \xC2\xB7 single player";
    std::string a = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + std::to_string(nonce) + "\",\"args\":{\"pid\":" +
                    std::to_string(GetCurrentProcessId()) + ",\"activity\":{";
    a += "\"details\":\"" + Json(details) + "\",\"state\":\"" + Json(state) + "\",";
    a += "\"timestamps\":{\"start\":" + std::to_string(start) + "},";
    a += "\"assets\":{\"large_image\":\"sc_offline\",\"large_text\":\"" + Json(s.version) +
         "\",\"small_image\":\"sc_offline_small\",\"small_text\":\"Offline, single player\"}";
    if (s.showButtons)
        a += std::string(",\"buttons\":[{\"label\":\"Join the Discord\",\"url\":\"") + kInvite +
             "\"},{\"label\":\"Get sc-offline\",\"url\":\"" + kDownload + "\"}]";
    a += "}}}";
    return a;
}

class Discord : public sco::sdk::Plugin {
public:
    sco_result OnLoad() override {
        settings_.Open(*this);   // a host without sco.settings: the defaults below apply
        veh_.Open(*this);        // the game services are optional: no ship or place, the rest works
        ent_.Open(*this);
        spatial_.Query(*this, SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION);
        {
            std::lock_guard<std::mutex> lock(mu_);
            snap_.version = (Api()->host_version && Api()->host_version()) ? Api()->host_version() : "sc-offline";
            ReadSettings(snap_);
        }
        start_ = static_cast<long long>(std::time(nullptr));
        tick_ = Subscribe("tick", [this](const void*) { OnTick(); });
        changed_ = Subscribe(SCO_SETTINGS_CHANGED_EVENT, [this](const void*) {
            std::lock_guard<std::mutex> lock(mu_);
            ReadSettings(snap_);
        });
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_) return SCO_FAILED;
        thread_ = CreateThread(nullptr, 0, &Discord::ThreadMain, this, 0, nullptr);
        if (!thread_) { CloseHandle(stop_); stop_ = nullptr; return SCO_FAILED; }
        Info("showing %s on Discord when the Discord app is open", snap_.version.c_str());
        return SCO_OK;
    }

    void OnUnload() override {
        tick_ = {};
        changed_ = {};
        if (thread_) {
            SetEvent(stop_);
            WaitForSingleObject(thread_, 5000);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
    }

private:
    void ReadSettings(Snapshot& s) const {
        if (!settings_) return;
        s.showPlace = settings_.Bool("show_location", true);
        s.showShip = settings_.Bool("show_ship", true);
        s.showButtons = settings_.Bool("show_buttons", true);
    }

    // Game thread. Reads where you are and what you're aboard, at most every kReadEveryMs.
    void OnTick() {
        const uint64_t now = GetTickCount64();
        if (now - lastRead_ < kReadEveryMs) return;
        lastRead_ = now;
        std::string place, ship;
        if (spatial_) {
            double pos[3], rot[4];
            uint64_t zone = 0;
            char name[128] = {};
            if (spatial_->player_pose(pos, rot, &zone) && zone && spatial_->zone_name(zone, name, sizeof(name)))
                place = ZoneLabel(name);
        }
        uint64_t shipId = 0;
        std::string cls;
        if (veh_ && ent_ && Has("game.vehicles.seats") && veh_.PlayerShip(shipId) == SCO_OK && shipId &&
            ent_.ClassOf(shipId, cls) == SCO_OK)
            ship = ShipLabel(cls);
        std::lock_guard<std::mutex> lock(mu_);
        snap_.place = place;
        snap_.ship = ship;
    }

    static DWORD WINAPI ThreadMain(void* self) { static_cast<Discord*>(self)->Run(); return 0; }

    // Plugin thread: connect, keep the activity current, hold the pipe (closing it clears the status).
    void Run() {
        bool told = false;
        uint64_t nonce = 1;
        while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
            HANDLE pipe = Connect();
            if (pipe == INVALID_HANDLE_VALUE) {
                if (!told) { Info("Discord isn't running; trying again every %lu s", kRetryMs / 1000); told = true; }
                if (WaitForSingleObject(stop_, kRetryMs) == WAIT_OBJECT_0) return;
                continue;
            }
            std::string reply;
            const bool ready = PipeWrite(pipe, 0, std::string("{\"v\":1,\"client_id\":\"") + kAppId + "\"}") &&
                               PipeRead(pipe, reply, 3000) && reply.find("\"READY\"") != std::string::npos;
            Snapshot sent;
            bool shown = false;
            while (ready && WaitForSingleObject(stop_, 1000) == WAIT_TIMEOUT) {
                Snapshot now;
                { std::lock_guard<std::mutex> lock(mu_); now = snap_; }
                if (!shown || !(now == sent)) {
                    if (!PipeWrite(pipe, 1, Activity(now, start_, nonce++)) || !PipeRead(pipe, reply, 3000) ||
                        reply.find("\"ERROR\"") != std::string::npos) {
                        Warn("Discord refused the status");
                        break;
                    }
                    if (!shown) Info("showing \"Playing sc-offline\" (%s) on your Discord profile", now.version.c_str());
                    sent = now;
                    shown = true;
                }
                DWORD avail = 0;   // drain what Discord sends; a broken pipe means it quit
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) break;
                if (avail && !PipeRead(pipe, reply, 1000)) break;
            }
            CloseHandle(pipe);
            told = false;
            if (WaitForSingleObject(stop_, kRetryMs) == WAIT_OBJECT_0) return;
        }
    }

    sco::sdk::Settings settings_;
    sco::sdk::game::Vehicles veh_;
    sco::sdk::game::Entities ent_;
    sco::sdk::ServiceRef<sc_spatial_v1> spatial_;
    sco::sdk::Subscription tick_, changed_;
    std::mutex mu_;
    Snapshot snap_;
    long long start_ = 0;
    uint64_t lastRead_ = 0;
    HANDLE stop_ = nullptr, thread_ = nullptr;
};

}  // namespace

// id must equal plugin.ini's id.
SCO_PLUGIN(Discord, "discord", "1.0.0", "sc-offline");
