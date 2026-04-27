// Tracks per-keypress results, per-key stats, and bigram statistics.

#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <vector>

struct KeyEvent {
  char expected;
  char typed;
  float ms;
  bool correct;
};

struct BigramStats {
  int total = 0;
  int errors = 0;
  float total_ms = 0.0f;

  float avgMs() const { return total > 0 ? total_ms / total : 0.0f; }
  float errorRate() const { return total > 0 ? (float)errors / total : 0.0f; }
  float weaknessScore() const { return avgMs() + (errorRate() * 500.0f); }
};

struct KeyStats {
  int total = 0;
  int errors = 0;
  float total_ms = 0.0f;

  float avgMs() const { return total > 0 ? total_ms / total : 0.0f; }
  float errorRate() const { return total > 0 ? (float)errors / total : 0.0f; }
  float score() const { return avgMs() + errorRate() * 500.0f; }
};

class StatsTracker {
public:
  void record(KeyEvent event) {
    if (!history.empty() && history.back().expected != '\0' && event.expected != '\0') {
      std::string bigram;
      bigram += history.back().expected;
      bigram += event.expected;

      auto &s = bigrams[bigram];
      s.total++;
      s.total_ms += event.ms;
      if (!event.correct)
        s.errors++;
    }

    if (event.expected != '\0') {
      auto &ks = keyStats[event.expected];
      ks.total++;
      ks.total_ms += event.ms;
      if (!event.correct)
        ks.errors++;
    }

    history.push_back(event);
  }

  void markLineBoundary() {
    history.push_back({ '\0', '\0', 0.0f, true });
  }

  void undoLast() {
    if (!history.empty()) {
      const auto& last = history.back();
      if (last.expected != '\0') {
        auto it = keyStats.find(last.expected);
        if (it != keyStats.end()) {
          it->second.total--;
          it->second.total_ms -= last.ms;
          if (!last.correct) it->second.errors--;
        }
      }
      history.pop_back();
    }
  }

  std::vector<std::pair<std::string, BigramStats>> weakestBigrams() const {
    std::vector<std::pair<std::string, BigramStats>> result(bigrams.begin(), bigrams.end());
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
      return a.second.weaknessScore() > b.second.weaknessScore();
    });
    return result;
  }

  float overallWPM() const {
    float totalMs = 0;
    int correctChars = 0;
    for (const auto &e : history) {
      totalMs += e.ms;
      if (e.correct && e.expected != '\0')
        correctChars++;
    }
    float minutes = totalMs / 60000.0f;
    return minutes > 0 ? (correctChars / 5.0f) / minutes : 0.0f;
  }

  float accuracy() const {
    int total = 0, correct = 0;
    for (const auto &e : history) {
      if (e.expected == '\0') continue;
      total++;
      if (e.correct) correct++;
    }
    return total > 0 ? 100.0f * correct / total : 0.0f;
  }

  int totalChars() const {
    int n = 0;
    for (const auto &e : history)
      if (e.expected != '\0') n++;
    return n;
  }

  int totalErrors() const {
    int n = 0;
    for (const auto &e : history)
      if (!e.correct && e.expected != '\0') n++;
    return n;
  }

  float totalTimeSeconds() const {
    float ms = 0;
    for (const auto &e : history) ms += e.ms;
    return ms / 1000.0f;
  }

  const std::map<std::string, BigramStats> &getBigrams() const { return bigrams; }
  const std::map<char, KeyStats> &getKeyStats() const { return keyStats; }

private:
  std::vector<KeyEvent> history;
  std::map<std::string, BigramStats> bigrams;
  std::map<char, KeyStats> keyStats;
};
