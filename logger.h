// Appends one row per session to sessions.csv.

#pragma once
#include <ctime>
#include <fstream>
#include <iomanip>
#include <string>
#include "stats.h"

class SessionLogger {
public:
  static void log(const std::string& filename, const StatsTracker& stats) {
    bool isNew = !fileExists(filename);
    std::ofstream f(filename, std::ios::app);
    if (!f.is_open()) return;

    if (isNew) {
      f << "date,wpm,keypresses,errors,"
        << "bigram1,score1,bigram2,score2,bigram3,score3,"
        << "bigram4,score4,bigram5,score5\n";
    }

    std::time_t t = std::time(nullptr);
    char dateBuf[11];
    std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", std::localtime(&t));

    f << std::fixed << std::setprecision(1);
    f << dateBuf << ","
      << stats.overallWPM() << ","
      << stats.totalChars() << ","
      << stats.totalErrors();

    auto weak = stats.weakestBigrams();
    int count = 0;
    for (const auto& [bigram, s] : weak) {
      if (count >= 5) break;
      f << "," << bigram << "," << std::setprecision(1) << s.weaknessScore();
      count++;
    }
    while (count++ < 5) f << ",,";

    f << "\n";
  }

private:
  static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
  }
};
