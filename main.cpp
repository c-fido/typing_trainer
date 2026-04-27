// Entry point — ties together terminal, stats, SM-2, ghost racer, code mode, network multiplayer, session logger, and the training loop.

// TODO
// FIX GHOST MODE
// FIX MULTIPLAYER
// Actual TUI?
// Bruh idk
// BETTER HEATMAP!! It sucks rn

#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include "terminal.h"
#include "stats.h"
#include "text_gen.h"
#include "code_gen.h"

#include "network.h"
#include "logger.h"

// Color codes
namespace Color {
    const std::string GREEN  = "\033[32m";
    const std::string RED    = "\033[31m";
    const std::string GRAY   = "\033[90m";
    const std::string RESET  = "\033[0m";
    const std::string BOLD   = "\033[1m";
    const std::string CLEAR_LINE = "\033[2K\r";  // erase current line
}

//  Terminal helpers 

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


// Ghost cursor helpers 
// struct GhostState {
//   const GhostData* playback            = nullptr; // null = no ghost
//   GhostData*       recorder            = nullptr; // null = not recording
//   std::chrono::steady_clock::time_point sessionStart;
//   int              charsBeforeThisRound = 0;
// };

// Main training round 

// Returns chars typed in this round, or -1 if the user pressed ESC.

int runRound(StatsTracker& stats,
             std::function<std::string()> getLine,
             /*GhostState& gs*/ ...)
{
  std::string target = getLine();
  std::vector<bool> typed_correct(target.size(), false);

  cursorBlinkingUnderline();
  std::cout << "\n";
  printTargetLine(target);

  size_t pos          = 0;
  // int prevGhostCol    = -1; // 1-indexed terminal column of last drawn ghost; -1 = none
  auto lastKeyTime    = std::chrono::steady_clock::now();

  // Ghost feature disabled
  // auto updateGhost = [&](bool force = false) { ... };
  // updateGhost();

  while (pos < target.size()) {
    char c = Terminal::readKey();

    if (c == 27) {
      std::cout << "\n";
      cursorRestore();
      return -1;
    }


    if (c == 127) {
      if (pos > 0) {
        pos--;
        stats.undoLast();
        typed_correct[pos] = false;
        std::cout << "\b" << Color::GRAY << target[pos] << Color::RESET << "\b";
        std::cout.flush();
      }
      // updateGhost(true); // force redraw — player pos changed
      continue;
    }

    auto now = std::chrono::steady_clock::now();
    float ms = std::chrono::duration<float, std::milli>(now - lastKeyTime).count();
    lastKeyTime = now;

    bool correct = (c == target[pos]);
    stats.record({ target[pos], c, ms, correct });
    typed_correct[pos] = correct;


    // if (gs.recorder) {
    //   auto elapsed = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
    //       now - gs.sessionStart).count();
    //   gs.recorder->record(elapsed);
    // }

    if (correct) std::cout << Color::GREEN << c        << Color::RESET;
    else         std::cout << Color::RED   << target[pos] << Color::RESET;
    std::cout.flush();

    pos++;
    // updateGhost();
  }

  std::cout << "\n";
  cursorRestore();
  stats.markLineBoundary();
  return (int)target.size();
}

// Multiplayer race 

void runMultiplayer(bool isHost, const std::string& peerIP, BigramMemory& memory) {
  signal(SIGPIPE, SIG_IGN); // handle dropped connections without crashing

  Network net;

  Terminal::disableRawMode(); // allow clean console output during connect

  if (isHost) {
    std::cout << "Hosting on port 5000, waiting for opponent...\n" << std::flush;
    if (!net.hostAndWait()) {
      std::cerr << "Failed to bind/accept on port 5000.\n";
      Terminal::enableRawMode();
      return;
    }
    std::cout << "Opponent connected!\n" << std::flush;
  } else {
    std::cout << "Connecting to " << peerIP << ":5000...\n" << std::flush;
    if (!net.connectTo(peerIP)) {
      std::cerr << "Could not connect to " << peerIP << ".\n";
      Terminal::enableRawMode();
      return;
    }
    std::cout << "Connected!\n" << std::flush;
  }

  // Exchange the shared target line
  std::string target;
  if (isHost) {
    TextGenerator gen;
    target = gen.generateLine(memory);
    if (!net.sendLine(target)) { std::cerr << "Failed to send line.\n"; return; }
    uint32_t ack = 0;
    net.recvU32(ack); // wait for client ready
    net.sendU32(0);   // go signal
  } else {
    target = net.recvLine();
    if (target.empty()) { std::cerr << "Failed to receive line.\n"; return; }
    net.sendU32(0);   // ready
    uint32_t go = 0;
    net.recvU32(go);  // wait for go signal
  }

  // Countdown 
  for (int i = 3; i >= 1; i--) {
    std::cout << i << "...\n" << std::flush;
    sleep(1);
  }
  std::cout << Color::BOLD << "GO!" << Color::RESET << "\n\n" << std::flush;

  Terminal::enableRawMode();

  printTargetLine(target);
  cursorBlinkingUnderline();

  size_t   pos              = 0;
  int      prevOpponentCol  = -1;
  auto     lastKeyTime      = std::chrono::steady_clock::now();
  bool     opponentDone     = false;
  float    opponentWPM      = 0.0f;

  StatsTracker stats;
  std::vector<bool> typed_correct(target.size(), false);

  auto drawOpponent = [&](int col, bool erase) {
    if (col <= 0 || col > (int)target.size()) return;
    int idx = col - 1;
    if (erase) {
      std::string ch = (idx < (int)pos)
        ? (typed_correct[idx] ? Color::GREEN : Color::RED) + std::string(1, target[idx]) + Color::RESET
        : Color::GRAY + std::string(1, target[idx]) + Color::RESET;
      overlayAt(col, ch);
    } else {
      overlayAt(col, Color::YELLOW + "~" + Color::RESET);
    }
    std::cout.flush();
  };

  while (pos < target.size()) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    FD_SET(net.fd, &fds);
    int maxfd = std::max(STDIN_FILENO, net.fd) + 1;
    if (select(maxfd, &fds, nullptr, nullptr, nullptr) < 0) break;

    // Network: update opponent cursor
    if (FD_ISSET(net.fd, &fds)) {
      uint32_t v = 0;
      if (::recv(net.fd, &v, sizeof(v), 0) != sizeof(v)) break; // disconnected
      if (Network::isDone(v)) {
        opponentDone = true;
        opponentWPM  = Network::doneWPM(v);
        if (prevOpponentCol > 0) { drawOpponent(prevOpponentCol, true); prevOpponentCol = -1; }
      } else {
        int newCol = (int)v + 1; // 1-indexed
        if (newCol != prevOpponentCol) {
          if (prevOpponentCol > 0) drawOpponent(prevOpponentCol, true);
          if ((int)v >= (int)pos && (int)v < (int)target.size()) {
            drawOpponent(newCol, false);
            prevOpponentCol = newCol;
          } else {
            prevOpponentCol = -1;
          }
        }
      }
    }

    // Keyboard
    if (FD_ISSET(STDIN_FILENO, &fds)) {
      char c = Terminal::readKey();

      if (c == 27) {
        net.sendU32(Network::mkDone(0.0f));
        std::cout << "\n";
        cursorRestore();
        return;
      }

      if (c == 127) {
        if (pos > 0) {
          pos--;
          stats.undoLast();
          typed_correct[pos] = false;
          std::cout << "\b" << Color::GRAY << target[pos] << Color::RESET << "\b";
          std::cout.flush();
        }
        net.sendU32(Network::mkPos((uint32_t)pos));
        continue;
      }

      auto now = std::chrono::steady_clock::now();
      float ms = std::chrono::duration<float, std::milli>(now - lastKeyTime).count();
      lastKeyTime = now;

      bool correct = (c == target[pos]);
      stats.record({ target[pos], c, ms, correct });
      typed_correct[pos] = correct;

      if (correct) std::cout << Color::GREEN << c           << Color::RESET;
      else         std::cout << Color::RED   << target[pos] << Color::RESET;
      std::cout.flush();

      pos++;
      net.sendU32(Network::mkPos((uint32_t)pos));
    }
  }

  float myWPM = stats.overallWPM();
  net.sendU32(Network::mkDone(myWPM));
  cursorRestore();
  std::cout << "\n";

  // If opponent hasn't finished yet, wait for their done message
  if (!opponentDone) {
    std::cout << "Waiting for opponent..." << std::flush;
    uint32_t v = 0;
    while (net.recvU32(v)) {
      if (Network::isDone(v)) { opponentWPM = Network::doneWPM(v); opponentDone = true; break; }
    }
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

  // --- Startup menu ---
  while (true) {
    std::cout << Color::BOLD << "\nSelect mode:\n" << Color::RESET;
    std::cout << "  1. Host multiplayer\n";
    std::cout << "  2. Join multiplayer\n";
    std::cout << "  3. Code mode\n";
    std::cout << "  4. Normal mode\n";
    std::cout << "Enter choice [1-4]: ";
    std::cout.flush();
    std::string choice;
    std::getline(std::cin, choice);
    if (choice == "1") {
      isHost = true;
      break;
    } else if (choice == "2") {
      isJoin = true;
      std::cout << "Enter host IP: ";
      std::getline(std::cin, joinIP);
      break;
    } else if (choice == "3") {
      codeMode = true;
      break;
    } else if (choice == "4") {
      break;
    } else {
      std::cout << "Invalid choice. Please try again.\n";
    }

  Terminal::enableRawMode();
  std::atexit([]() { Terminal::disableRawMode(); });

  BigramMemory memory;
  memory.load("bigram_memory.json");

  // Multiplayer mode 
  if (isHost || isJoin) {
    runMultiplayer(isHost, joinIP, memory);
    return 0;
  }

  // Normal / code mode 
  StatsTracker stats;
  TextGenerator gen;
  CodeGenerator codeGen;

  auto sessionStart    = std::chrono::steady_clock::now();
  int  totalCharsTyped = 0;

  std::cout << Color::BOLD << "Adaptive Typing Trainer" << Color::RESET;
  if (codeMode) std::cout << "  " << Color::YELLOW << "[Code Mode]" << Color::RESET;
  std::cout << "\n";
  std::cout << "Type the highlighted text. Press ESC to quit.\n";
  std::cout << "----------------------------------------------\n";

  const int MAX_ROUNDS = 10;
  for (int round = 1; round <= MAX_ROUNDS; round++) {
    std::cout << "\nRound " << round << "/" << MAX_ROUNDS;

    auto getLine = [&]() -> std::string {
      return codeMode ? codeGen.nextLine() : gen.generateLine(memory);
    };

    int n = runRound(stats, getLine /*, gs*/);
    if (n < 0) break; // ESC
    totalCharsTyped += n;
  }

  printSummary(stats);
  printHeatmap(stats);

  // SM-2 update (skip in code mode — snippets are fixed, not adaptive)
  if (!codeMode) {
    for (const auto& [bigram, bs] : stats.getBigrams()) {
      int q = BigramMemory::qualityFromStats(bs);
      if (q >= 0) memory.update(bigram, q);
    }
    memory.save("bigram_memory.json");
  }

  SessionLogger::log("sessions.csv", stats);
  return 0;
}
