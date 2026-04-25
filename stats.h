// Tracks per-keypress results and computes two-letter pair statistics.


#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <vector>

// One recorded keypress
struct KeyEvent {
  char expected;
  char typed;   
  float ms;    
  bool correct; 
};

// Running stats for a single two letter pair
struct BigramStats {
  int total = 0;         
  int errors = 0;        
  float total_ms = 0.0f; 

  float avgMs() const { return total > 0 ? total_ms / total : 0.0f; }

  float errorRate() const { return total > 0 ? (float)errors / total : 0.0f; }

  // Weakness score, higher is worse
  float weaknessScore() const { return avgMs() + (errorRate() * 500.0f); }
};

class StatsTracker {
public:
  // Call after every keypress 
  void record(KeyEvent event) {
    history.push_back(event);

    if (!history.empty() && history.size() >= 2) {
      std::string bigram = "";
      bigram += history[history.size() - 2].expected;
      bigram += history[history.size() - 1].expected;

      auto &s = bigrams[bigram];
      s.total++;
      s.total_ms += event.ms;
      if (!event.correct)
        s.errors++;
    }
  }
  void undoLast() {
    if (!history.empty())
      history.pop_back();
  }

  // Returns pairs sorted from weakest to strongest
  std::vector<std::pair<std::string, BigramStats>> weakestBigrams() const {
    std::vector<std::pair<std::string, BigramStats>> result(bigrams.begin(),
                                                            bigrams.end());
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
      return a.second.weaknessScore() > b.second.weaknessScore();
    });
    return result;
  }

  // Overall wpm based on total time and correctness
  float overallWPM() const {
    if (history.empty())
      return 0.0f;
    float totalMs = 0;
    int correctChars = 0;
    for (const auto &e : history) {
      totalMs += e.ms;
      if (e.correct)
        correctChars++;
    }
    float minutes = totalMs / 60000.0f;
    return minutes > 0 ? (correctChars / 5.0f) / minutes
                       : 0.0f; 
  }

  const std::map<std::string, BigramStats> &getBigrams() const {
    return bigrams;
  }

private:
  std::vector<KeyEvent> history;
  std::map<std::string, BigramStats> bigrams;
};
