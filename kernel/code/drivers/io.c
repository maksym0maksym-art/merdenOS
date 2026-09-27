#include "io.h"

void ide_wait_ready()
{
	while(inb(0x1F7) & 0x80);
	while(!(inb(0x1F7) & 0x08));
}
