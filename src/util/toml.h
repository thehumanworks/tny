/* toml.h — tiny TOML subset for Codex/Grok config.toml MCP tables.
 * Not a general TOML library. See docs/adr/0051-mcp-import-from-harnesses.md. */
#ifndef TNY_TOML_H
#define TNY_TOML_H

#include "json/json.h"

/* Parse a bounded TOML subset into a yyjson object document:
 *   - comments (# to end of line)
 *   - dotted [tables] and [[arrays of tables]] (arrays of tables ignored)
 *   - string, integer, float, bool, and inline array of those
 *   - inline tables as objects
 * Nested keys (`a.b = 1`) create objects. Unknown / unparseable lines are
 * skipped. Returns NULL on allocation failure or when a recognized
 * `mcp_servers` field is malformed (over-nested, wrong type, bad env table);
 * an empty object is a successful empty parse. Caller yyjson_doc_free's. */
yyjson_doc *toml_parse_subset(const char *src, size_t len);

#endif
