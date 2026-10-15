# Improvement ideas

Brief review; ideas are unverified beyond a skim of the tree.

1. ~~**Web: add `lint` and `test:run` scripts** (`web/package.json`). Only dev/build/preview exist; add ESLint plus Vitest for `api/sovdClient.ts`.~~ **Done.**
2. ~~**Web: connect() bypasses the API layer** (`web/src/App.vue`). It calls `fetch(url + '/')` directly; add a `getRoot()` method to `SovdClient`.~~ **Done.**
3. ~~**Web: persist connection settings** (`App.vue`). Remember base URL in localStorage and never persist the token.~~ **Done.**
4. ~~**Web: loading skeletons and retry buttons** in `views/FaultViewer.vue`, `DataTable.vue` and `LiveChart.vue`.~~ **Done.**
5. ~~**Web: LiveChart reconnect/backoff** (`views/LiveChart.vue`) for dropped streams, with a visible stream status.~~ **Done.**
6. ~~**Server: serve the built web UI** (`server/src/routes.cpp`). No static file mount exists today; an optional `--web-root` flag would remove the separate dev server and CORS setup.~~ **Done.**
7. **Server: split `routes.cpp`** by resource (data, faults, locks, streams) for readability and faster rebuilds.
8. **Client: unit tests with a mock server** (`client/`, `tests/`) covering 401/403 handling and retries.
9. ~~**CI: add a web job** (`bun install`, `bun run build`, lint and test) beside the C++ build.~~ **Done.**
10. ~~**Docs: web UI section** in `README.md` / `docs/USAGE.md` covering `VITE_SOVD_BASE_URL`, the token field and the domain-server-only constraint (D1).~~ **Done.**
