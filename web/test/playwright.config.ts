import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: '.',
  timeout: 30000,
  retries: 0,
  reporter: [['list']],
  use: {
    // baseURL intentionally not set here -- each spec reads
    // MUD_BASE_URL itself and fails loudly if it is missing, rather
    // than silently defaulting to localhost and testing nothing real.
    trace: 'retain-on-failure',
  },
});
