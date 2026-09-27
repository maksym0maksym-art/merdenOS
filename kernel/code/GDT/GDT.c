#include "../../../standarts_lib/stdio.h"
#include "../../headers/GDT.h"

//*створення точної структури
struct GDT kernel_code = {
    .base = 0,
    .limit = 0xFFFFF,
    .access_byte = GDT_CODE_ACCESS_PL0,
    .flags = GDT_FLAGS
};


//* енкод  
void encode_Gdt_entry(uint8_t *target, struct GDT source)
{
    // limit
    target[0] = source.limit & 0xFF;
    target[1] = (source.limit >> 8) & 0xFF;
    target[6] = (source.limit >> 16) & 0x0F;

    // base
    target[2] = source.base & 0xFF;
    target[3] = (source.base >> 8) & 0xFF;
    target[4] = (source.base >> 16) & 0xFF;
    target[7] = (source.base >> 24) & 0xFF;

    // access byte
    target[5] = source.access_byte;

    // flags (upper nibble of byte 6)
    target[6] |= (source.flags & 0x0F) << 4;
};

uint64_t
create_descriptor(uint32_t base, uint32_t limit, uint16_t flag)
{
	uint64_t descriptor = 0;

	descriptor = limit & 0x000F0000;
	descriptor |= (flag << 8)  & 0x00F0FF00;
	descriptor |= (base << 16) & 0xFF;
	descriptor |= base   	   & 0xFF000000;

	descriptor <<= 32;

	descriptor |= base << 16;
	descriptor |= base & 0x0000FFFF; 


    t_printf("0x%.16llX\n", descriptor);

	return descriptor;
}

int main_g(void) 
{
	create_descriptor(0, 0, 0);
	create_descriptor(0, 0x000FFFFF, (GDT_CODE_ACCESS_PL0));
	create_descriptor(0, 0x000FFFFF, (GDT_DATA_ACCESS_PL0));
	create_descriptor(0, 0x000FFFFF, (GDT_CODE_ACCESS_PL3));
	create_descriptor(0, 0x000FFFFF, (GDT_DATA_ACCESS_PL3));

	return 0;
}
