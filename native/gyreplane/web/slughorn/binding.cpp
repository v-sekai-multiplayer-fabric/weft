// RFD 0074: batched WASM binding over SlugHorn's core Atlas.
//
// The single-shape POC (../slughorn-wasm-poc/binding.cpp) proved the
// pipeline; this proves the next documented milestone, multi-shape
// batching, per that POC's own README: "a real integration would
// follow the example's attribute-per-instance approach for many
// shapes sharing one atlas." Six hand-authored star shapes (point
// count varies 5-10, so each is visibly distinct, not six copies of
// the same glyph) share one Atlas and one pair of curve/band
// textures, the same way many real glyphs from one font would.
//
// No FreeType yet, same as the single-shape POC -- that is still a
// separate, not-yet-covered milestone. This binding hand-authors
// shapes; it exercises the batching machinery, not glyph loading.

#include "slughorn/slughorn.hpp"

#include <emscripten/emscripten.h>

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace slughorn;

namespace {
	constexpr double kPi = 3.14159265358979323846;
	constexpr int kShapeCount = 6;

	std::unique_ptr<Atlas> g_atlas;
	std::vector<Key> g_keys;

	// A star with `points` outer/inner vertex pairs, em-space [0,1]x[0,1].
	// Point count varies per shape so the six are visibly distinguishable
	// in a screenshot, not six identical copies proving nothing about
	// per-shape data actually varying across the batch.
	void addStar(Atlas& atlas, const Key& key, int points) {
		Atlas::ShapeInfo info;
		info.autoMetrics = true;

		constexpr double cx = 0.5, cy = 0.5;
		constexpr double rOuter = 0.48, rInner = 0.19;
		const int n = points * 2;

		std::vector<double> px(n), py(n);
		for(int i = 0; i < n; i++) {
			double ang = -kPi / 2.0 + i * kPi / points;
			double r = (i % 2 == 0) ? rOuter : rInner;
			px[i] = cx + r * std::cos(ang);
			py[i] = cy + r * std::sin(ang);
		}

		for(int i = 0; i < n; i++) {
			int j = (i + 1) % n;
			Atlas::Curve c;
			c.x1 = static_cast<slug_t>(px[i]);
			c.y1 = static_cast<slug_t>(py[i]);
			c.x2 = static_cast<slug_t>((px[i] + px[j]) / 2.0);
			c.y2 = static_cast<slug_t>((py[i] + py[j]) / 2.0);
			c.x3 = static_cast<slug_t>(px[j]);
			c.y3 = static_cast<slug_t>(py[j]);
			info.curves.push_back(c);
		}

		atlas.addShape(key, info);
	}
}

extern "C" {

// Builds one atlas holding kShapeCount distinct shapes. Call once
// before any of the accessors below.
EMSCRIPTEN_KEEPALIVE
void slughorn_buildBatchAtlas() {
	g_atlas = std::make_unique<Atlas>(512);
	g_keys.clear();

	for(int i = 0; i < kShapeCount; i++) {
		std::string name = "star" + std::to_string(i);
		g_keys.emplace_back(name.c_str());
		// 5..10 points, one distinct value per shape.
		addStar(*g_atlas, g_keys.back(), 5 + i);
	}

	g_atlas->build();
}

EMSCRIPTEN_KEEPALIVE int slughorn_shapeCount() { return kShapeCount; }

EMSCRIPTEN_KEEPALIVE const uint8_t* slughorn_curveTexPtr() { return g_atlas->getCurveTextureData().bytes.data(); }
EMSCRIPTEN_KEEPALIVE int slughorn_curveTexLen() { return static_cast<int>(g_atlas->getCurveTextureData().bytes.size()); }
EMSCRIPTEN_KEEPALIVE int slughorn_curveTexWidth() { return static_cast<int>(g_atlas->getCurveTextureData().width); }
EMSCRIPTEN_KEEPALIVE int slughorn_curveTexHeight() { return static_cast<int>(g_atlas->getCurveTextureData().height); }

EMSCRIPTEN_KEEPALIVE const uint8_t* slughorn_bandTexPtr() { return g_atlas->getBandTextureData().bytes.data(); }
EMSCRIPTEN_KEEPALIVE int slughorn_bandTexLen() { return static_cast<int>(g_atlas->getBandTextureData().bytes.size()); }
EMSCRIPTEN_KEEPALIVE int slughorn_bandTexWidth() { return static_cast<int>(g_atlas->getBandTextureData().width); }
EMSCRIPTEN_KEEPALIVE int slughorn_bandTexHeight() { return static_cast<int>(g_atlas->getBandTextureData().height); }

EMSCRIPTEN_KEEPALIVE
int slughorn_atlasTexWidthLog2() {
	return static_cast<int>(std::log2(static_cast<double>(g_atlas->getTextureWidth())));
}

// Per-shape fields, read one float at a time, matching the single-shape
// POC's own accessor shape, extended with a shapeIdx argument.
// idx: 0=bandTexX 1=bandTexY 2=bandMaxX 3=bandMaxY 4=bandScaleX 5=bandScaleY
//      6=bandOffsetX 7=bandOffsetY 8=bearingX 9=bearingY 10=width 11=height
EMSCRIPTEN_KEEPALIVE
float slughorn_shapeField(int shapeIdx, int idx) {
	if(shapeIdx < 0 || shapeIdx >= static_cast<int>(g_keys.size())) return 0.0f;
	auto shape = g_atlas->getShape(g_keys[shapeIdx]);
	if(!shape) return 0.0f;

	switch(idx) {
		case 0: return static_cast<float>(shape->bandTexX);
		case 1: return static_cast<float>(shape->bandTexY);
		case 2: return static_cast<float>(shape->bandMaxX);
		case 3: return static_cast<float>(shape->bandMaxY);
		case 4: return shape->bandScaleX;
		case 5: return shape->bandScaleY;
		case 6: return shape->bandOffsetX;
		case 7: return shape->bandOffsetY;
		case 8: return shape->bearingX;
		case 9: return shape->bearingY;
		case 10: return shape->width;
		case 11: return shape->height;
		default: return 0.0f;
	}
}

} // extern "C"
