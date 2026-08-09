# ![Rhendium logo](chrome/app/theme/chromium/product_logo_64.png) Rhendium

> Rhendium — Deterministic Chromium rendering, pixel for pixel.

Rhendium is a Chromium-based browser built to render ordinary web content the
same way on Windows, Linux, and macOS. It pins the browser engine and rendering
configuration, ships a verified Noto font pack, bypasses host font discovery
and glyph fallback for normal webpages, and applies one cross-platform text
rasterization protocol. A pinned KDE Chromium build is the visual design
reference, while Rhendium's own reproducible rendering output is the final
standard.

Rhendium intentionally keeps each operating system's default Chromium browser
interface. Cross-platform pixel consistency applies to webpage content, not to
the native browser chrome, menus, window frame, or platform integrations.

## Playwright

```sh
npm install --save-dev @playwright/test @rhendium/playwright
npx rhendium install
```

```js
import { defineConfig } from '@playwright/test';
import { rhendiumProject } from '@rhendium/playwright';

export default defineConfig({ projects: [rhendiumProject()] });
```

Run the tests with `npx playwright test --project=rhendium`; see
[`rhendium-playwright`](https://github.com/rhendium/rhendium-playwright) for
additional options.

## Upstream Chromium

Chromium is an open-source browser project that aims to build a safer, faster,
and more stable way for all users to experience the web.

The project's web site is https://www.chromium.org.

To check out the source code locally, don't use `git clone`! Instead,
follow [the instructions on how to get the code](docs/get_the_code.md).

Documentation in the source is rooted in [docs/README.md](docs/README.md).

Learn how to [Get Around the Chromium Source Code Directory
Structure](https://www.chromium.org/developers/how-tos/getting-around-the-chrome-source-code).

For historical reasons, there are some small top level directories. Now the
guidance is that new top level directories are for product (e.g. Chrome,
Android WebView, Ash). Even if these products have multiple executables, the
code should be in subdirectories of the product.

If you found a bug, please file it at https://crbug.com/new.
