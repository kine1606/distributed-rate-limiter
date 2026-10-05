#pragma once

#include <string>

#include "ratelimiter/rule.h"

namespace ratelimiter {

// Parses a YAML rules file (see config/rules.yaml). Throws std::runtime_error with a
// message naming the offending rule/field on any error, so a bad SIGHUP reload can be
// rejected without touching the live table.
RuleTable LoadRules(const std::string& path);

}  // namespace ratelimiter
