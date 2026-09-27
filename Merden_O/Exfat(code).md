[[Exfat(code)]]

```c
UInt32 BootChecksum (UCHAR * Sectors, USHORT BytesPerSector)
{
  /*
    UCHAR * Sectors, above, points to an in-memory copy of the 11 sectors
    That is, 0 - 10 for the Main Boot Region, or 12 - 22 for the Backup Boot Region
  */

  UInt32 NumberOfBytes = (UInt32)BytesPerSector * 11;
  UInt32 Checksum = 0;
  UInt32 Index;

  for (Index = 0; Index < NumberOfBytes; Index++)
  {
    if ((Index == 106) || (Index == 107) || (Index == 112))
    {
      continue;
    }
    Checksum = ((Checksum&1) ? 0x80000000 : 0) + (Checksum>>1) + (UInt32)Sectors[Index];
  }
  return Checksum;
}

struct {
        uint8_t entrytype = 0x85;
        uint8_t entrycount; // This is the number of subsequent entries that also belong to this "file" entry. Should be at least 2, one 0xC0 and one 0xC1 for info and filename.
        uint16_t checksum;
        uint16_t flags; // 0x10 == directory
        uint16_t rsvd; // Reserved
        uint32_t creation, modification, access;
        char pad[12];
} fileEntry;

struct {
        uint8_t entryType = 0xC0;
        uint8_t flags;
        uint8_t reserved;
        uint8_t filenameLengthInBytes;
        uint16_t filenameHash;
        uint16_t reserved_2;
        uint64_t valid_filesize;
        uint32_t reserved3;
        uint32_t startCluster; // minus 2! Same as fat12/16/32.
        uint64_t filesize;
} fileInfoEntry;

struct {
        uint8_t entrytype = 0xC1, entrycount = 0x00;
        uint16_t name[15];
} filenameEntry;


```