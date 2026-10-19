#ifndef KEYBOARD_H
#define KEYBOARD_H

void wait(long c);

extern char keyboard_map[128];

extern char keyboard_shift_map[128];

extern char keyboard_caps_map[128];

extern char keyboard_map_ua[128];

extern char keyboard_shift_map_ua[128];

#define KBD_LAYOUT_EN 0
#define KBD_LAYOUT_UA 1

extern int keyboard_layout;

char keyboard_translate(unsigned char sc, int shift, int caps, int force_latin);

int keyboard_layout_track(int ctrl, int alt, int shift, int other_key);

unsigned char inb(unsigned short port);

void keyboard_handler();

#endif 