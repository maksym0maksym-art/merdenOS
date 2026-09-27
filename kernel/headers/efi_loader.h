#ifndef EFI_LOADER_H
#define EFI_LOADER_H

#include <efi.h>
#include <efilib.h>

EFI_STATUS InitFileSystem(
    EFI_HANDLE ImageHandle,
    EFI_FILE_PROTOCOL **Root
);

EFI_STATUS OpenKernel(
    EFI_FILE_PROTOCOL *Root,
    CHAR16 *Path,
    EFI_FILE_PROTOCOL **Kernel
);

EFI_STATUS GetFileSize(
    EFI_FILE_PROTOCOL *File,
    UINTN *Size
);

EFI_STATUS LoadKernel(
    EFI_FILE_PROTOCOL *Kernel,
    EFI_PHYSICAL_ADDRESS Address,
    UINTN Size
);

EFI_STATUS ExitBoot(
    EFI_HANDLE ImageHandle
);

#endif