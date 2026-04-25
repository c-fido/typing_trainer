// Handles switching the terminal in/out of raw mode

#pragma once
#include <termios.h>
#include <unistd.h>

class Terminal {
public:
    static void enableRawMode() {
        termios raw;
        tcgetattr(STDIN_FILENO, &raw);       
        original = raw;                       

        raw.c_lflag &= ~(ECHO | ICANON);     
        raw.c_cc[VMIN] = 1;                  
        raw.c_cc[VTIME] = 0;                 

        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }

    // Call this at the end of the program to restore normal terminal behavior
    static void disableRawMode() {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    }

    // Read a single keypress and return it
    static char readKey() {
        char c;
        read(STDIN_FILENO, &c, 1);
        return c;
    }

private:
    inline static termios original;
};
