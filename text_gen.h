// Generates practice lines from a word list.
// Words containing bigrams that are due for SM-2 review are weighted higher.

#pragma once
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include "sm2.h"

class TextGenerator {
public:
  TextGenerator(const std::string& filename = "words.txt") {
    std::ifstream file(filename);
    if (!file.is_open()) {
      wordList = { "the", "and", "for", "are", "but", "not", "you", "all" };
      return;
    }
    std::string word;
    while (std::getline(file, word))
      if (!word.empty()) wordList.push_back(word);
  }

  // Generate a line of ~60 characters, biasing toward words that contain
  // bigrams due (or overdue) for SM-2 review.
  std::string generateLine(const BigramMemory& memory) {
    std::string line;
    while (line.size() < 60) {
      if (!line.empty()) line += ' ';
      line += pickWord(memory);
    }
    return line;
  }

private:
  std::vector<std::string> wordList;
  std::mt19937 rng{ std::random_device{}() };

  float scoreWord(const std::string& word, const BigramMemory& memory) {
    float score = 1.0f;
    for (size_t i = 0; i + 1 < word.size(); i++)
      score += memory.dueWeight(word.substr(i, 2));
    return score;
  }

  std::string pickWord(const BigramMemory& memory) {
    std::vector<float> weights;
    weights.reserve(wordList.size());
    for (const auto& w : wordList)
      weights.push_back(scoreWord(w, memory));
    std::discrete_distribution<int> dist(weights.begin(), weights.end());
    return wordList[dist(rng)];
  }
};
