#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
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

	constexpr float DOT_BASE_SIZE = 12.f; // rough size of the dot object at scale 1; tune if glows look too big/small

	// Pads, orbs and portals that get a glow halo. Edit freely.
	constexpr int GLOW_OTHER_IDS[] = {
		35, 67, 140, 1332,                              // pads
		36, 84, 141, 1022, 1330, 1333, 1704, 1751,      // orbs
		10, 11, 12, 13, 45, 46, 47, 99, 101, 111, 286, 287, 660, 745, 747, 1331 // portals
	};

	bool isGlowOtherId(int id) {
		for (int g : GLOW_OTHER_IDS) if (g == id) return true;
		return false;
	}

	struct GlowTarget { float x, y, d; };

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

	// Saw blade object IDs. 88/89/98/397-399 are the old black saws; 1705-1710 are
	// their recolorable versions. Edit this list if a saw in your level isn't recolored.
	constexpr int SAW_IDS[] = { 88, 89, 98, 397, 398, 399, 1705, 1706, 1707, 1708, 1709, 1710 };

	bool isSawId(int id) {
		for (int sid : SAW_IDS) if (sid == id) return true;
		return false;
	}

	// Old black saws can't be recolored, so swap them for their recolorable twins.
	int recolorableId(int id) {
		switch (id) {
			case 88: return 1705;
			case 89: return 1706;
			case 98: return 1707;
			case 397: return 1708;
			case 398: return 1709;
			case 399: return 1710;
			default: return id;
		}
	}

	struct Side { int dx, dy; float rot; };
	// top, bottom, right, left (rotation = which way the piece points)
	const Side SIDES[4] = { {0, 1, 0.f}, {0, -1, 180.f}, {1, 0, 90.f}, {-1, 0, 270.f} };

	int runAutoDeco(LevelEditorLayer* lel, int& rethemed) {
		rethemed = 0;
		auto mod = Mod::get();
		auto style = mod->getSettingValue<std::string>("style");
		float density = (float)mod->getSettingValue<double>("density");
		float size = (float)mod->getSettingValue<double>("size");
		float variation = (float)mod->getSettingValue<double>("variation");
		int radius = (int)mod->getSettingValue<int64_t>("scatter-radius");
		int color1 = (int)mod->getSettingValue<int64_t>("color-id");
		int color2 = (int)mod->getSettingValue<int64_t>("color-id-2");
		int color3 = (int)mod->getSettingValue<int64_t>("color-id-3");
		int z = (int)mod->getSettingValue<int64_t>("z-order");
		bool behind = mod->getSettingValue<bool>("behind-blocks");
		bool hazards = mod->getSettingValue<bool>("include-hazards");
		bool replaceOld = mod->getSettingValue<bool>("replace-old");
		bool hellRetheme = mod->getSettingValue<bool>("hell-retheme");
		bool glowOn = mod->getSettingValue<bool>("glow");
		float glowStrength = (float)mod->getSettingValue<double>("glow-strength");
		int glowCh = (int)mod->getSettingValue<int64_t>("glow-color-id");
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

		// Hell palette: a list of color channels from darkest to brightest. Colors vary
		// smoothly across the level (so reds flow from one shade to another) with a bit
		// of randomness, instead of every piece being the same red.
		std::vector<int> pal;
		{
			std::string ps = mod->getSettingValue<std::string>("hell-palette");
			size_t from = 0;
			while (from <= ps.size()) {
				size_t comma = ps.find(',', from);
				std::string tokStr = ps.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
				int v = std::atoi(tokStr.c_str());
				if (v > 0 && v < 1000) pal.push_back(v);
				if (comma == std::string::npos) break;
				from = comma + 1;
			}
		}
		if (pal.size() < 2) pal = { color1, color2, color3 };
		// lo/hi pick which part of the palette to use (0 = darkest, 1 = brightest)
		auto palIdx = [&](float x, float y, float lo, float hi) -> size_t {
			float t = 0.5f + 0.5f * std::sin(x / 430.f + y / 270.f + 1.3f);
			t += rnd(-0.12f, 0.12f);
			t = std::clamp(lo + (hi - lo) * t, 0.f, 0.999f);
			return (size_t)(t * pal.size());
		};
		auto palPick = [&](float x, float y, float lo, float hi) {
			return pal[palIdx(x, y, lo, hi)];
		};

		// ---- Scan the level ----
		std::vector<GameObject*> oldDeco;
		std::set<Cell> solid, blocked;
		std::vector<std::pair<GameObject*, bool>> retheme; // (object, isSaw)
		std::vector<GlowTarget> glowTargets;
		for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
			if (!obj) continue;
			if (replaceOld && isDecoId(obj->m_objectID)) {
				oldDeco.push_back(obj);
				continue;
			}
			{
				int oid = obj->m_objectID;
				CCPoint gp = obj->getPosition();
				if (isSawId(oid)) {
					glowTargets.push_back({ gp.x, gp.y, 70.f * std::max(obj->getScaleX(), 0.3f) });
				}
				else if (obj->m_objectType == GameObjectType::Hazard) {
					glowTargets.push_back({ gp.x, gp.y, 34.f });
				}
				else if (isGlowOtherId(oid)) {
					glowTargets.push_back({ gp.x, gp.y, 44.f });
				}
			}
			if (hellRetheme && style == "Hell") {
				bool isSaw = isSawId(obj->m_objectID);
				if (isSaw || obj->m_objectType == GameObjectType::Solid) {
					retheme.push_back({ obj, isSaw });
				}
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

		// ---- Hell theme for existing blocks and saws ----
		// Each object is rebuilt from its own save data with new colors, so every other
		// property (position, rotation, scale, groups, triggers' targets) is kept.
		// Blocks: main color + detail color. Saws: dark blade with an accent-color glow.
		if (!retheme.empty()) {
			std::string all;
			for (auto const& [obj, saw] : retheme) {
				std::string src = std::string(obj->getSaveString(lel));
				CCPoint op = obj->getPosition();
				while (!src.empty() && src.back() == ';') src.pop_back();

				std::vector<std::string> tok;
				size_t start = 0;
				while (true) {
					size_t comma = src.find(',', start);
					if (comma == std::string::npos) { tok.push_back(src.substr(start)); break; }
					tok.push_back(src.substr(start, comma - start));
					start = comma + 1;
				}

				for (size_t i = 0; i + 1 < tok.size(); i += 2) {
					int key = std::atoi(tok[i].c_str());
					if (key == 21 || key == 22 || key == 96) continue; // drop old colors; 96 = 'no glow'
					std::string val = tok[i + 1];
					if (key == 1 && saw) val = std::to_string(recolorableId(std::atoi(val.c_str())));
					all += tok[i] + "," + val + ",";
				}
				int baseCh, detailCh;
				if (saw) {
					baseCh = palPick(op.x, op.y, 0.f, 0.4f);   // dark blade
					detailCh = palPick(op.x, op.y, 0.7f, 1.f); // hot core
				} else {
					size_t pi = palIdx(op.x, op.y, 0.f, 0.55f); // dark-ish block
					baseCh = pal[pi];
					detailCh = pal[std::min(pi + 1, pal.size() - 1)]; // one shade brighter
				}
				all += fmt::format("21,{},22,{};", baseCh, detailCh);
			}
			for (auto const& [obj, saw] : retheme) lel->removeObject(obj, true);
			lel->createObjectsFromString(all, true, true);
			rethemed = (int)retheme.size();
		}

		// ---- Output builder ----
		// Keys: 1=id 2=x 3=y 6=rotation 21=main color 22=detail color
		//       24=z layer 25=z order 32=scale
		std::string data;
		int count = 0;
		int zNow = z, c1Now = color1, c2Now = color2;
		auto put = [&](int id, float x, float y, float rot, float scale, int col1, int col2,
		               int zz, int layer, bool blend) {
			scale = std::clamp(scale, 0.05f, 20.f);
			data += fmt::format(
				"1,{},2,{:.2f},3,{:.2f},6,{:.1f},32,{:.3f},21,{},22,{},25,{}",
				id, x, y, rot, scale, col1, col2, zz
			);
			if (layer != INT_MIN) data += fmt::format(",24,{}", layer);
			if (blend) data += ",17,1";
			data += ';';
			count++;
		};
		auto add = [&](int id, float x, float y, float rot, float scale) {
			scale = std::clamp(scale, 0.05f, 4.f);
			put(id, x, y, rot, scale, c1Now, c2Now, zNow, behind ? 1 : INT_MIN, false);
			// glow: a soft halo behind some pieces
			if (glowOn && roll(0.3f * glowStrength)) {
				put(DOT_ID, x, y, 0.f, (14.f * scale * 2.2f) / DOT_BASE_SIZE,
				    glowCh, glowCh, z - 4, -1, true);
			}
		};
		// Bigger glow around existing level objects (saws, spikes, pads, orbs, portals).
		auto halo = [&](float x, float y, float diameter) {
			float sc = diameter / DOT_BASE_SIZE;
			put(DOT_ID, x, y, 0.f, sc, glowCh, glowCh, z - 4, -1, true);
			if (glowStrength > 0.35f) put(DOT_ID, x, y, 0.f, sc * 1.7f, glowCh, glowCh, z - 5, -1, true);
			if (glowStrength > 0.7f) put(DOT_ID, x, y, 0.f, sc * 2.6f, glowCh, glowCh, z - 6, -1, true);
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

		// ---- Level bounds (used for big structural shapes) ----
		int minX = INT_MAX, maxX = INT_MIN, minY = INT_MAX, maxY = INT_MIN;
		for (auto [x, y] : solid) {
			minX = std::min(minX, x); maxX = std::max(maxX, x);
			minY = std::min(minY, y); maxY = std::max(maxY, y);
		}
		float levelLeft = minX * GRID, levelRight = (maxX + 1) * GRID;
		float levelBottom = minY * GRID, levelTop = (maxY + 1) * GRID;

		// Random pick from a 3-color palette (main / detail / accent).
		auto palette = [&]() {
			float r = rnd(0.f, 1.f);
			if (r < 0.45f) { c1Now = color1; c2Now = color2; }
			else if (r < 0.8f) { c1Now = color2; c2Now = color3; }
			else { c1Now = color3; c2Now = color1; }
		};

		// ===== TROPICAL: bright, flowing, multicolor =====
		// Foliage clusters on tops, grass fringe, rising bubble streams.
		auto tidalCell = [&](Cell c) {
			auto const& s = SIDES[0];
			if (!exposed(c, s)) return;
			CCPoint ctr = cellCenter(c);

			if (roll(density * 0.3f)) { // tropical foliage cluster
				CCPoint base = sidePoint(c, s, 0.f, 4.f);
				int n = 3 + (int)rnd(0.f, 2.99f);
				for (int i = 0; i < n; i++) {
					palette();
					float sc = size * rnd(0.8f, 1.5f);
					add(pick(BUSH_IDS), base.x + (i - n / 2) * 9.f + rnd(-3.f, 3.f),
					    base.y + sc * 5.f + rnd(0.f, 4.f), rnd(-35.f, 35.f) * variation, sc);
				}
			}
			if (roll(density * 0.5f)) { // grass fringe
				palette();
				float sc = size * rnd(0.8f, 1.3f);
				CCPoint p = sidePoint(c, s, rnd(-11.f, 11.f), sc * 6.f);
				add(pick(GRASS_IDS), p.x, p.y, tilt(15.f), sc);
			}
			if (roll(density * 0.06f)) { // bubble stream rising and growing
				float x = ctr.x + rnd(-8.f, 8.f);
				float y = ctr.y + GRID / 2 + rnd(8.f, 20.f);
				int n = 4 + (int)rnd(0.f, 4.f);
				for (int i = 0; i < n; i++) {
					palette();
					x += std::sin(i * 1.3f) * 3.f;
					y += 11.f + i * 3.f;
					add(DOT_ID, x, y, 0.f, size * (0.3f + 0.12f * i));
				}
			}
		};

		// Two long wavy ribbons of dots flowing across the whole level.
		auto tidalRibbons = [&]() {
			if (levelRight - levelLeft < GRID * 4) return;
			for (int k = 0; k < 2; k++) {
				float mid = levelBottom + (levelTop - levelBottom) * (0.35f + 0.3f * k);
				float amp = 25.f + 15.f * k;
				float period = 240.f + 80.f * k;
				float phase = rnd(0.f, 6.28f);
				for (float x = levelLeft; x < levelRight; x += 14.f) {
					float y = mid + amp * std::sin(x / period * 6.2832f + phase);
					if (blocked.count(toCell({ x, y })) || !roll(density * 0.9f)) continue;
					palette();
					add(DOT_ID, x, y, 0.f, size * (0.35f + 0.25f * (0.5f + 0.5f * std::sin(x / 60.f))));
				}
			}
		};

		// ===== HELL: dark, sharp, glowing embers =====
		// Long hanging thorns with glowing tips, spiky shard fans, rising embers.
		auto griefCell = [&](Cell c) {
			CCPoint ctr = cellCenter(c);
			{ // hanging thorns: beads ending in a sharp tip
				auto const& s = SIDES[1];
				if (exposed(c, s) && roll(density * 0.55f)) {
					int len = 2 + (int)rnd(0.f, (float)vineLen);
					float x = ctr.x + rnd(-10.f, 10.f);
					float y = ctr.y - GRID / 2;
					for (int i = 0; i < len; i++) {
						bool last = (i == len - 1);
						float sc = last ? size * rnd(0.9f, 1.4f) : size * 0.35f;
						c1Now = (last && roll(0.6f)) ? palPick(x, y, 0.65f, 1.f) : palPick(x, y, 0.f, 0.45f);
						c2Now = palPick(x, y, 0.f, 0.3f);
						y -= last ? 10.f * sc : 7.f * size;
						x += rnd(-1.5f, 1.5f);
						if (last) add(pick(GRASS_IDS), x, y, 180.f + tilt(10.f), sc);
						else add(DOT_ID, x, y, 0.f, sc);
					}
				}
			}
			{ // shard fans: spiky clusters pointing up and out
				auto const& s = SIDES[0];
				if (exposed(c, s) && roll(density * 0.35f)) {
					CCPoint base = sidePoint(c, s, rnd(-8.f, 8.f), 2.f);
					int n = 2 + (int)rnd(0.f, 2.0f);
					for (int i = 0; i < n; i++) {
						float ang = (i - (n - 1) / 2.f) * 28.f * (0.6f + 0.4f * variation) + rnd(-6.f, 6.f);
						float sc = size * rnd(1.0f, 1.8f);
						c1Now = roll(0.3f) ? palPick(base.x, base.y, 0.65f, 1.f) : palPick(base.x, base.y, 0.f, 0.5f);
						c2Now = palPick(base.x, base.y, 0.f, 0.3f);
						add(pick(GRASS_IDS), base.x + (i - n / 2) * 4.f, base.y + sc * 6.f, ang, sc);
					}
				}
			}
			if (roll(density * 0.06f)) { // rising embers, shrinking as they drift up
				float x = ctr.x + rnd(-14.f, 14.f);
				float y = ctr.y + GRID / 2 + rnd(6.f, 14.f);
				int n = 4 + (int)rnd(0.f, 4.f);
				for (int i = 0; i < n; i++) {
					c1Now = palPick(x, y, 0.65f, 1.f);
					c2Now = palPick(x, y, 0.f, 0.3f);
					x += rnd(-4.f, 4.f);
					y += 10.f + i * 2.f;
					add(DOT_ID, x, y, 0.f, size * std::max(0.55f - 0.06f * i, 0.12f));
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
			else if (style == "Tropical") tidalCell(c);
			else if (style == "Hell") griefCell(c);
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
		if (style == "Tropical") tidalRibbons();
		c1Now = color1;
		c2Now = color2;

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

		// ---- Glow around existing blocks and objects ----
		if (glowOn) {
			for (auto [gx, gy] : solid) {
				Cell gc{ gx, gy };
				for (auto const& sd : SIDES) {
					if (!exposed(gc, sd)) continue;
					CCPoint gp = sidePoint(gc, sd, 0.f, 5.f);
					put(DOT_ID, gp.x, gp.y, 0.f, 44.f / DOT_BASE_SIZE, glowCh, glowCh, z - 4, -1, true);
					if (glowStrength > 0.7f && roll(0.3f)) {
						put(DOT_ID, gp.x, gp.y, 0.f, 76.f / DOT_BASE_SIZE, glowCh, glowCh, z - 5, -1, true);
					}
				}
			}
			for (auto const& t : glowTargets) halo(t.x, t.y, t.d);
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
				int rethemed = 0;
				int n = runAutoDeco(m_editorLayer, rethemed);
				if (n == 0) {
					Notification::create("No solid blocks found to decorate",
					                     NotificationIcon::Error)->show();
				} else {
					std::string msg = fmt::format("Placed {} deco pieces", n);
					if (rethemed > 0) msg += fmt::format(", recolored {} blocks and saws", rethemed);
					Notification::create(msg, NotificationIcon::Success)->show();
				}
			}
		);
	}
};
