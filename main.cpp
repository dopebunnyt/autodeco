#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
	constexpr float GRID = 30.f;
	// Object ID used for deco pieces (basic block). Made non-solid below via "no touch".
	// Swap this for any decoration object ID you like.
	constexpr int DECO_ID = 1;

	using Cell = std::pair<int, int>;

	Cell toCell(CCPoint p) {
		return { (int)std::floor(p.x / GRID), (int)std::floor(p.y / GRID) };
	}

	CCPoint cellCenter(Cell c) {
		return { c.first * GRID + GRID / 2, c.second * GRID + GRID / 2 };
	}

	GameObject* place(LevelEditorLayer* lel, CCPoint pos, float sx, float sy, float rot,
	                  int colorID, int zOrder) {
		auto obj = lel->createObject(DECO_ID, pos, false);
		if (!obj) return nullptr;
		obj->m_isNoTouch = true; // decoration only, no hitbox
		obj->updateCustomScaleX(sx);
		obj->updateCustomScaleY(sy);
		if (rot != 0.f) obj->setRotation(rot);
		if (obj->m_baseColor) obj->m_baseColor->m_colorID = colorID;
		obj->m_zOrder = zOrder;
		return obj;
	}

	int runAutoDeco(LevelEditorLayer* lel) {
		auto mod = Mod::get();
		auto style = mod->getSettingValue<std::string>("style");
		float density = (float)mod->getSettingValue<double>("density");
		float thick = (float)mod->getSettingValue<double>("thickness");
		int radius = (int)mod->getSettingValue<int64_t>("scatter-radius");
		int colorID = (int)mod->getSettingValue<int64_t>("color-id");
		int z = (int)mod->getSettingValue<int64_t>("z-order");
		bool hazards = mod->getSettingValue<bool>("include-hazards");
		int64_t seed = mod->getSettingValue<int64_t>("seed");

		std::mt19937 rng(seed != 0 ? (unsigned)seed : std::random_device{}());
		std::uniform_real_distribution<float> chance(0.f, 1.f);
		std::uniform_real_distribution<float> jitter(-GRID / 3, GRID / 3);

		// Snapshot level objects first so we never decorate our own deco.
		std::set<Cell> solid, occupied;
		std::vector<Cell> hazardCells;
		for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
			if (!obj || obj->m_isNoTouch) continue;
			Cell c = toCell(obj->getPosition());
			occupied.insert(c);
			if (obj->m_objectType == GameObjectType::Solid) solid.insert(c);
			else if (hazards && obj->m_objectType == GameObjectType::Hazard) {
				solid.insert(c);
			}
		}

		int count = 0;
		auto empty = [&](int x, int y) { return !occupied.count({x, y}); };

		if (style == "Border") {
			for (auto [x, y] : solid) {
				CCPoint c = cellCenter({x, y});
				float off = GRID / 2;
				float t = thick;
				if (empty(x, y + 1) && chance(rng) < density) count += place(lel, {c.x, c.y + off}, 1.f, t, 0, colorID, z) != nullptr;
				if (empty(x, y - 1) && chance(rng) < density) count += place(lel, {c.x, c.y - off}, 1.f, t, 0, colorID, z) != nullptr;
				if (empty(x + 1, y) && chance(rng) < density) count += place(lel, {c.x + off, c.y}, t, 1.f, 0, colorID, z) != nullptr;
				if (empty(x - 1, y) && chance(rng) < density) count += place(lel, {c.x - off, c.y}, t, 1.f, 0, colorID, z) != nullptr;
			}
		}
		else if (style == "Corners") {
			for (auto [x, y] : solid) {
				CCPoint c = cellCenter({x, y});
				float off = GRID / 2;
				for (int dx : {-1, 1}) for (int dy : {-1, 1}) {
					// corner is exposed if both adjacent sides are open
					if (empty(x + dx, y) && empty(x, y + dy) && chance(rng) < density) {
						count += place(lel, {c.x + dx * off, c.y + dy * off}, thick, thick, 45, colorID, z) != nullptr;
					}
				}
			}
		}
		else if (style == "Scatter") {
			std::set<Cell> done;
			for (auto [x, y] : solid) {
				for (int dx = -radius; dx <= radius; dx++) for (int dy = -radius; dy <= radius; dy++) {
					Cell n{x + dx, y + dy};
					if (occupied.count(n) || done.count(n)) continue;
					done.insert(n);
					if (chance(rng) < density * 0.25f) {
						CCPoint p = cellCenter(n);
						float s = thick * (0.5f + chance(rng));
						count += place(lel, {p.x + jitter(rng), p.y + jitter(rng)}, s, s,
						               chance(rng) * 90.f, colorID, z) != nullptr;
					}
				}
			}
		}
		else if (style == "Spikes") {
			for (auto [x, y] : solid) {
				if (!empty(x, y + 1)) continue;
				CCPoint c = cellCenter({x, y});
				// a few thin vertical shards along the top edge
				for (int i = -1; i <= 1; i++) {
					if (chance(rng) >= density) continue;
					float h = thick * (1.f + chance(rng) * 1.5f);
					count += place(lel, {c.x + i * 9.f, c.y + GRID / 2 + h * GRID / 2},
					               thick * 0.5f, h, 0, colorID, z) != nullptr;
				}
			}
		}
		return count;
	}
}

class $modify(AutoDecoPauseLayer, EditorPauseLayer) {
	bool init(LevelEditorLayer* lel) {
		if (!EditorPauseLayer::init(lel)) return false;

		auto menu = CCMenu::create();
		menu->setID("auto-deco-menu"_spr);
		auto spr = ButtonSprite::create("Auto Deco");
		auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AutoDecoPauseLayer::onAutoDeco));
		btn->setID("auto-deco-button"_spr);
		menu->addChild(btn);

		auto win = CCDirector::sharedDirector()->getWinSize();
		menu->setPosition({ 70.f, win.height - 30.f });
		this->addChild(menu);
		return true;
	}

	void onAutoDeco(CCObject*) {
		createQuickPopup(
			"Auto Deco",
			"Decorate this level using your current <cy>Auto Deco</c> settings?\n"
			"(Change them in <cg>Geode > Mods > Auto Deco > Settings</c>.)",
			"Cancel", "Decorate",
			[this](auto, bool confirmed) {
				if (!confirmed) return;
				int n = runAutoDeco(m_editorLayer);
				Notification::create(fmt::format("Placed {} deco objects", n),
				                     NotificationIcon::Success)->show();
			}
		);
	}
};
