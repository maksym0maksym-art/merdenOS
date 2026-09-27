#include "../headers/efi_loader.h"

EFI_STATUS InitFileSystem(
    EFI_HANDLE ImageHandle,
    EFI_FILE_PROTOCOL **Root)
{
    EFI_STATUS Status;

    EFI_LOADED_IMAGE *LoadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;

    Status = BS->HandleProtocol(
        ImageHandle,
        &gEfiLoadedImageProtocolGuid,
        (void**)&LoadedImage);

    if (EFI_ERROR(Status))
        return Status;

    Status = BS->HandleProtocol(
        LoadedImage->DeviceHandle,
        &gEfiSimpleFileSystemProtocolGuid,
        (void**)&Fs);

    if (EFI_ERROR(Status))
        return Status;

    return Fs->OpenVolume(Fs, Root);
}

EFI_STATUS OpenKernel(
    EFI_FILE_PROTOCOL *Root,
    CHAR16 *Path,
    EFI_FILE_PROTOCOL **Kernel)
{
    return Root->Open(
        Root,
        Kernel,
        Path,
        EFI_FILE_MODE_READ,
        0
    );
}

EFI_STATUS GetFileSize(
    EFI_FILE_PROTOCOL *File,
    UINTN *Size)
{
    EFI_FILE_INFO *Info;
    UINTN BufferSize = SIZE_OF_EFI_FILE_INFO + 256;

    BS->AllocatePool(
        EfiLoaderData,
        BufferSize,
        (void**)&Info);

    EFI_STATUS Status =
        File->GetInfo(
            File,
            &gEfiFileInfoGuid,
            &BufferSize,
            Info);

    if (EFI_ERROR(Status))
        return Status;

    *Size = Info->FileSize;

    BS->FreePool(Info);

    return EFI_SUCCESS;
}

EFI_STATUS LoadKernel(
    EFI_FILE_PROTOCOL *Kernel,
    EFI_PHYSICAL_ADDRESS Address,
    UINTN Size)
{
    UINTN Pages = EFI_SIZE_TO_PAGES(Size);

    EFI_STATUS Status =
        BS->AllocatePages(
            AllocateAddress,
            EfiLoaderData,
            Pages,
            &Address);

    if (EFI_ERROR(Status))
        return Status;

    return Kernel->Read(
        Kernel,
        &Size,
        (void*)Address);
}

EFI_STATUS ExitBoot(
    EFI_HANDLE ImageHandle)
{
    UINTN MemoryMapSize = 0;
    EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
    UINTN MapKey;
    UINTN DescriptorSize;
    UINT32 DescriptorVersion;

    BS->GetMemoryMap(
        &MemoryMapSize,
        MemoryMap,
        &MapKey,
        &DescriptorSize,
        &DescriptorVersion);

    MemoryMapSize += DescriptorSize * 8;

    BS->AllocatePool(
        EfiLoaderData,
        MemoryMapSize,
        (void**)&MemoryMap);

    BS->GetMemoryMap(
        &MemoryMapSize,
        MemoryMap,
        &MapKey,
        &DescriptorSize,
        &DescriptorVersion);

    return BS->ExitBootServices(
        ImageHandle,
        MapKey);
}