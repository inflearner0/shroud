#include "shroud/Config.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>

namespace shroud {

static std::string trim(const std::string &s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

static bool parseU64(const std::string &s, uint64_t &out) {
  if (s.empty()) return false;
  errno = 0;
  char *end = nullptr;
  unsigned long long v = std::strtoull(s.c_str(), &end, 0);
  if (errno != 0 || end == s.c_str() || *end != '\0') return false;
  out = static_cast<uint64_t>(v);
  return true;
}

static bool parseBool(const std::string &s, bool &out) {
  if (s == "1" || s == "true" || s == "yes" || s == "on") {
    out = true;
    return true;
  }
  if (s == "0" || s == "false" || s == "no" || s == "off") {
    out = false;
    return true;
  }
  return false;
}

std::vector<std::string> splitSpec(const std::string &spec) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : spec) {
    if (c == ';') {
      out.push_back(trim(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(trim(cur));
  return out;
}

bool isShroudToken(const std::string &token) {
  return token == "shroud" || token.rfind("shroud:", 0) == 0;
}

bool isShroudSpec(const std::string &spec) {
  for (const std::string &token : splitSpec(spec))
    if (isShroudToken(token)) return true;
  return false;
}

bool globMatch(const std::string &pattern, const std::string &name) {
  size_t p = 0;
  size_t n = 0;
  size_t star = std::string::npos;
  size_t mark = 0;
  while (n < name.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
      ++p;
      ++n;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = n;
    } else if (star != std::string::npos) {
      p = star + 1;
      n = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

Plan parsePlan(const std::string &spec, const Plan &base) {
  Plan plan = base;
  for (std::string token : splitSpec(spec)) {
    if (token.empty()) continue;
    if (token == "shroud") {
      plan.enabled = true;
      continue;
    }
    if (token.rfind("shroud:", 0) == 0) {
      plan.enabled = true;
      token = trim(token.substr(7));
      if (token.empty()) continue;
    }
    size_t eq = token.find('=');
    if (eq == std::string::npos) {
      if (token == "opaque")
        plan.opaque = true;
      else if (token == "bogus")
        plan.bogus = true;
      else if (token == "flatten")
        plan.flatten = true;
      else if (token == "virtualize")
        plan.virtualize = true;
      else if (token == "strings")
        plan.strings = true;
      else
        plan.unknown.push_back(token);
      continue;
    }
    std::string key = trim(token.substr(0, eq));
    std::string value = trim(token.substr(eq + 1));
    bool b = false;
    uint64_t u = 0;
    if (key == "opaque" && parseBool(value, b))
      plan.opaque = b;
    else if (key == "bogus" && parseBool(value, b))
      plan.bogus = b;
    else if (key == "bpred" && parseBool(value, b))
      plan.bpred = b;
    else if (key == "flatten" && parseBool(value, b))
      plan.flatten = b;
    else if (key == "virtualize" && parseBool(value, b))
      plan.virtualize = b;
    else if (key == "strings" && parseBool(value, b))
      plan.strings = b;
    else if (key == "consts" && parseBool(value, b))
      plan.consts = b;
    else if (key == "mba" && parseU64(value, u))
      plan.mbaDepth = static_cast<uint32_t>(u);
    else if (key == "seed" && parseU64(value, u))
      plan.seed = u;
    else
      plan.unknown.push_back(key);
  }
  return plan;
}

}
