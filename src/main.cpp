#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <vector>
#include <Geode/utils/file.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <random>
#include <algorithm>
#include "Season.hpp"
#include "Guard.hpp"
#include <globed/soft-link/API.hpp>

using namespace geode::prelude;

// ---------- helpers ----------
static void sfx(char const* name) { FMODAudioEngine::sharedEngine()->playEffect(name); }

static int  getMMR()    { return Mod::get()->getSavedValue<int>("mmr", 0); }
static int  getStreak() { return Mod::get()->getSavedValue<int>("streak", 0); }
static std::string getSyncKey() {
    auto key = Mod::get()->getSavedValue<std::string>("sync-key", "");
    if (key.empty()) {
        std::random_device rd; std::mt19937_64 g(rd());
        key = fmt::format("GDSA-{:08X}-{:08X}", (uint32_t)g(), (uint32_t)g());
        Mod::get()->setSavedValue("sync-key", key);
    }
    return key;
}
static std::string serverUrl() {
    auto u = Mod::get()->getSettingValue<std::string>("server-url");
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u;
}
static std::string jsonEscape(std::string const& in) {
    std::string out;
    for (char c : in) { if (c == '"' || c == '\\') out += '\\'; if ((unsigned char)c >= 32) out += c; }
    return out;
}
static int getWins()   { return Mod::get()->getSavedValue<int>("wins", 0); }
static int getLosses() { return Mod::get()->getSavedValue<int>("losses", 0); }

// Pending match (ghost or Globed friend); result is reported afterwards.
struct PendingMatch { bool active = false; std::string name; int mmr = 0; bool ghost = true; };
static PendingMatch g_pending;
static async::TaskHolder<web::WebResponse> g_report;

// Upload this player's stats to the shared leaderboard (Cloudflare Worker).
static void uploadStats(bool notify) {
    auto* acc = GJAccountManager::get();
    auto body = fmt::format(
        "{{\"account_id\":{},\"name\":\"{}\",\"mmr\":{},\"wins\":{},\"losses\":{},\"platform\":\"{}\",\"region\":\"ID\"}}",
        acc->m_accountID, jsonEscape(std::string(acc->m_username)), getMMR(), getWins(), getLosses(), GEODE_PLATFORM_NAME);
    auto req = web::WebRequest();
    req.bodyString(body);
    g_report.spawn(req.post(serverUrl() + "/report"), [notify](web::WebResponse res) {
        if (notify || !res.ok())
            Notification::create(res.ok() ? "Leaderboard updated" : "Leaderboard upload failed",
                res.ok() ? NotificationIcon::Success : NotificationIcon::Error)->show();
    });
}

// Apply a match result (shared by the Report button and the automatic ghost result).
static void applyResult(bool won) {
    if (!g_pending.active) return;
    int me = getMMR();
    int delta = gdsa::mmrDelta(me, g_pending.mmr, won, getStreak());
    int now = std::max(0, me + delta);
    Mod::get()->setSavedValue("mmr", now);
    Mod::get()->setSavedValue("streak", won ? getStreak() + 1 : 0);
    if (won) Mod::get()->setSavedValue("wins", getWins() + 1);
    else Mod::get()->setSavedValue("losses", getLosses() + 1);
    g_pending.active = false;
    sfx(won ? "gold02.ogg" : "playSound_01.ogg");
    Notification::create(fmt::format("{}{} MMR (now {})", delta >= 0 ? "+" : "", delta, now),
        won ? NotificationIcon::Success : NotificationIcon::Warning)->show();
    uploadStats(false);
}

// ---------- Overlay (SafePlay / Ghost opacity / Delta / Streamer) ----------
class OverlayPopup : public Popup {
protected:
    bool init() {
        if (!Popup::init(300.f, 240.f)) return false;
        setTitle("GDSA Overlay");
        auto menu = CCMenu::create();
        m_mainLayer->addChildAtPosition(menu, Anchor::Center);
        float y = 45;
        for (auto [id, label] : std::vector<std::pair<std::string, std::string>>{
                 {"safeplay", "SafePlay 240Hz"}, {"live-delta", "Live Delta Lead"}, {"streamer-mode", "Streamer Mode"}}) {
            auto t = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(OverlayPopup::onToggle), 0.7f);
            t->setUserObject("id", CCString::create(id));
            t->toggle(Mod::get()->getSettingValue<bool>(id));
            t->setPosition({-90, y});
            auto l = CCLabelBMFont::create(label.c_str(), "bigFont.fnt");
            l->setScale(0.45f); l->setAnchorPoint({0, 0.5f}); l->setPosition({-65, y});
            menu->addChild(t); menu->addChild(l);
            y -= 32;
        }
        auto sl = Slider::create(this, menu_selector(OverlayPopup::onGhost), 0.8f);
        sl->setPosition({0, -55});
        sl->setValue((Mod::get()->getSettingValue<double>("ghost-opacity") - 0.1) / 0.9);
        menu->addChild(sl);
        auto gl = CCLabelBMFont::create("Ghost Opacity", "bigFont.fnt");
        gl->setScale(0.4f); gl->setPosition({0, -35}); menu->addChild(gl);
        return true;
    }
    void onToggle(CCObject* s) {
        auto t = static_cast<CCMenuItemToggler*>(s);
        auto id = std::string(static_cast<CCString*>(t->getUserObject("id"))->getCString());
        Mod::get()->setSettingValue<bool>(id, !t->isToggled());
        sfx("playSound_01.ogg");
    }
    void onGhost(CCObject* s) {
        float v = static_cast<SliderThumb*>(s)->getValue();
        Mod::get()->setSettingValue<double>("ghost-opacity", 0.1 + v * 0.9);
    }
public:
    static OverlayPopup* create() {
        auto r = new OverlayPopup();
        if (r->init()) { r->autorelease(); return r; }
        delete r; return nullptr;
    }
};

// ---------- Main Ranked popup ----------
class RankedPopup : public Popup {
    CCLabelBMFont* m_season = nullptr;
    CCLabelBMFont* m_timer = nullptr;
    CCLabelBMFont* m_rank = nullptr;
    async::TaskHolder<web::WebResponse> m_listener;

    void refresh() {
        auto s = gdsa::currentSeason();
        m_season->setString(s.demon ? "Season: DEMON" : "Season: BASIC");
        m_season->setColor(s.demon ? ccColor3B{255, 70, 70} : ccColor3B{80, 200, 255});
        m_timer->setString(("Next rotation: " + gdsa::formatCountdown(s.secondsLeft)).c_str());
        auto& t = gdsa::tierFor(getMMR());
        m_rank->setString(fmt::format("{}  |  {} MMR  |  streak {}", t.name, getMMR(), getStreak()).c_str());
    }
    void tick(float) { refresh(); }

    void addRow(float y, char const* bg, std::vector<std::pair<char const*, SEL_MenuHandler>> const& items) {
        auto menu = CCMenu::create();
        menu->setLayout(RowLayout::create()->setGap(6)->setAutoScale(false));
        menu->setContentSize({m_size.width - 20, 40});
        for (auto& [text, handler] : items) {
            menu->addChild(CCMenuItemSpriteExtra::create(
                ButtonSprite::create(text, 90, true, "bigFont.fnt", bg, 30, 0.5f), this, handler));
        }
        menu->updateLayout();
        m_mainLayer->addChildAtPosition(menu, Anchor::Center, {0, y});
    }

    bool init() {
        if (!Popup::init(380.f, 260.f)) return false;
        setTitle("GDSA Ranked");
        m_season = CCLabelBMFont::create("", "goldFont.fnt"); m_season->setScale(0.7f);
        m_timer  = CCLabelBMFont::create("", "chatFont.fnt");
        m_rank   = CCLabelBMFont::create("", "bigFont.fnt"); m_rank->setScale(0.4f);
        m_mainLayer->addChildAtPosition(m_season, Anchor::Center, {0, 70});
        m_mainLayer->addChildAtPosition(m_timer,  Anchor::Center, {0, 50});
        m_mainLayer->addChildAtPosition(m_rank,   Anchor::Center, {0, 28});

        addRow(-20, "GJ_button_01.png", {
            {"Find Match", menu_selector(RankedPopup::onMatch)},
            {"Report", menu_selector(RankedPopup::onReport)},
            {"Leaderboard", menu_selector(RankedPopup::onBoard)}});
        addRow(-65, "GJ_button_04.png", {
            {"Guard", menu_selector(RankedPopup::onGuard)},
            {"Cloud Sync", menu_selector(RankedPopup::onSync)},
            {"Overlay", menu_selector(RankedPopup::onOverlay)}});

        refresh();
        this->schedule(schedule_selector(RankedPopup::tick), 1.f);
        return true;
    }

    void onMatch(CCObject*) {
        sfx("playSound_01.ogg");
        auto rep = gdsa::Guard::get().diagnostic(GJAccountManager::get()->m_accountID, getSyncKey());
        if (!rep.passed()) { FLAlertLayer::create("GDSA Guard", "Client failed integrity checks. Matchmaking blocked.", "OK")->show(); return; }
        createQuickPopup("Find Match",
            "<cy>Ghost</c>: rating-matched opponent (works offline). Result is automatic.\n"
            "<cg>Friend</c>: play a friend in a Globed room, then both report.",
            "Ghost", "Friend", [](FLAlertLayer*, bool friendMode) {
                static std::mt19937 rng{std::random_device{}()};
                static char const* names[] = {"Ghost Nova", "Ghost Lyra", "Ghost Orion", "Ghost Vega", "Ghost Kairo"};
                PendingMatch m;
                m.ghost = !friendMode;
                if (friendMode) { m.name = "Friend (Globed)"; m.mmr = getMMR(); }
                else {
                    m.name = names[rng() % 5];
                    m.mmr = std::max(0, getMMR() + (int)(rng() % 121) - 60);
                }
                createQuickPopup("Match Found",
                    fmt::format("Opponent: <cy>{}</c> ({} MMR)\nOpen any level to start. A match bar appears in-game.", m.name, m.mmr),
                    "Decline", "Accept", [m](FLAlertLayer*, bool accept) {
                        if (!accept) return;
                        g_pending = m; g_pending.active = true;
                        sfx("gold02.ogg");
                        Notification::create("Match started - open a level", NotificationIcon::Success)->show();
                    });
            });
    }

    void onReport(CCObject*) {
        if (!g_pending.active) {
            FLAlertLayer::create("Report", "No active match. Press Find Match first.", "OK")->show();
            return;
        }
        createQuickPopup("Report Result",
            fmt::format("Opponent: <cy>{}</c> ({} MMR)\nDid you win?", g_pending.name, g_pending.mmr),
            "I lost", "I won", [](FLAlertLayer*, bool won) { applyResult(won); });
    }

    void onBoard(CCObject*) {
        auto req = web::WebRequest();
        m_listener.spawn(req.get(serverUrl() + "/leaderboard?region=ID"), [](web::WebResponse res) {
            if (!res.ok()) { Notification::create("Leaderboard unavailable", NotificationIcon::Error)->show(); return; }
            auto json = res.json().unwrapOr(matjson::Value());
            std::string text; int i = 1;
            auto arr = json["players"].asArray();
            if (arr.isOk()) {
                for (auto& p : arr.unwrap()) {
                    text += fmt::format("{}. {} - {} MMR [{}] {}%\n", i++,
                        p["name"].asString().unwrapOr("?"), p["mmr"].asInt().unwrapOr(0),
                        p["platform"].asString().unwrapOr("?"), p["winrate"].asInt().unwrapOr(0));
                    if (i > 10) break;
                }
            }
            FLAlertLayer::create("Leaderboard (ID)", text.empty() ? "No players yet - play a match and report!" : text, "OK")->show();
        });
    }

    void onGuard(CCObject*) {
        auto r = gdsa::Guard::get().diagnostic(GJAccountManager::get()->m_accountID, getSyncKey());
        FLAlertLayer::create("GDSA Guard Diagnostic",
            fmt::format("Clock drift: {:.2f}% [{}]\nClick stddev: {:.2f} ms over {} clicks [{}]\nIntegrity CRC32: {:08X} [{}]\n\nToken:\n{}",
                r.drift * 100, r.clockOk ? "OK" : "FAIL", r.stddevMs, r.clicks, r.entropyOk ? "OK" : "FAIL",
                r.crc, r.integrityOk ? "OK" : "FAIL", r.token.substr(0, 32) + "\n" + r.token.substr(32)),
            "OK")->show();
    }
    void onSync(CCObject*) {
        uploadStats(true);
        FLAlertLayer::create("Cloud Sync Key", fmt::format("Your key:\n<cy>{}</c>", getSyncKey()), "OK")->show();
    }
    void onOverlay(CCObject*) { OverlayPopup::create()->show(); }

public:
    static RankedPopup* create() {
        auto r = new RankedPopup();
        if (r->init()) { r->autorelease(); return r; }
        delete r; return nullptr;
    }
};

// ---------- Hooks ----------
class $modify(GDSAMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        log::info("[gdsa.ranked] loaded, season index {}", gdsa::currentSeason().index);
        auto spr = ButtonSprite::create("GDSA", 50, true, "goldFont.fnt", "GJ_button_01.png", 30, 0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(GDSAMenu::onGDSA));
        btn->setID("gdsa-ranked-button"_spr);
        if (auto menu = this->getChildByID("bottom-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        }
        return true;
    }
    void onGDSA(CCObject*) { RankedPopup::create()->show(); }
};

// In-level match HUD: progress bar with "you" and "opponent" markers.
static constexpr float kBarW = 220.f;

class $modify(GDSAPlay, PlayLayer) {
    struct Fields {
        CCLabelBMFont* label = nullptr;
        CCLayerColor* meDot = nullptr;
        CCLayerColor* oppDot = nullptr;
        float elapsed = 0.f;
        float levelSecs = 60.f;
        float speed = 1.f;
        float oppPct = 0.f;
        bool ghost = false;
        bool hud = false;
    };

    void buildHud() {
        if (!g_pending.active) return;
        auto f = m_fields.self();
        f->hud = true;
        f->ghost = g_pending.ghost;
        // Rough level length: level units / base speed (ignores speed portals).
        float len = m_levelLength / 311.58f;
        f->levelSecs = len > 5.f ? len : 60.f;
        // Higher-rated ghost runs a bit faster, lower-rated a bit slower.
        f->speed = std::clamp(1.f + (g_pending.mmr - getMMR()) / 1500.f, 0.85f, 1.15f);

        auto win = CCDirector::get()->getWinSize();
        auto node = CCNode::create();
        node->setPosition({win.width / 2, win.height - 28.f});
        node->setID("gdsa-match-hud"_spr);

        auto bg = CCLayerColor::create({0, 0, 0, 150}, kBarW, 8.f);
        bg->setPosition({-kBarW / 2, -4.f});
        node->addChild(bg);

        f->oppDot = CCLayerColor::create({255, 80, 80, 255}, 6.f, 14.f);
        f->oppDot->setPosition({-kBarW / 2, -7.f});
        node->addChild(f->oppDot, 2);
        f->meDot = CCLayerColor::create({90, 255, 120, 255}, 6.f, 14.f);
        f->meDot->setPosition({-kBarW / 2, -7.f});
        node->addChild(f->meDot, 3);

        f->label = CCLabelBMFont::create("", "bigFont.fnt");
        f->label->setScale(0.3f);
        f->label->setPosition({0, -16.f});
        node->addChild(f->label);

        m_uiLayer->addChild(node, 50);

        // Friend mode: make sure far-away players keep sending their position.
        if (!f->ghost && globed::api::available()) globed::api::game::toggleCullingEnabled(false);
    }

    void updateHud(float dt) {
        auto f = m_fields.self();
        if (!f->hud || !g_pending.active) return;
        f->elapsed += dt;
        float me = std::clamp(this->getCurrentPercent(), 0.f, 100.f);

        if (f->ghost) {
            f->oppPct = std::clamp(f->elapsed * f->speed / (f->levelSecs * 1.15f) * 100.f, 0.f, 100.f);
            f->oppDot->setPositionX(-kBarW / 2 + kBarW * f->oppPct / 100.f - 3.f);
            float diff = me - f->oppPct;
            f->label->setString(fmt::format("You {:.0f}%  |  {} {:.0f}%  |  {}{:.0f}%",
                me, g_pending.name, f->oppPct, diff >= 0 ? "+" : "", diff).c_str());
        } else {
            // Friend mode: read the leading Globed player's position from the Globed API.
            bool found = false;
            if (globed::api::available() && globed::api::game::isActive() && m_levelLength > 1.f) {
                float best = -1.f;
                std::string bestName;
                for (auto const& rp : globed::api::game::getPlayers()) {
                    auto* vp = globed::api::player::getFirst(rp.get());
                    if (!vp) continue;
                    // VisualPlayer is a node; Globed docs say casting to CCNode* is safe.
                    auto* node = reinterpret_cast<CCNode*>(vp);
                    float pct = std::clamp(node->getPositionX() / m_levelLength * 100.f, 0.f, 100.f);
                    if (pct > best) { best = pct; bestName = globed::api::player::getUsername(rp.get()); }
                }
                if (best >= 0.f) {
                    found = true;
                    f->oppPct = best;
                    f->oppDot->setVisible(true);
                    f->oppDot->setPositionX(-kBarW / 2 + kBarW * best / 100.f - 3.f);
                    float diff = me - best;
                    f->label->setString(fmt::format("You {:.0f}%  |  {} {:.0f}%  |  {}{:.0f}%",
                        me, bestName, best, diff >= 0 ? "+" : "", diff).c_str());
                }
            }
            if (!found) {
                f->oppDot->setVisible(false);
                f->label->setString(fmt::format("You {:.0f}%  |  Waiting for Globed players...  |  {:.0f}s",
                    me, f->elapsed).c_str());
            }
        }
        f->meDot->setPositionX(-kBarW / 2 + kBarW * me / 100.f - 3.f);
    }

    bool init(GJGameLevel* lvl, bool a, bool b) {
        gdsa::Guard::get().resetLevel();
        if (!PlayLayer::init(lvl, a, b)) return false;
        buildHud();
        return true;
    }
    void onQuit() {
        if (m_fields->hud && !m_fields->ghost && globed::api::available())
            globed::api::game::toggleCullingEnabled(true);
        PlayLayer::onQuit();
    }
    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->elapsed = 0.f;
    }
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        gdsa::Guard::get().onFrame(dt);
        updateHud(dt);
    }
    void levelComplete() {
        auto f = m_fields.self();
        if (f->hud && g_pending.active) {
            if (f->ghost) {
                bool won = f->oppPct < 100.f;   // finished before the ghost did
                Notification::create(won ? "You beat the ghost!" : "Ghost finished first",
                    won ? NotificationIcon::Success : NotificationIcon::Warning)->show();
                applyResult(won);
            } else {
                Notification::create("Level complete - press Report in GDSA menu", NotificationIcon::Success)->show();
            }
        }
        PlayLayer::levelComplete();
    }
};

class $modify(GDSAInput, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (down) gdsa::Guard::get().onClick();
    }
};
