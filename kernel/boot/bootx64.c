#include <efi.h>
#include <efilib.h>
#include "../headers/efi_loader.h"

EFI_STATUS EFIAPI efi_main(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable)
{
    InitializeLib(ImageHandle, SystemTable);

    EFI_FILE_PROTOCOL *Root;
    EFI_FILE_PROTOCOL *Kernel;
    EFI_STATUS Status;
    UINTN Size;

    Status = InitFileSystem(ImageHandle, &Root);
    if (EFI_ERROR(Status))
        return Status;

    Status = OpenKernel(Root, L"kernel.bin", &Kernel);
    if (EFI_ERROR(Status))
        return Status;

    Status = GetFileSize(Kernel, &Size);
    if (EFI_ERROR(Status))
        return Status;

    Status = LoadKernel(Kernel, 0x100000, Size);
    if (EFI_ERROR(Status))
        return Status;

    Status = ExitBoot(ImageHandle);
    if (EFI_ERROR(Status))
        return Status;

    ((void (*)(void))0x100000)();

    while (1);

    return EFI_SUCCESS;
}