// Entry point. Ties together the terminal, stats tracker, text generator,
// and runs the main training loop.

//TO DO!!!!
// Add a round limit or time limit in future?
// Make summary screen better

#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include "terminal.h"
#include "stats.h"
#include "text_gen.h"

// Color codes
namespace Color {
    const std::string GREEN  = "\033[32m";
    const std::string RED    = "\033[31m";
    const std::string GRAY   = "\033[90m";
    const std::string RESET  = "\033[0m";
    const std::string BOLD   = "\033[1m";
    const std::string CLEAR_LINE = "\033[2K\r";  // erase current line
}

// Prints the line in gray, then moves the cursor back to the start so the user's typed characters can overlay it in green/red.
void printTargetLine(const std::string& line) {
    std::cout << Color::GRAY << line << Color::RESET;
    // Move cursor back to start of the line
    std::cout << "\r";
    std::cout.flush();
}

// Prints a summary
void printSummary(const StatsTracker& stats) {
    std::cout << "\n\n" << Color::BOLD << "=== Session Summary ===" << Color::RESET << "\n";
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "WPM: " << Color::GREEN << stats.overallWPM() << Color::RESET << "\n\n";

    auto weak = stats.weakestBigrams();
    if (!weak.empty()) {
        std::cout << Color::BOLD << "Your weakest bigrams:" << Color::RESET << "\n";
        int count = 0;
        for (const auto& [bigram, s] : weak) {
            if (count++ >= 5) break;
            std::cout << "  \"" << bigram << "\""
                      << "  avg: " << s.avgMs() << "ms"
                      << "  errors: " << s.errors << "/" << s.total
                      << "\n";
        }
    } else {
        std::cout << "Not enough data for bigram analysis yet.\n";
    }
}

// Runs one round
void runRound(StatsTracker& stats, TextGenerator& gen) {
    std::string target = gen.generateLine(stats);

    std::cout << "\n";
    printTargetLine(target);

    size_t pos = 0;
    auto lineStart = std::chrono::steady_clock::now();
    auto lastKeyTime = lineStart;

    while (pos < target.size()) {
        char c = Terminal::readKey();

        // Let ESC quit early
        if (c == 27) {
            std::cout << "\n";
            return;
        }
        if (c == 127 && pos > 0) {
            pos--;
            stats.undoLast();
            std::cout << "\b" << Color::GRAY << target[pos] << Color::RESET << "\b";
            std::cout.flush();
            continue;
        }
        else if (c == 127 && pos > 0){
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        float ms = std::chrono::duration<float, std::milli>(now - lastKeyTime).count();
        lastKeyTime = now;

        bool correct = (c == target[pos]);

        // Record keypress
        stats.record({ target[pos], c, ms, correct });

        // Print the character in green or red 
        if (correct) {
            std::cout << Color::GREEN << c << Color::RESET;
        } else {
            std::cout << Color::RED << target[pos] << Color::RESET;
        }
        std::cout.flush();

        pos++; 
    }

    std::cout << "\n";
}

int main() {
    Terminal::enableRawMode();

    std::atexit([]() { Terminal::disableRawMode(); });

    StatsTracker stats;
    TextGenerator gen;

    std::cout << Color::BOLD << "Adaptive Typing Trainer" << Color::RESET << "\n";
    std::cout << "Type the text shown. Press ESC to quit.\n";
    std::cout << "--------------------------------------\n";


    int rounds = 0;
    const int MAX_ROUNDS = 10;

    while (rounds < MAX_ROUNDS) {
        rounds++;
        std::cout << "\nRound " << rounds << "/" << MAX_ROUNDS;
        runRound(stats, gen);
    }

    printSummary(stats);
    return 0;
}
