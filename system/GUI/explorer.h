#ifndef EXPLORER_H
#define EXPLORER_H

#include "gui.h"

/* Провідник: диски (RAM-диск і USB-флешки), папки, відкриття файлів у редакторі,
 * створення / перейменування / видалення. Один екземпляр. */
gui_window_t *gui_explorer_open(void);

#endif
