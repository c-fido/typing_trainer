// Entry point — ties together terminal, stats, SM-2,
// code mode, network multiplayer, session logger, and the training loop.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <sys/select.h>
#include <unistd.h>

#include "terminal.h"
#include "stats.h"
#include "sm2.h"
#include "text_gen.h"
#include "code_gen.h"
#include "network.h"
#include "logger.h"

namespace Color {
  const std::string GREEN      = "\033[32m";
  const std::string RED        = "\033[31m";
  const std::string YELLOW     = "\033[38;2;255;200;0m";
  const std::string GRAY       = "\033[90m";
  const std::string RESET      = "\033[0m";
  const std::string BOLD       = "\033[1m";
  const std::string CLEAR_LINE = "\033[2K\r";
  const std::string CYAN       = "\033[36m";
  const std::string DIM        = "\033[2m";
}

// Terminal helpers

void printTargetLine(const std::string& line) {
  std::cout << Color::GRAY << line << Color::RESET << "\r";
  std::cout.flush();
}

static void cursorBlinkingUnderline() { std::cout << "\033[3 q"; std::cout.flush(); }
static void cursorRestore()           { std::cout << "\033[0 q"; std::cout.flush(); }

static void overlayAt(int col, const std::string& s) {
  std::cout << "\033[s"
            << "\033[" << col << "G"
            << s
            << "\033[u";
}

// Read a key with a millisecond timeout; returns -1 on timeout.
static int readKeyTimeout(int timeoutMs) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
  if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) return -1;
  char c;
  read(STDIN_FILENO, &c, 1);
  return (unsigned char)c;
}

// Heatmap

static std::string rgbFg(int r, int g, int b) {
  return "\033[38;2;" + std::to_string(r) + ";" +
                        std::to_string(g) + ";" +
                        std::to_string(b) + "m";
}

static std::string scoreColor(float t) {
  int r, g, b;
  if (t < 0.5f) {
    float u = t * 2.0f;
    r = (int)(20  + u * 200);
    g = (int)(210 - u * 30);
    b = 20;
  } else {
    float u = (t - 0.5f) * 2.0f;
    r = 220;
    g = (int)(180 - u * 160);
    b = (int)(20  + u * 10);
  }
  return rgbFg(r, g, b);
}

void printHeatmap(const StatsTracker& stats) {
  const auto& km = stats.getKeyStats();

  float minS = std::numeric_limits<float>::max(), maxS = 0.0f;
  for (const auto& [k, ks] : km) {
    if (ks.total == 0) continue;
    float s = ks.score();
    minS = std::min(minS, s);
    maxS = std::max(maxS, s);
  }
  float range = maxS - minS;

  auto keyColor = [&](char c) -> std::string {
    auto it = km.find(c);
    if (it == km.end() || it->second.total == 0)
      return "\033[38;2;70;70;70m";
    float t = (range > 0.0f) ? (it->second.score() - minS) / range : 0.0f;
    return scoreColor(t);
  };

  auto pk = [&](char c, const std::string& label = "") {
    std::string lbl = label.empty() ? std::string(1, c) : label;
    std::cout << keyColor(c) << lbl << Color::RESET << " ";
  };

  std::cout << "\n" << Color::BOLD << "Key Heatmap" << Color::RESET << "\n";
  std::cout << Color::DIM
            << "  green = fast/accurate   red = slow/errorful   dim = no data\n\n"
            << Color::RESET;

  std::cout << "  ";
  for (char c : {'`','1','2','3','4','5','6','7','8','9','0','-','='}) pk(c);
  std::cout << "\n   ";
  for (char c : {'q','w','e','r','t','y','u','i','o','p','[',']','\\'}) pk(c);
  std::cout << "\n    ";
  for (char c : {'a','s','d','f','g','h','j','k','l',';','\''}) pk(c);
  std::cout << "\n      ";
  for (char c : {'z','x','c','v','b','n','m',',','.','/'}) pk(c);
  std::cout << "\n          " << keyColor(' ') << "[    space    ]" << Color::RESET << "\n";

  // Per-key breakdown: worst keys
  std::vector<std::pair<char, float>> keyed;
  for (const auto& [k, ks] : km)
    if (ks.total >= 2) keyed.push_back({k, ks.score()});
  std::sort(keyed.begin(), keyed.end(), [](auto& a, auto& b){ return a.second > b.second; });

  if (!keyed.empty()) {
    std::cout << "\n" << Color::BOLD << "Slowest keys:" << Color::RESET << "\n";
    int n = std::min((int)keyed.size(), 8);
    for (int i = 0; i < n; i++) {
      char k = keyed[i].first;
      const auto& ks = km.at(k);
      float t = (range > 0.0f) ? (ks.score() - minS) / range : 0.0f;
      std::string kname = (k == ' ') ? "space" : std::string(1, k);
      std::cout << "  " << scoreColor(t) << std::setw(6) << kname << Color::RESET
                << "  " << std::fixed << std::setprecision(0)
                << std::setw(4) << ks.avgMs() << "ms"
                << "  err " << ks.errors << "/" << ks.total << "\n";
    }
  }
}

// Summary

void printSummary(const StatsTracker& stats) {
  float wpm   = stats.overallWPM();
  float acc   = stats.accuracy();
  int   chars = stats.totalChars();
  float secs  = stats.totalTimeSeconds();
  int   mins  = (int)(secs / 60);
  int   sec   = (int)(secs) % 60;

  std::cout << "\n\n" << Color::BOLD << "=== Session Summary ===" << Color::RESET << "\n";
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "  WPM:      " << Color::GREEN << std::setw(6) << wpm  << Color::RESET << "\n";
  std::cout << "  Accuracy: " << Color::GREEN << std::setw(5) << acc  << "%" << Color::RESET << "\n";
  std::cout << "  Chars:    " << std::setw(6) << chars << "\n";
  std::cout << "  Time:     " << std::setw(4) << mins << "m " << std::setw(2) << sec << "s\n";

  auto weak = stats.weakestBigrams();
  if (!weak.empty()) {
    std::cout << Color::BOLD << "Weakest bigrams:\n" << Color::RESET;
    int count = 0;
    for (const auto& [bigram, s] : weak) {
      if (count++ >= 5) break;
      std::cout << "  \"" << bigram << "\""
                << "  avg: " << s.avgMs() << "ms"
                << "  errors: " << s.errors << "/" << s.total << "\n";
    }
  } else {
    std::cout << "Not enough data for bigram analysis yet.\n";
  }
}

// Training round — returns chars typed, or -1 if ESC pressed.

int runRound(StatsTracker& stats, std::function<std::string()> getLine) {
  std::string target = getLine();
  std::vector<bool> typed_correct(target.size(), false);

  cursorBlinkingUnderline();
  std::cout << "\n";
  printTargetLine(target);

  size_t pos       = 0;
  auto lastKeyTime = std::chrono::steady_clock::now();

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
  }

  std::cout << "\n";
  cursorRestore();
  stats.markLineBoundary();
  return (int)target.size();
}

// Multiplayer race

void runMultiplayer(bool isHost, const std::string& peerIP, BigramMemory& memory) {
  signal(SIGPIPE, SIG_IGN);

  Network net;
  Terminal::disableRawMode();

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

  std::string target;
  if (isHost) {
    TextGenerator gen;
    target = gen.generateLine(memory);
    if (!net.sendLine(target)) { std::cerr << "Failed to send line.\n"; return; }
    uint32_t ack = 0;
    net.recvU32(ack);
    net.sendU32(0);
  } else {
    target = net.recvLine();
    if (target.empty()) { std::cerr << "Failed to receive line.\n"; return; }
    net.sendU32(0);
    uint32_t go = 0;
    net.recvU32(go);
  }

  for (int i = 3; i >= 1; i--) {
    std::cout << i << "...\n" << std::flush;
    sleep(1);
  }
  std::cout << Color::BOLD << "GO!" << Color::RESET << "\n\n" << std::flush;

  Terminal::enableRawMode();

  printTargetLine(target);
  cursorBlinkingUnderline();

  size_t pos             = 0;
  int    prevOpponentCol = -1;
  auto   lastKeyTime     = std::chrono::steady_clock::now();
  bool   opponentDone    = false;
  float  opponentWPM     = 0.0f;

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

    if (FD_ISSET(net.fd, &fds)) {
      uint32_t v = 0;
      if (::recv(net.fd, &v, sizeof(v), 0) != sizeof(v)) break;
      if (Network::isDone(v)) {
        opponentDone = true;
        opponentWPM  = Network::doneWPM(v);
        if (prevOpponentCol > 0) { drawOpponent(prevOpponentCol, true); prevOpponentCol = -1; }
      } else {
        int newCol = (int)v + 1;
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

  if (!opponentDone) {
    std::cout << "Waiting for opponent..." << std::flush;
    uint32_t v = 0;
    while (net.recvU32(v)) {
      if (Network::isDone(v)) { opponentWPM = Network::doneWPM(v); opponentDone = true; break; }
    }
    std::cout << "\n";
  }

  std::cout << "\n" << Color::BOLD << "=== Race Result ===" << Color::RESET << "\n";
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "  Your WPM:     " << Color::GREEN << myWPM << Color::RESET << "\n";
  if (opponentDone)
    std::cout << "  Opponent WPM: " << opponentWPM << "\n";

  if (!opponentDone || myWPM >= opponentWPM)
    std::cout << Color::GREEN << Color::BOLD << "  You win!\n" << Color::RESET;
  else
    std::cout << Color::RED   << Color::BOLD << "  Opponent wins.\n" << Color::RESET;

  // Update SM-2 and log session after a multiplayer race
  for (const auto& [bigram, bs] : stats.getBigrams()) {
    int q = BigramMemory::qualityFromStats(bs);
    if (q >= 0) memory.update(bigram, q);
  }
  memory.save("bigram_memory.json");
  SessionLogger::log("sessions.csv", stats);
}

// Start menu — arrow keys or 1-4 to select, Enter to confirm.
// Returns: 0=Normal, 1=Code, 2=Host, 3=Join, -1=quit

static int showMenu() {
  const std::vector<std::string> options = {
    "Normal mode",
    "Code mode",
    "Host multiplayer",
    "Join multiplayer",
  };

  std::cout << "\033[2J\033[H"; // clear screen, cursor to top-left
  std::cout << Color::BOLD << Color::CYAN
            << "  ##  ####  #  #  #  #    ###  ####  ###  #  #\n"
            << "   #  #  #  #  #  ## #    #  # #  #  # #  # # \n"
            << "   #  #  #  ####  # ##    ###  #  #  ###  ##   \n"
            << "#  #  #  #  #  #  #  #    #    #  #  # #  # #  \n"
            << " ##   ####  #  #  #  #    #    ####  #  # #  # \n"
            << Color::RESET
            << Color::BOLD << "          Adaptive Typing Trainer\n\n" << Color::RESET
            << Color::DIM  << "  Use arrow keys or 1-4 to select, Enter to confirm.\n\n"
            << Color::RESET;

  int selected = 0;

  auto draw = [&]() {
    std::cout << "\033[" << (int)options.size() << "A"; // move up N lines
    for (int i = 0; i < (int)options.size(); i++) {
      std::cout << Color::CLEAR_LINE;
      if (i == selected)
        std::cout << Color::BOLD << Color::GREEN << "  > " << (i + 1) << ". " << options[i] << Color::RESET;
      else
        std::cout << Color::DIM  << "    " << (i + 1) << ". " << options[i] << Color::RESET;
      std::cout << "\n";
    }
    std::cout.flush();
  };

  // Initial render
  for (int i = 0; i < (int)options.size(); i++) {
    if (i == selected)
      std::cout << Color::BOLD << Color::GREEN << "  > " << (i + 1) << ". " << options[i] << Color::RESET;
    else
      std::cout << Color::DIM  << "    " << (i + 1) << ". " << options[i] << Color::RESET;
    std::cout << "\n";
  }
  std::cout.flush();

  while (true) {
    char c = Terminal::readKey();

    if (c == '\r' || c == '\n') {
      return selected;
    }

    if (c == 27) {
      // Check if this is an escape sequence (arrow keys send \033[A etc.)
      int c2 = readKeyTimeout(50);
      if (c2 == '[') {
        int c3 = readKeyTimeout(50);
        if (c3 == 'A') { // up arrow
          selected = (selected - 1 + (int)options.size()) % (int)options.size();
          draw();
        } else if (c3 == 'B') { // down arrow
          selected = (selected + 1) % (int)options.size();
          draw();
        }
      } else {
        // Plain ESC
        return -1;
      }
      continue;
    }

    if (c >= '1' && c <= '4') {
      selected = c - '1';
      draw();
      return selected;
    }
  }
}

// Entry point

int main() {
  Terminal::enableRawMode();
  std::atexit([]() { Terminal::disableRawMode(); });

  int choice = showMenu();
  if (choice < 0) return 0;

  bool codeMode = (choice == 1);
  bool isHost   = (choice == 2);
  bool isJoin   = (choice == 3);
  std::string joinIP;

  if (isJoin) {
    Terminal::disableRawMode();
    std::cout << "\nEnter host IP: ";
    std::cout.flush();
    std::getline(std::cin, joinIP);
    Terminal::enableRawMode();
  }

  BigramMemory memory;
  memory.load("bigram_memory.json");

  if (isHost || isJoin) {
    runMultiplayer(isHost, joinIP, memory);
    return 0;
  }

  StatsTracker stats;
  TextGenerator gen;
  CodeGenerator codeGen;

  std::cout << "\033[2J\033[H"; // clear screen
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
    int n = runRound(stats, getLine);
    if (n < 0) break;
  }

  printSummary(stats);
  printHeatmap(stats);

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
