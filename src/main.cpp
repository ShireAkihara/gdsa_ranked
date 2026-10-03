#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <vector>
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
// NOTE: online features (matchmaking, leaderboard, cloud upload) are stubbed until the
// server exists and the v5 async web API is wired in.
class RankedPopup : public Popup {
    CCLabelBMFont* m_season = nullptr;
    CCLabelBMFont* m_timer = nullptr;
    CCLabelBMFont* m_rank = nullptr;

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
            {"Leaderboard", menu_selector(RankedPopup::onBoard)},
            {"Guard", menu_selector(RankedPopup::onGuard)}});
        addRow(-65, "GJ_button_04.png", {
            {"Cloud Sync", menu_selector(RankedPopup::onSync)},
            {"Export", menu_selector(RankedPopup::onExport)},
            {"Overlay", menu_selector(RankedPopup::onOverlay)}});

        refresh();
        this->schedule(schedule_selector(RankedPopup::tick), 1.f);
        return true;
    }

    void onMatch(CCObject*) {
        sfx("playSound_01.ogg");
        auto rep = gdsa::Guard::get().diagnostic(GJAccountManager::get()->m_accountID, getSyncKey());
        if (!rep.passed()) { FLAlertLayer::create("GDSA Guard", "Client failed integrity checks. Matchmaking blocked.", "OK")->show(); return; }
        FLAlertLayer::create("Matchmaking", "Guard check passed.\nServer not connected yet.", "OK")->show();
    }
    void onBoard(CCObject*) {
        FLAlertLayer::create("Leaderboard", "Server not connected yet.", "OK")->show();
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
        FLAlertLayer::create("Cloud Sync Key", fmt::format("Your recovery key:\n<cy>{}</c>\n\nUpload needs the server.", getSyncKey()), "OK")->show();
    }
    void onExport(CCObject*) {
        auto path = Mod::get()->getSaveDir() / "gdsa.geode.json";
        auto j = fmt::format("{{\"key\":\"{}\",\"mmr\":{},\"streak\":{}}}", getSyncKey(), getMMR(), getStreak());
        auto r = utils::file::writeString(path, j);
        Notification::create(r.isOk() ? "Exported to save dir" : "Export failed",
            r.isOk() ? NotificationIcon::Success : NotificationIcon::Error)->show();
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
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (down) gdsa::Guard::get().onClick();
    }
};
