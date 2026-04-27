// Entry point — ties together terminal, stats, SM-2, ghost racer,
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
#include <unistd.h>

#include "terminal.h"
#include "stats.h"
#include "sm2.h"
#include "text_gen.h"
#include "code_gen.h"
#include "ghost.h"
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
}

// ── Terminal helpers ──────────────────────────────────────────────────────────

void printTargetLine(const std::string& line) {
  std::cout << Color::GRAY << line << Color::RESET << "\r";
  std::cout.flush();
}

static void cursorBlinkingUnderline() { std::cout << "\033[3 q"; std::cout.flush(); }
static void cursorRestore()           { std::cout << "\033[0 q"; std::cout.flush(); }

// Save cursor, move to 1-indexed column col, print s, restore cursor.
static void overlayAt(int col, const std::string& s) {
  std::cout << "\033[s"
            << "\033[" << col << "G"
            << s
            << "\033[u";
}

// Heatmap

static std::string rgbFg(int r, int g, int b) {
  return "\033[38;2;" + std::to_string(r) + ";" +
                        std::to_string(g) + ";" +
                        std::to_string(b) + "m";
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
      return "\033[38;2;110;110;110m";
    float t = (range > 0.0f) ? (it->second.score() - minS) / range : 0.0f;
    int r, g, b;
    if (t < 0.5f) {
      float u = t * 2.0f;
      r = (int)(u * 220); g = (int)(210 - u * 10); b = (int)(60 - u * 60);
    } else {
      float u = (t - 0.5f) * 2.0f;
      r = 220; g = (int)(200 - u * 170); b = (int)(u * 30);
    }
    return rgbFg(r, g, b);
  };

  auto pk = [&](char c) {
    std::cout << keyColor(c) << c << Color::RESET << " ";
  };

  std::cout << "\n" << Color::BOLD << "Key Heatmap" << Color::RESET
            << "  (green = fast/accurate  →  red = slow/errorful  gray = no data)\n\n";
  std::cout << "  "; for (char c : {'`','1','2','3','4','5','6','7','8','9','0','-','='}) pk(c);
  std::cout << "\n    "; for (char c : {'q','w','e','r','t','y','u','i','o','p','[',']','\\'}) pk(c);
  std::cout << "\n     "; for (char c : {'a','s','d','f','g','h','j','k','l',';','\''}) pk(c);
  std::cout << "\n       "; for (char c : {'z','x','c','v','b','n','m',',','.','/'}) pk(c);
  std::cout << "\n         "; pk(' '); std::cout << " (space)\n";
}

// Summary 

void printSummary(const StatsTracker& stats) {
  float wpm    = stats.overallWPM();
  float acc    = stats.accuracy();
  int   chars  = stats.totalChars();
  float secs   = stats.totalTimeSeconds();
  int   mins   = (int)(secs / 60);
  int   sec    = (int)(secs) % 60;

  std::cout << "\n\n" << Color::BOLD << "=== Session Summary ===" << Color::RESET << "\n";
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "  WPM:      " << Color::GREEN << std::setw(6) << wpm  << Color::RESET << "\n";
  std::cout << "  Accuracy: " << Color::GREEN << std::setw(5) << acc  << "%" << Color::RESET << "\n";
  std::cout << "  Chars:    " << std::setw(6) << chars << "\n";
  std::cout << "  Time:     " << std::setw(4) << mins << "m " << std::setw(2) << sec << "s\n";

  auto weak = stats.weakestBigrams();
  std::cout << "\n" << Color::BOLD << "Weakest bigrams:" << Color::RESET << "\n";
  if (weak.empty()) { std::cout << "  Not enough data yet.\n"; return; }

  std::cout << "  " << Color::BOLD
            << std::left  << std::setw(8)  << "Pair"
            << std::right << std::setw(10) << "Avg ms"
            << std::setw(10) << "Errors"
            << std::setw(8)  << "Total"
            << Color::RESET << "\n";

  int count = 0;
  for (const auto& [bigram, s] : weak) {
    if (count++ >= 5) break;
    std::cout << "  \"" << bigram << "\"   "
              << std::right << std::setw(8) << std::setprecision(0) << s.avgMs() << "ms"
              << std::setw(8) << s.errors << "   /" << s.total << "\n";
  }
}

// ── Ghost cursor helpers ───────────────────────────────────────────────────────

struct GhostState {
  const GhostData* playback            = nullptr; // null = no ghost
  GhostData*       recorder            = nullptr; // null = not recording
  std::chrono::steady_clock::time_point sessionStart;
  int              charsBeforeThisRound = 0;
};

// Main training round 

// Returns chars typed in this round, or -1 if the user pressed ESC.
int runRound(StatsTracker& stats,
             std::function<std::string()> getLine,
             GhostState& gs)
{
  std::string target = getLine();
  std::vector<bool> typed_correct(target.size(), false);

  cursorBlinkingUnderline();
  std::cout << "\n";
  printTargetLine(target);

  size_t pos          = 0;
  int prevGhostCol    = -1; // 1-indexed terminal column of last drawn ghost; -1 = none
  auto lastKeyTime    = std::chrono::steady_clock::now();

  // Redraw the ghost cursor based on current elapsed time.
  // Only renders in the untyped zone (ghostLinePos >= pos) to avoid fighting typed chars.
  auto updateGhost = [&](bool force = false) {
    if (!gs.playback) return;

    auto elapsed = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - gs.sessionStart).count();
    int ghostSessionPos = gs.playback->posAtMs(elapsed);
    int ghostLinePos    = ghostSessionPos - gs.charsBeforeThisRound;
    int ghostCol        = (ghostLinePos >= 0 && ghostLinePos < (int)target.size())
                            ? ghostLinePos + 1  // 1-indexed
                            : -1;

    if (!force && ghostCol == prevGhostCol) return;

    // Erase previous ghost if it is still in the untyped zone
    if (prevGhostCol > 0) {
      int prevIdx = prevGhostCol - 1;
      if (prevIdx >= (int)pos && prevIdx < (int)target.size())
        overlayAt(prevGhostCol, Color::GRAY + target[prevIdx] + Color::RESET);
    }

    // Draw new ghost only when it is ahead of the player (untyped zone)
    if (ghostCol > 0 && ghostLinePos >= (int)pos) {
      bool ghostAhead  = ghostLinePos > (int)pos;
      std::string col  = ghostAhead ? Color::RED : Color::YELLOW;
      overlayAt(ghostCol, col + "|" + Color::RESET);
      prevGhostCol = ghostCol;
    } else {
      prevGhostCol = -1;
    }

    std::cout.flush();
  };

  updateGhost();

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
      updateGhost(true); // force redraw — player pos changed
      continue;
    }

    auto now = std::chrono::steady_clock::now();
    float ms = std::chrono::duration<float, std::milli>(now - lastKeyTime).count();
    lastKeyTime = now;

    bool correct = (c == target[pos]);
    stats.record({ target[pos], c, ms, correct });
    typed_correct[pos] = correct;

    if (gs.recorder) {
      auto elapsed = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
          now - gs.sessionStart).count();
      gs.recorder->record(elapsed);
    }

    if (correct) std::cout << Color::GREEN << c        << Color::RESET;
    else         std::cout << Color::RED   << target[pos] << Color::RESET;
    std::cout.flush();

    pos++;
    updateGhost();
  }

  std::cout << "\n";
  cursorRestore();
  stats.markLineBoundary();
  return (int)target.size();
}

// ── Multiplayer race ──────────────────────────────────────────────────────────

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

  // Countdown (both sides run it simultaneously)
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
  }

  // Results
  std::cout << "\n" << Color::BOLD << "=== Race Result ===" << Color::RESET << "\n";
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "  Your WPM:     " << Color::GREEN << myWPM << Color::RESET << "\n";
  if (opponentDone)
    std::cout << "  Opponent WPM: " << opponentWPM << "\n";

  if (!opponentDone || myWPM >= opponentWPM)
    std::cout << Color::GREEN << Color::BOLD << "  You win!" << Color::RESET << "\n";
  else
    std::cout << Color::RED << Color::BOLD << "  Opponent wins." << Color::RESET << "\n";
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  bool codeMode = false;
  bool isHost   = false;
  bool isJoin   = false;
  std::string joinIP;

  for (int i = 1; i < argc; i++) {
    if      (std::strcmp(argv[i], "--code") == 0) codeMode = true;
    else if (std::strcmp(argv[i], "--host") == 0) isHost   = true;
    else if (std::strcmp(argv[i], "--join") == 0 && i + 1 < argc) {
      isJoin = true;
      joinIP = argv[++i];
    }
  }

  Terminal::enableRawMode();
  std::atexit([]() { Terminal::disableRawMode(); });

  BigramMemory memory;
  memory.load("bigram_memory.json");

  // ── Multiplayer mode ────────────────────────────────────────────────────────
  if (isHost || isJoin) {
    runMultiplayer(isHost, joinIP, memory);
    return 0;
  }

  // Normal / code mode 
  StatsTracker stats;
  TextGenerator gen;
  CodeGenerator codeGen;

  GhostData ghost, newGhost;
  ghost.load("ghost.dat");

  auto sessionStart    = std::chrono::steady_clock::now();
  int  totalCharsTyped = 0;

  std::cout << Color::BOLD << "Adaptive Typing Trainer" << Color::RESET;
  if (codeMode) std::cout << "  " << Color::YELLOW << "[Code Mode]" << Color::RESET;
  std::cout << "\n";
  if (!ghost.empty())
    std::cout << "Ghost loaded — best: " << ghost.timestamps.size() << " chars in "
              << ghost.totalMs() / 1000 << "s\n";
  std::cout << "Type the highlighted text. Press ESC to quit.\n";
  std::cout << "----------------------------------------------\n";

  const int MAX_ROUNDS = 10;
  for (int round = 1; round <= MAX_ROUNDS; round++) {
    std::cout << "\nRound " << round << "/" << MAX_ROUNDS;

    GhostState gs;
    gs.playback            = ghost.empty() ? nullptr : &ghost;
    gs.recorder            = &newGhost;
    gs.sessionStart        = sessionStart;
    gs.charsBeforeThisRound = totalCharsTyped;

    auto getLine = [&]() -> std::string {
      return codeMode ? codeGen.nextLine() : gen.generateLine(memory);
    };

    int n = runRound(stats, getLine, gs);
    if (n < 0) break; // ESC
    totalCharsTyped += n;
  }

  printSummary(stats);
  printHeatmap(stats);

  // Ghost: save if this session is the new best
  if (!newGhost.empty()) {
    if (ghost.empty() || newGhost.totalMs() < ghost.totalMs()) {
      newGhost.save("ghost.dat");
      std::cout << "\n" << Color::GREEN << Color::BOLD
                << "New best! Ghost updated." << Color::RESET << "\n";
    }
  }

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
