#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/file.hpp>
#include <random>
#include "Season.hpp"
#include "Guard.hpp"

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
static std::string serverUrl() { return Mod::get()->getSettingValue<std::string>("server-url"); }

// ---------- Overlay (SafePlay / Ghost opacity / Delta / Streamer) ----------
class OverlayPopup : public Popup<> {
protected:
    bool setup() override {
        setTitle("GDSA Overlay");
        auto menu = CCMenu::create();
        m_mainLayer->addChildAtPosition(menu, Anchor::Center);
        float y = 45;
        for (auto [id, label] : std::vector<std::pair<std::string, std::string>>{
                 {"safeplay", "SafePlay 240Hz"}, {"live-delta", "Live Delta Lead"}, {"streamer-mode", "Streamer Mode"}}) {
            auto t = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(OverlayPopup::onToggle), 0.7f);
            t->setTag(0); t->setUserObject("id", CCString::create(id));
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
        auto id = static_cast<CCString*>(t->getUserObject("id"))->getCString();
        // callback fires before the visual flip, so the new value is the inverse of isToggled()
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
        if (r->initAnchored(300, 240)) { r->autorelease(); return r; }
        delete r; return nullptr;
    }
};

// ---------- Main Ranked popup ----------
class RankedPopup : public Popup<> {
    CCLabelBMFont* m_season = nullptr;
    CCLabelBMFont* m_timer = nullptr;
    CCLabelBMFont* m_rank = nullptr;
    EventListener<web::WebTask> m_listener;
    std::chrono::steady_clock::time_point m_sent;

    void refresh() {
        auto s = gdsa::currentSeason();
        m_season->setString(s.demon ? "Season: DEMON" : "Season: BASIC");
        m_season->setColor(s.demon ? ccColor3B{255, 70, 70} : ccColor3B{80, 200, 255});
        m_timer->setString(("Next rotation: " + gdsa::formatCountdown(s.secondsLeft)).c_str());
        auto& t = gdsa::tierFor(getMMR());
        m_rank->setString(fmt::format("{}  |  {} MMR  |  streak {}", t.name, getMMR(), getStreak()).c_str());
    }
    void tick(float) { refresh(); }

    bool setup() override {
        setTitle("GDSA Ranked");
        auto size = m_mainLayer->getContentSize();
        m_season = CCLabelBMFont::create("", "goldFont.fnt"); m_season->setScale(0.7f);
        m_timer  = CCLabelBMFont::create("", "chatFont.fnt");
        m_rank   = CCLabelBMFont::create("", "bigFont.fnt"); m_rank->setScale(0.4f);
        m_mainLayer->addChildAtPosition(m_season, Anchor::Center, {0, 70});
        m_mainLayer->addChildAtPosition(m_timer,  Anchor::Center, {0, 50});
        m_mainLayer->addChildAtPosition(m_rank,   Anchor::Center, {0, 28});

        auto menu = CCMenu::create();
        menu->setLayout(RowLayout::create()->setGap(6)->setAutoScale(false));
        menu->setContentSize({size.width - 20, 40});
        struct B { char const* t; SEL_MenuHandler h; };
        for (auto b : {B{"Find Match", menu_selector(RankedPopup::onMatch)},
                       B{"Leaderboard", menu_selector(RankedPopup::onBoard)},
                       B{"Guard", menu_selector(RankedPopup::onGuard)}}) {
            menu->addChild(CCMenuItemSpriteExtra::create(
                ButtonSprite::create(b.t, 90, true, "bigFont.fnt", "GJ_button_01.png", 30, 0.5f), this, b.h));
        }
        menu->updateLayout();
        m_mainLayer->addChildAtPosition(menu, Anchor::Center, {0, -20});

        auto menu2 = CCMenu::create();
        menu2->setLayout(RowLayout::create()->setGap(6)->setAutoScale(false));
        menu2->setContentSize({size.width - 20, 40});
        for (auto b : {B{"Cloud Sync", menu_selector(RankedPopup::onSync)},
                       B{"Export", menu_selector(RankedPopup::onExport)},
                       B{"Overlay", menu_selector(RankedPopup::onOverlay)}}) {
            menu2->addChild(CCMenuItemSpriteExtra::create(
                ButtonSprite::create(b.t, 90, true, "bigFont.fnt", "GJ_button_04.png", 30, 0.5f), this, b.h));
        }
        menu2->updateLayout();
        m_mainLayer->addChildAtPosition(menu2, Anchor::Center, {0, -65});

        refresh();
        this->schedule(schedule_selector(RankedPopup::tick), 1.f);
        return true;
    }

    // --- matchmaking: server pairs within ~±150 MMR; ping measured on the request round trip ---
    void onMatch(CCObject*) {
        sfx("playSound_01.ogg");
        auto rep = gdsa::Guard::get().diagnostic(GJAccountManager::get()->m_accountID, getSyncKey());
        if (!rep.passed()) { FLAlertLayer::create("GDSA Guard", "Client failed integrity checks. Matchmaking blocked.", "OK")->show(); return; }
        m_sent = std::chrono::steady_clock::now();
        auto req = web::WebRequest();
        req.bodyJSON(matjson::makeObject({
            {"account_id", GJAccountManager::get()->m_accountID},
            {"mmr", getMMR()}, {"region", "SEA-JKT"}, {"token", rep.token}}));
        m_listener.bind([this](web::WebTask::Event* e) {
            if (auto* res = e->getValue()) {
                auto ping = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_sent).count();
                if (!res->ok()) { Notification::create("Matchmaking unavailable", NotificationIcon::Error)->show(); return; }
                auto json = res->json().unwrapOr(matjson::Value());
                auto opp = json["opponent"].asString().unwrapOr("???");
                auto oppMmr = json["mmr"].asInt().unwrapOr(getMMR());
                sfx("gold02.ogg");
                createQuickPopup("Match Found",
                    fmt::format("Opponent: <cy>{}</c> ({} MMR)\nPing: {} ms (Jakarta SEA)", opp, oppMmr, ping),
                    "Decline", "Accept", [oppMmr](auto, bool accept) {
                        if (accept) Notification::create(fmt::format("Accepted (opp {} MMR) - level loads when both ready", oppMmr),
                                                         NotificationIcon::Success)->show();
                    });
            }
        });
        m_listener.setFilter(req.post(serverUrl() + "/match/find"));
    }

    void onBoard(CCObject*) {
        auto req = web::WebRequest();
        m_listener.bind([](web::WebTask::Event* e) {
            if (auto* res = e->getValue()) {
                if (!res->ok()) { Notification::create("Leaderboard unavailable", NotificationIcon::Error)->show(); return; }
                auto json = res->json().unwrapOr(matjson::Value());
                std::string text; int i = 1;
                for (auto& p : json["players"].asArray().unwrapOr({})) {
                    text += fmt::format("{}. {} - {} MMR [{}]{}\n", i++,
                        p["name"].asString().unwrapOr("?"), p["mmr"].asInt().unwrapOr(0),
                        p["platform"].asString().unwrapOr("?"),
                        p["blacklisted"].asBool().unwrapOr(false) ? " BANNED" : "");
                    if (i > 10) break;
                }
                FLAlertLayer::create("Leaderboard (ID)", text.empty() ? "No data" : text, "OK")->show();
            }
        });
        m_listener.setFilter(req.get(serverUrl() + "/leaderboard?region=ID"));
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
        auto req = web::WebRequest();
        req.bodyJSON(matjson::makeObject({{"key", getSyncKey()}, {"mmr", getMMR()}, {"streak", getStreak()}}));
        m_listener.bind([](web::WebTask::Event* e) {
            if (auto* res = e->getValue())
                Notification::create(res->ok() ? "Cloud sync complete" : "Cloud sync failed",
                    res->ok() ? NotificationIcon::Success : NotificationIcon::Error)->show();
        });
        m_listener.setFilter(req.post(serverUrl() + "/sync"));
        FLAlertLayer::create("Cloud Sync Key", fmt::format("Your recovery key:\n<cy>{}</c>", getSyncKey()), "OK")->show();
    }

    void onExport(CCObject*) {
        auto path = Mod::get()->getSaveDir() / "gdsa.geode.json";
        auto j = matjson::makeObject({{"key", getSyncKey()}, {"mmr", getMMR()}, {"streak", getStreak()}});
        auto r = geode::utils::file::writeString(path, j.dump());
        Notification::create(r ? "Exported to save dir" : "Export failed",
            r ? NotificationIcon::Success : NotificationIcon::Error)->show();
    }

    void onOverlay(CCObject*) { OverlayPopup::create()->show(); }

public:
    static RankedPopup* create() {
        auto r = new RankedPopup();
        if (r->initAnchored(380, 260)) { r->autorelease(); return r; }
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

class $modify(GDSAPlay, PlayLayer) {
    bool init(GJGameLevel* lvl, bool a, bool b) {
        gdsa::Guard::get().resetLevel();
        return PlayLayer::init(lvl, a, b);
    }
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        gdsa::Guard::get().onFrame(dt);
    }
};

class $modify(GDSAInput, GJBaseGameLayer) {
    void pushButton(PlayerButton btn, bool p1) {
        GJBaseGameLayer::pushButton(btn, p1);
        gdsa::Guard::get().onClick();
    }
};
