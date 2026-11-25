#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
	constexpr float GRID = 30.f;

	// Real decoration object IDs (from community object-ID references).
	// If one looks wrong in your game, change the number here.
	constexpr int GRASS_IDS[] = { 406, 407, 408, 414 }; // small grass tufts
	constexpr int BUSH_IDS[]  = { 123, 124, 125, 126, 127, 128 }; // bushes
	constexpr int DOT_ID   = 1764; // small dot
	constexpr int CLOUD_ID = 131;  // small cloud
	constexpr int PIXEL_ID = 1964; // tiny pixel piece

	using Cell = std::pair<int, int>;

	Cell toCell(CCPoint p) {
		return { (int)std::floor(p.x / GRID), (int)std::floor(p.y / GRID) };
	}

	CCPoint cellCenter(Cell c) {
		return { c.first * GRID + GRID / 2, c.second * GRID + GRID / 2 };
	}

	bool isDecoId(int id) {
		for (int g : GRASS_IDS) if (g == id) return true;
		for (int b : BUSH_IDS) if (b == id) return true;
		return id == DOT_ID || id == CLOUD_ID || id == PIXEL_ID;
	}

	struct Side { int dx, dy; float rot; };
	// top, bottom, right, left (rotation = which way the piece points)
	const Side SIDES[4] = { {0, 1, 0.f}, {0, -1, 180.f}, {1, 0, 90.f}, {-1, 0, 270.f} };

	int runAutoDeco(LevelEditorLayer* lel) {
		auto mod = Mod::get();
		auto style = mod->getSettingValue<std::string>("style");
		float density = (float)mod->getSettingValue<double>("density");
		float size = (float)mod->getSettingValue<double>("size");
		float variation = (float)mod->getSettingValue<double>("variation");
		int radius = (int)mod->getSettingValue<int64_t>("scatter-radius");
		int color1 = (int)mod->getSettingValue<int64_t>("color-id");
		int color2 = (int)mod->getSettingValue<int64_t>("color-id-2");
		int z = (int)mod->getSettingValue<int64_t>("z-order");
		bool behind = mod->getSettingValue<bool>("behind-blocks");
		bool hazards = mod->getSettingValue<bool>("include-hazards");
		bool replaceOld = mod->getSettingValue<bool>("replace-old");
		bool depthLayer = mod->getSettingValue<bool>("depth-layer");
		int vineLen = (int)mod->getSettingValue<int64_t>("vine-length");
		int64_t seed = mod->getSettingValue<int64_t>("seed");

		std::mt19937 rng(seed != 0 ? (unsigned)seed : std::random_device{}());
		auto rnd = [&](float a, float b) {
			return std::uniform_real_distribution<float>(a, b)(rng);
		};
		auto roll = [&](float p) { return rnd(0.f, 1.f) < p; };
		auto pick = [&](auto const& arr) {
			size_t n = std::size(arr);
			return arr[std::uniform_int_distribution<size_t>(0, n - 1)(rng)];
		};

		// ---- Scan the level ----
		std::vector<GameObject*> oldDeco;
		std::set<Cell> solid, blocked;
		for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
			if (!obj) continue;
			if (replaceOld && isDecoId(obj->m_objectID)) {
				oldDeco.push_back(obj);
				continue;
			}
			Cell c = toCell(obj->getPosition());
			if (obj->m_objectType == GameObjectType::Solid) {
				solid.insert(c);
				blocked.insert(c);
			}
			else if (obj->m_objectType == GameObjectType::Hazard) {
				blocked.insert(c);
				if (hazards) solid.insert(c);
			}
		}
		if (solid.empty()) return 0;

		for (auto o : oldDeco) lel->removeObject(o, true);

		// ---- Output builder ----
		// Keys: 1=id 2=x 3=y 6=rotation 21=main color 22=detail color
		//       24=z layer 25=z order 32=scale
		std::string data;
		int count = 0;
		int zNow = z, c1Now = color1, c2Now = color2;
		auto add = [&](int id, float x, float y, float rot, float scale) {
			scale = std::clamp(scale, 0.05f, 4.f);
			data += fmt::format(
				"1,{},2,{:.2f},3,{:.2f},6,{:.1f},32,{:.3f},21,{},22,{},25,{}",
				id, x, y, rot, scale, c1Now, c2Now, zNow
			);
			if (behind) data += ",24,1";
			data += ';';
			count++;
		};

		auto exposed = [&](Cell c, Side const& s) {
			return !blocked.count({ c.first + s.dx, c.second + s.dy });
		};
		// A point on a block's side: t slides along the edge, out pushes away from it.
		auto sidePoint = [&](Cell c, Side const& s, float t, float out) {
			CCPoint ctr = cellCenter(c);
			float tx = (float)std::abs(s.dy), ty = (float)std::abs(s.dx);
			return CCPoint{
				ctr.x + s.dx * (GRID / 2 + out) + tx * t,
				ctr.y + s.dy * (GRID / 2 + out) + ty * t
			};
		};
		auto tilt = [&](float maxDeg) { return rnd(-1.f, 1.f) * variation * maxDeg; };

		// ---- Techniques ----
		auto grass = [&](Cell c) {
			auto const& s = SIDES[0];
			if (!exposed(c, s)) return;
			int n = roll(0.5f) ? 2 : 1;
			for (int i = 0; i < n; i++) {
				if (!roll(density)) continue;
				float sc = size * rnd(0.7f, 1.2f);
				CCPoint p = sidePoint(c, s, rnd(-11.f, 11.f), sc * 6.f);
				add(pick(GRASS_IDS), p.x, p.y, tilt(15.f), sc);
			}
		};

		auto cornerBush = [&](Cell c) {
			if (!exposed(c, SIDES[0])) return;
			for (int sx : { -1, 1 }) {
				if (blocked.count({ c.first + sx, c.second })) continue;
				if (!roll(density * 0.35f)) continue;
				CCPoint ctr = cellCenter(c);
				float sc = size * rnd(0.5f, 0.9f);
				add(pick(BUSH_IDS), ctr.x + sx * GRID / 2, ctr.y + GRID / 2 + sc * 5.f,
				    tilt(20.f), sc);
			}
		};

		auto hanging = [&](Cell c) {
			auto const& s = SIDES[1];
			if (!exposed(c, s) || !roll(density * 0.5f)) return;
			float sc = size * rnd(0.5f, 0.9f);
			CCPoint p = sidePoint(c, s, rnd(-9.f, 9.f), sc * 5.f);
			add(pick(BUSH_IDS), p.x, p.y, 180.f + tilt(15.f), sc);
		};

		auto sideBush = [&](Cell c) {
			for (int i = 2; i < 4; i++) {
				auto const& s = SIDES[i];
				if (!exposed(c, s) || !roll(density * 0.2f)) continue;
				float sc = size * rnd(0.5f, 0.8f);
				CCPoint p = sidePoint(c, s, rnd(-9.f, 9.f), sc * 5.f);
				add(pick(BUSH_IDS), p.x, p.y, s.rot + tilt(15.f), sc);
			}
		};

		auto dots = [&](Cell c, bool includeTop) {
			for (int i = includeTop ? 0 : 1; i < 4; i++) {
				auto const& s = SIDES[i];
				if (!exposed(c, s)) continue;
				for (int k = 0; k < 2; k++) {
					if (!roll(density)) continue;
					float t = (k == 0 ? -8.f : 8.f) + rnd(-3.f, 3.f);
					CCPoint p = sidePoint(c, s, t, rnd(-2.f, 3.f));
					add(DOT_ID, p.x, p.y, 0.f, size * rnd(0.4f, 0.9f));
				}
			}
		};

		auto pixels = [&](Cell c) {
			for (auto const& s : SIDES) {
				if (!exposed(c, s)) continue;
				int n = roll(0.5f) ? 3 : 2;
				for (int k = 0; k < n; k++) {
					if (!roll(density)) continue;
					CCPoint p = sidePoint(c, s, rnd(-13.f, 13.f), rnd(0.f, 12.f));
					add(PIXEL_ID, p.x, p.y, rnd(0.f, 45.f) * variation, size * rnd(0.08f, 0.25f));
				}
			}
		};

		auto overgrown = [&](Cell c) {
			grass(c);
			cornerBush(c);
			hanging(c);
			sideBush(c);
			dots(c, false);
		};

		// Vines: short hanging strands under blocks, thinning toward the tip.
		auto vines = [&](Cell c) {
			auto const& s = SIDES[1];
			if (!exposed(c, s) || !roll(density * 0.4f)) return;
			int len = 1 + (int)rnd(0.f, (float)vineLen);
			CCPoint ctr = cellCenter(c);
			float x = ctr.x + rnd(-9.f, 9.f);
			float y = ctr.y - GRID / 2;
			for (int i = 0; i < len; i++) {
				float sc = std::max(size * (0.9f - 0.1f * i) * rnd(0.8f, 1.1f), 0.2f * size);
				y -= sc * 9.f;
				x += rnd(-2.5f, 2.5f);
				bool last = (i == len - 1);
				if (last) add(pick(BUSH_IDS), x, y, 180.f + tilt(15.f), sc * 0.8f);
				else add(DOT_ID, x, y, 0.f, sc * 0.6f);
			}
		};

		// Moss: grass growing out of every exposed side, thickest on top.
		auto moss = [&](Cell c) {
			for (int i = 0; i < 4; i++) {
				auto const& s = SIDES[i];
				if (!exposed(c, s)) continue;
				if (!roll(i == 0 ? density : density * 0.45f)) continue;
				float sc = size * rnd(0.6f, 1.1f);
				CCPoint p = sidePoint(c, s, rnd(-11.f, 11.f), sc * 6.f);
				add(pick(GRASS_IDS), p.x, p.y, s.rot + tilt(15.f), sc);
			}
		};

		// Studs: a dot on every outer corner, like rivets.
		auto studs = [&](Cell c) {
			CCPoint ctr = cellCenter(c);
			for (int sx : { -1, 1 }) {
				for (int sy : { -1, 1 }) {
					bool outer = !blocked.count({ c.first + sx, c.second }) &&
					             !blocked.count({ c.first, c.second + sy });
					if (!outer || !roll(density)) continue;
					add(DOT_ID, ctr.x + sx * (GRID / 2 - 4.f), ctr.y + sy * (GRID / 2 - 4.f),
					    0.f, size * rnd(0.5f, 0.7f));
				}
			}
		};

		// Clouds: small overlapping cloud banks floating above block tops.
		auto clouds = [&](Cell c) {
			auto const& s = SIDES[0];
			if (!exposed(c, s) || !roll(density * 0.12f)) return;
			CCPoint base = sidePoint(c, s, 0.f, rnd(25.f, 60.f));
			int n = 3 + (int)rnd(0.f, 2.99f);
			for (int i = 0; i < n; i++) {
				float sc = size * rnd(0.4f, 0.9f);
				add(CLOUD_ID, base.x + (i - n / 2) * 14.f + rnd(-4.f, 4.f),
				    base.y + rnd(-6.f, 6.f), tilt(8.f), sc);
			}
		};

		// Floaty: clouds and dots scattered in the air around blocks.
		auto floaty = [&](float mult, int extra, float scaleMult) {
			std::set<Cell> done;
			int r = radius + extra;
			for (auto [x, y] : solid) {
				for (int dx = -r; dx <= r; dx++) {
					for (int dy = -r; dy <= r; dy++) {
						Cell n{ x + dx, y + dy };
						if (blocked.count(n) || done.count(n)) continue;
						done.insert(n);
						if (!roll(density * 0.2f * mult)) continue;
						CCPoint p = cellCenter(n);
						p.x += rnd(-10.f, 10.f);
						p.y += rnd(-10.f, 10.f);
						if (roll(0.3f)) add(CLOUD_ID, p.x, p.y, tilt(10.f), size * rnd(0.3f, 0.7f) * scaleMult);
						else add(DOT_ID, p.x, p.y, 0.f, size * rnd(0.4f, 1.1f) * scaleMult);
					}
				}
			}
		};

		// ---- Apply the chosen style ----
		for (auto c : solid) {
			if (style == "Grass") { grass(c); cornerBush(c); }
			else if (style == "Moss") moss(c);
			else if (style == "Vines") { vines(c); grass(c); }
			else if (style == "Dots") dots(c, true);
			else if (style == "Studs") studs(c);
			else if (style == "Clouds") clouds(c);
			else if (style == "Overgrown") overgrown(c);
			else if (style == "Pixels") pixels(c);
			else if (style == "Mixed") {
				float r = rnd(0.f, 1.f);
				if (r < 0.25f) { grass(c); cornerBush(c); }
				else if (r < 0.4f) moss(c);
				else if (r < 0.55f) vines(c);
				else if (r < 0.7f) dots(c, true);
				else if (r < 0.85f) studs(c);
				else overgrown(c);
			}
		}
		if (style == "Floaty") floaty(1.f, 0, 1.f);
		if (style == "Mixed") floaty(0.4f, 0, 1.f);

		// Depth layer: big, sparse pieces far behind everything, in the detail color.
		if (depthLayer) {
			zNow = z - 6;
			c1Now = color2;
			c2Now = color1;
			floaty(0.6f, 2, 1.8f);
			zNow = z;
			c1Now = color1;
			c2Now = color2;
		}

		if (count > 0) {
			lel->createObjectsFromString(data, true, true);
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
				if (n == 0) {
					Notification::create("No solid blocks found to decorate",
					                     NotificationIcon::Error)->show();
				} else {
					Notification::create(fmt::format("Placed {} deco pieces", n),
					                     NotificationIcon::Success)->show();
				}
			}
		);
	}
};
