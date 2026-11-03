// yaml.hpp — the YAML subset the bot configs use, resolved like PyYAML's
// yaml.safe_load (YAML 1.1): `no`/`off` are booleans, `0123` is octal, `1e-4`
// (no dot) is a STRING, `-.5` is a string. Supported: block mappings and
// sequences (including indentless sequences), flow [..] and {..} (may span
// lines), plain / single- / double-quoted scalars, comments, a leading `---`.
// Not supported (throws): anchors/aliases, tags, block scalars (| >).
// Timestamps resolve to their source text (the engine never reads one).
#pragma once

#include <stdexcept>
#include <string>

#include "json.hpp"

namespace yamlite {

struct YamlError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

mj::Value parse(const std::string& text);
mj::Value parse_file(const std::string& path);

// Resolve a plain scalar the way PyYAML's implicit resolvers do.
mj::Value resolve_plain(const std::string& s);

}  // namespace yamlite
