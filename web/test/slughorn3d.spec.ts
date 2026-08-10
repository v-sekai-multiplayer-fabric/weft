import { test, expect } from '@playwright/test';

/*
 * End-to-end test for mud/web/index.html + mud3d.js -- the three.js MUD
 * room with SlugHorn's WASM/Slug-shader markers drawn into the same
 * WebGL2 context. This is now the only MUD web UI: the plain-text page
 * that used to live at mud/web/index.html (with mud.js) was removed,
 * and this three.js/SlugHorn view was promoted to the root path.
 *
 * What this asserts is deliberately the pipeline, not the prose: that
 * SlugHorn's real WASM module loaded, built a real Atlas (non-zero curve
 * and band texture dimensions read back out of wasm linear memory), and
 * that its fragment shader actually put pixels on the framebuffer.
 *
 * It does NOT assert readable text, because there is none: the shapes are
 * hand-authored star outlines (mud/web/slughorn/binding.cpp), not font
 * glyphs. Real glyph rendering needs a FreeType binding that does not
 * exist in SlugHorn upstream or here. A test that claimed otherwise would
 * be testing a lie.
 *
 * Headless WebGL2 needs a software rasteriser: the launchOptions below
 * turn on ANGLE/SwiftShader explicitly rather than hoping the CI runner
 * has a real GPU. Chromium, not the Camoufox/Firefox fixture the other
 * specs use, because SwiftShader is the dependable headless path for the
 * RGBA32F/RGBA16UI integer textures the Slug technique needs.
 *
 * MUD_BASE_URL must point at a real reachable instance -- no default, so
 * a missing env var fails loudly instead of silently testing nothing.
 */

const baseURL = process.env.MUD_BASE_URL;

test.use({
  launchOptions: {
    args: [
      '--use-gl=angle',
      '--use-angle=swiftshader',
      '--enable-unsafe-swiftshader',
    ],
  },
});

test.beforeAll(() => {
  if (!baseURL) {
    throw new Error('MUD_BASE_URL is not set -- point it at a real running instance');
  }
});

test('SlugHorn WASM builds a real atlas and the Slug shader draws pixels', async ({ page }) => {
  const errors: string[] = [];
  page.on('pageerror', (e) => errors.push('pageerror: ' + e.message));

  await page.goto(baseURL!);

  // window.__slughorn is only set after the wasm module instantiated, ran
  // Atlas::build(), and its curve/band textures were uploaded to WebGL2.
  await page.waitForFunction('window.__slughorn !== undefined', null, { timeout: 60000 });
  const atlas = await page.evaluate(() => (window as any).__slughorn);

  expect(atlas.shapeCount).toBeGreaterThan(0);
  expect(atlas.curveW).toBeGreaterThan(0);
  expect(atlas.bandW).toBeGreaterThan(0);
  // texWidthLog2 must match the Atlas(512) the binding constructs.
  expect(atlas.texWidthLog2).toBe(9);

  // Sample the framebuffer from inside a live frame (mud3d.js's own
  // readPixels hook). Reading a WebGL canvas from outside the frame that
  // drew it returns nothing once the browser has composited it.
  await page.evaluate(() => {
    (window as any).__slughornPixels = undefined;
    (window as any).__slughornWantCapture = true;
  });
  await page.waitForFunction('window.__slughornPixels !== undefined', null, { timeout: 30000 });
  const pixels = await page.evaluate(() => (window as any).__slughornPixels);

  // Strongly-hued pixels can only come from the Slug markers: the room's
  // own three.js geometry is near-grey by construction.
  expect(pixels.saturated).toBeGreaterThan(500);

  expect(errors).toEqual([]);
});

test('a real "look" drives the 3D room from the real backend', async ({ page }) => {
  await page.goto(baseURL!);
  // mud3d.js issues an opening "look" on load, so a real POST
  // /api/mud/command round trip should name the room in the HUD.
  await expect(page.locator('#roomName')).not.toHaveText('(no room yet)', { timeout: 30000 });
  await expect(page.locator('#log .turn')).not.toHaveCount(0);
});
