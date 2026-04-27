// Cycles through code snippets from snippets.txt for code training mode.
// Snippets are separated by lines containing only "---".

#pragma once
#include <fstream>
#include <string>
#include <vector>

class CodeGenerator {
public:
    CodeGenerator(const std::string& filename = "snippets.txt") {
        std::ifstream f(filename);
        if (!f.is_open()) {
            snippets = {
                "for (int i = 0; i < n; ++i) arr[i] = 0;",
                "if (ptr != nullptr && ptr->val != expected) return;",
                "std::vector<std::string> result = split(input, ',');",
                "while (!q.empty()) { auto x = q.front(); q.pop(); }",
            };
            return;
        }
        std::string line, current;
        while (std::getline(f, line)) {
            if (line == "---") {
                if (!current.empty()) { snippets.push_back(current); current.clear(); }
            } else {
                if (!current.empty()) current += ' ';
                current += line;
            }
        }
        if (!current.empty()) snippets.push_back(current);
    }

    std::string nextLine() {
        if (snippets.empty()) return "// no snippets loaded";
        return snippets[index++ % snippets.size()];
    }

private:
    std::vector<std::string> snippets;
    size_t index = 0;
};
