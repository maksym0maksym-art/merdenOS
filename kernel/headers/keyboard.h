#ifndef KEYBOARD_H
#define KEYBOARD_H

void wait(long c);

extern char keyboard_map[128];

extern char keyboard_shift_map[128];

extern char keyboard_caps_map[128];

unsigned char inb(unsigned short port);

void keyboard_handler();

#endif 