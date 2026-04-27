// SM-2 spaced repetition for bigrams.
// Persists state to bigram_memory.json across sessions.

#pragma once
#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include "stats.h"

struct SM2State {
  int interval    = 1;    // days until next review
  int repetitions = 0;
  float ef        = 2.5f; // easiness factor
  std::string next_review; // "YYYY-MM-DD", empty = due now
};

class BigramMemory {
public:
  static std::string today() {
    std::time_t t = std::time(nullptr);
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&t));
    return std::string(buf);
  }

  // Weight for text generation: higher = higher priority in word selection
  float dueWeight(const std::string& bigram) const {
    auto it = states.find(bigram);
    if (it == states.end()) return 1.5f; // unseen bigrams get a mild boost
    const auto& s = it->second;
    if (s.next_review.empty() || s.next_review <= today())
      return 4.0f; // due or overdue
    return 0.3f;   // mastered, de-prioritize
  }

  // Map session bigram performance to SM-2 quality score (0–5)
  static int qualityFromStats(const BigramStats& s) {
    if (s.total < 3) return -1; // not enough data to judge
    float ms = s.avgMs();
    float er = s.errorRate();
    if (ms < 120 && er == 0.0f) return 5;
    if (ms < 180 && er < 0.05f) return 4;
    if (ms < 260 && er < 0.10f) return 3;
    if (ms < 380 && er < 0.20f) return 2;
    if (ms < 520 && er < 0.40f) return 1;
    return 0;
  }

  void update(const std::string& bigram, int quality) {
    auto& s = states[bigram];
    if (quality < 3) {
      s.repetitions = 0;
      s.interval    = 1;
    } else {
      if      (s.repetitions == 0) s.interval = 1;
      else if (s.repetitions == 1) s.interval = 6;
      else                         s.interval = (int)(s.interval * s.ef + 0.5f);

      s.ef += 0.1f - (5 - quality) * (0.08f + (5 - quality) * 0.02f);
      s.ef = std::max(1.3f, s.ef);
      s.repetitions++;
    }
    s.next_review = addDays(today(), s.interval);
  }

  void load(const std::string& filename) {
    std::ifstream f(filename);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
      if (line.find("\"b\":") == std::string::npos) continue;
      SM2State s;
      std::string bigram  = extractStr(line, "b");
      if (bigram.empty()) continue;
      s.interval    = std::stoi(safeNum(extractNum(line, "i")));
      s.repetitions = std::stoi(safeNum(extractNum(line, "r")));
      s.ef          = std::stof(safeNum(extractNum(line, "ef")));
      s.next_review = extractStr(line, "nr");
      states[bigram] = s;
    }
  }

  void save(const std::string& filename) const {
    std::ofstream f(filename);
    if (!f.is_open()) return;
    f << "{\n\"bigrams\": [\n";
    bool first = true;
    for (const auto& [bigram, s] : states) {
      if (!first) f << ",\n";
      first = false;
      f << "{\"b\":\"" << bigram << "\""
        << ",\"i\":"   << s.interval
        << ",\"r\":"   << s.repetitions
        << ",\"ef\":"  << std::fixed << std::setprecision(4) << s.ef
        << ",\"nr\":\"" << s.next_review << "\"}";
    }
    f << "\n]\n}\n";
  }

  std::map<std::string, SM2State> states;

private:
  static std::string addDays(const std::string& date, int days) {
    struct tm tm = {};
    strptime(date.c_str(), "%Y-%m-%d", &tm);
    tm.tm_mday += days;
    mktime(&tm);
    char buf[11];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return std::string(buf);
  }

  // Require the key to be preceded by ',' or '{' to avoid partial matches
  // (e.g. "r" matching inside "nr").
  static size_t findKey(const std::string& line, const std::string& key) {
    for (const char* pre : { ",\"", "{\"" }) {
      std::string pat = std::string(pre) + key + "\":";
      size_t p = line.find(pat);
      if (p != std::string::npos) return p + pat.size();
    }
    return std::string::npos;
  }

  static std::string extractStr(const std::string& line, const std::string& key) {
    size_t pos = findKey(line, key);
    if (pos == std::string::npos || pos >= line.size() || line[pos] != '"') return "";
    pos++; // skip opening quote
    size_t end = line.find('"', pos);
    if (end == std::string::npos) return "";
    return line.substr(pos, end - pos);
  }

  static std::string extractNum(const std::string& line, const std::string& key) {
    size_t pos = findKey(line, key);
    if (pos == std::string::npos) return "";
    size_t end = line.find_first_of(",}", pos);
    if (end == std::string::npos) return "";
    return line.substr(pos, end - pos);
  }

  static std::string safeNum(const std::string& s) {
    return s.empty() ? "0" : s;
  }
};
