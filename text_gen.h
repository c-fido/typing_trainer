// text_gen.h
// Generates practice lines from a word list.
// Once you have bigram stats, words containing your weakest bigrams
// get picked more often — that's the "adaptive" part.

#pragma once
#include <string>
#include <vector>
#include <random>
#include <fstream>
#include "stats.h"

class TextGenerator {
public:
    TextGenerator(const std::string& filename = "words.txt") {
    std::ifstream file(filename);

    if (!file.is_open()) {
        // Fallback to a small default list if the file isn't found
        wordList = { "the", "and", "for", "are", "but", "not", "you", "all" };
        return;
    }

    std::string word;
    while (std::getline(file, word)) {
        if (!word.empty()) {
            wordList.push_back(word);
        }
    }
}
    // Generate a line of ~60 characters for the user to type.
    // If we have bigram stats, bias toward words that contain weak bigrams.
    std::string generateLine(const StatsTracker& stats) {
        auto weak = stats.weakestBigrams();
        std::string line;

        while (line.size() < 60) {
            std::string word = pickWord(weak);
            if (!line.empty()) line += ' ';
            line += word;
        }

        return line;
    }

private:
    std::vector<std::string> wordList;
    std::mt19937 rng{ std::random_device{}() };

    // Score a word by how much it contains weak bigrams.
    // Returns 0 if no bigram data yet (falls back to random).
    float scoreWord(const std::string& word,
                    const std::vector<std::pair<std::string, BigramStats>>& weak) {
        if (weak.empty()) return 1.0f;

        float score = 1.0f;
        for (size_t i = 0; i + 1 < word.size(); i++) {
            std::string bg = word.substr(i, 2);
            // Check if this bigram is in our weak list
            for (size_t r = 0; r < std::min(weak.size(), (size_t)5); r++) {
                if (weak[r].first == bg) {
                    score += (5 - r) * 2.0f;  // top weak bigrams boost score most
                }
            }
        }
        return score;
    }

    // Weighted random pick — words with higher scores are chosen more often
    std::string pickWord(const std::vector<std::pair<std::string, BigramStats>>& weak) {
        std::vector<float> weights;
        for (const auto& w : wordList) {
            weights.push_back(scoreWord(w, weak));
        }

        std::discrete_distribution<int> dist(weights.begin(), weights.end());
        return wordList[dist(rng)];
    }
};
