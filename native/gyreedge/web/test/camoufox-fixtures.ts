/*
 * A `test`/`expect` pair backed by Camoufox (a stealthy, minimalistic
 * Firefox build, github.com/apify/camoufox-js) instead of Playwright's
 * default Chromium, per this project's own QA requirement to verify
 * the MUD web client in a real Firefox-family browser.
 *
 * camoufox-js pins to `playwright-core <1.61.0` (its own
 * peerDependencies), which is why package.json pins `@playwright/test`
 * at ^1.60.0 rather than the newer line the rest of this repo might
 * otherwise use -- confirmed locally: 1.62.1 fails at browser launch
 * with a Camoufox protocol-schema mismatch (`Browser.setDefaultViewport`
 * rejects `isMobile`), 1.60.0 does not.
 *
 * `@playwright/test`'s own `use.launchOptions` config field is
 * synchronous, but `camoufox-js`'s `launchOptions()` is async (it
 * resolves the downloaded binary path and a fingerprint config), so
 * this fixture overrides the `browser` fixture directly instead of
 * setting `projects` in playwright.config.ts -- the documented pattern
 * for camoufox-js under the Playwright test runner.
 *
 * Run `npm run camoufox:fetch` once (downloads the Camoufox Firefox
 * build, ~660MB) before running this spec.
 */
import { test as base, expect, firefox } from "@playwright/test";
import { launchOptions } from "camoufox-js";

export const test = base.extend({
  browser: async ({}, use) => {
    const browser = await firefox.launch(await launchOptions({ headless: true }));
    await use(browser);
    await browser.close();
  },
});

export { expect };
