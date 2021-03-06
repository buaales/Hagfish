/*
 * Copyright (c) 2015, ETH Zuerich.
 * Copyright (c) 2016, Hewlett Packard Enterprise Development LP.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* EDK Headers */
#include <Uefi.h>

#include <Guid/Acpi.h>
#include <Guid/SmBios.h>

#include <IndustryStandard/Acpi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DebugPrintErrorLevelLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/ShellLib.h>

#include <Protocol/LoadedImage.h>
#include <Protocol/LoadFile.h>
#include <Protocol/LoadFile2.h>
#include <Protocol/PxeBaseCode.h>
#include <Protocol/SimpleFileSystem.h>

/* Package headers */
#include <libelf.h>
#include <multiboot2.h>
#include <vm.h>

/* Application headers */
#include <Allocation.h>
#include <Config.h>
#include <Hardware.h>
#include <Memory.h>
#include <Util.h>
#include <Loader.h>
#include <Acpi.h>

#define roundpage(x) COVER((x), PAGE_4k)

typedef void (*cpu_driver_entry)(uint32_t multiboot_magic,
                                 void *multiboot_info,
                                 void *stack);

/* Copy a base+length string into a null-terminated string.  Destination
 * buffer must be large enough to hold the terminator i.e. n+1 characters. */
static inline void
ntstring(char *dest, const char *src, size_t len) {
    memcpy(dest, src, len);
    dest[len]= '\0';
}

/* Load a component (kernel or module) over TFTP, and fill in the relevant
 * fields in the configuration structure. */
int
load_component(struct hagfish_loader *loader, struct component_config *cmp,
        const char *buf) {
    EFI_STATUS status;

    ASSERT(cmp);

    /* Allocate a null-terminated string. */
    char *path= malloc((cmp->path_len + 1) * sizeof(char));
    if(!path) {
        DebugPrint(DEBUG_ERROR, "malloc: %a\n", strerror(errno));
        return 0;
    }
    ntstring(path, buf + cmp->path_start, cmp->path_len);

    DebugPrint(DEBUG_INFO, "%a ", path);

    /* Get the file size. */
    status = loader->size_fn(loader, path, (UINTN *) &cmp->image_size);
    if(status != EFI_SUCCESS) {
        DebugPrint(DEBUG_ERROR, "\nfile size: %r\n", status);
        return EFI_SUCCESS;
    }

    /* Allocate a page-aligned buffer. */
    size_t npages= roundpage(cmp->image_size);
    cmp->image_address= allocate_pages(npages, EfiBarrelfishELFData);
    if(!cmp->image_address) {
        DebugPrint(DEBUG_ERROR,
                   "\nFailed to allocate %d pages\n", npages);
        return 0;
    }

    /* Load the image. */
    status = loader->read_fn(loader, path, (UINTN *) &cmp->image_size, cmp->image_address);
    if(status != EFI_SUCCESS) {
        DebugPrint(DEBUG_ERROR, "\nread file: %r\n", status);
        return EFI_SUCCESS;
    }

    free(path);

    DebugPrint(DEBUG_LOADFILE,
               " done (%p, %dB)\n", cmp->image_address, cmp->image_size);
    return 1;
}


#define ROUND_UP(x, y) (((x) + ((y) - 1)) & ~((y) - 1))

#define ALIGN(x) ROUND_UP((x), sizeof(uintptr_t))

/* Allocate and fill the Multiboot information structure.  The memory map is
 * preallocated, but left empty until all allocations are finished. */
void *
create_multiboot_info(struct hagfish_config *cfg,
                      struct hagfish_loader *loader) {
    UINTN size, npages;
    struct component_config *cmp;
    void *cursor;

    /* Calculate the boot information size. */
    /* Fixed header */
    size= ALIGN(sizeof(struct multiboot_header));

    /* CPU Driver entry point*/
    size+= ALIGN(sizeof(struct multiboot_tag_efi64));

    /* cpu driver command line */
    size+= ALIGN(sizeof(struct multiboot_tag_string)
         + cfg->cpu_driver->args_len+1);
    /* DCHP ack packet */
    size+= ALIGN(sizeof(struct multiboot_tag_network)
         + sizeof(EFI_PXE_BASE_CODE_PACKET));
    /* ACPI 1.0 header */
    if(cfg->acpi1_header) {
        size+= ALIGN(sizeof(struct multiboot_tag_old_acpi)
             + sizeof(EFI_ACPI_1_0_ROOT_SYSTEM_DESCRIPTION_POINTER));
    }
    /* ACPI 2.0+ header */
    if(cfg->acpi2_header) {
        size+= ALIGN(sizeof(struct multiboot_tag_new_acpi)
             + sizeof(EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER));
    }
    /* Boot driver module tag, including command line and ELF image */
    size+= ALIGN(sizeof(struct multiboot_tag_module_64)
         + cfg->boot_driver->args_len+1 + cfg->boot_driver->image_size);
    /* CPU driver module tag, including command line and ELF image */
    size+= ALIGN(sizeof(struct multiboot_tag_module_64)
             + cfg->cpu_driver->args_len+1 + cfg->cpu_driver->image_size);
    /* All other modules */
    for(cmp= cfg->first_module; cmp; cmp= cmp->next) {
        size+= ALIGN(sizeof(struct multiboot_tag_module_64)
             + cmp->args_len+1 + cmp->image_size);
    }
    /* EFI memory map */
    size+= ALIGN(sizeof(struct multiboot_tag_efi_mmap)
         + MEM_MAP_SIZE);

    /* Round up to a page size and allocate. */
    npages= roundpage(size);
    cfg->multiboot= allocate_pages(npages, EfiBarrelfishMultibootData);
    if(!cfg->multiboot) {
        DebugPrint(DEBUG_ERROR, "allocate_pages: failed\n");
        return NULL;
    }
    memset(cfg->multiboot, 0, npages * PAGE_4k);
    AsciiPrint("Allocated %d pages for %dB multiboot info at %p.\n",
               npages, size, cfg->multiboot);

    cursor= cfg->multiboot;
    {
        //AsciiPrint("creating multiboot_header\n");
        //AsciiPrint("%-10a:%016lx\n","addr",cursor);

        struct multiboot_header *hdr = (struct multiboot_header *)cursor;
        hdr->magic = MULTIBOOT2_BOOTLOADER_MAGIC;
        hdr->architecture = MULTIBOOT_ARCHITECTURE_AARCH64;
        hdr->header_length = size;
        hdr->checksum = -(hdr->magic + hdr->architecture + hdr->header_length);
        cursor+= ALIGN(sizeof(struct multiboot_header));
        
        //AsciiPrint("%-10a:%d\n","magic",hdr->magic);
        //AsciiPrint("%-10a:%d\n","arch",hdr->architecture);
        //AsciiPrint("%-10a:%d\n","len",hdr->header_length);
        //AsciiPrint("%-10a:%08x\n\n","checksum",hdr->checksum);
    }
    /* Add the ELF section headers. */

    {
        //AsciiPrint("creating multiboot_tag_efi64\n");
        //AsciiPrint("%-10a:%016lx\n","addr",cursor);
        struct multiboot_tag_efi64 *efi=
            (struct multiboot_tag_efi64 *)cursor;

        efi->type = MULTIBOOT_TAG_TYPE_EFI64;
        efi->size = ALIGN(sizeof(struct multiboot_tag_efi64));
        efi->pointer = (uint64_t)cfg->cpu_driver_entry;

        cursor+= ALIGN(sizeof(struct multiboot_tag_efi64));

        //AsciiPrint("%-10a:%d\n","type",efi->type);
        //AsciiPrint("%-10a:%d\n","size",efi->size);
        //AsciiPrint("%-10a:%016lx\n\n","pointer",efi->pointer);
    }

    /* Add the boot command line */
    {
        //AsciiPrint("creating multiboot_tag_string\n");
        //AsciiPrint("%-10a:%016lx\n","addr",cursor);
        struct multiboot_tag_string *bootcmd=
            (struct multiboot_tag_string *)cursor;

        bootcmd->type= MULTIBOOT_TAG_TYPE_CMDLINE;
        bootcmd->size= ALIGN(sizeof(struct multiboot_tag_string)
                     + cfg->cpu_driver->args_len+1);
        ntstring(bootcmd->string,
                 cfg->buf + cfg->cpu_driver->args_start,
                 cfg->cpu_driver->args_len);

        cursor+= ALIGN(sizeof(struct multiboot_tag_string)
               + cfg->cpu_driver->args_len+1);

        //AsciiPrint("%-10a:%d\n","type",bootcmd->type);
        //AsciiPrint("%-10a:%d\n","size",bootcmd->size);
        //AsciiPrint("%-10a:%a\n\n","cmdline",bootcmd->string);
    }
    /* Add the boot command line */

    /* Add the DHCP ack packet. */
    {
        //AsciiPrint("creating multiboot_network\n");
        //AsciiPrint("%-10a:%016lx\n","addr",cursor);
        loader->prepare_multiboot_fn(loader, &cursor);
        //AsciiPrint("%-10a:%d\n","type",MULTIBOOT_TAG_TYPE_NETWORK);
        //UINTN _size = ALIGN(sizeof(struct multiboot_tag_network) + sizeof(EFI_PXE_BASE_CODE_PACKET));

        //AsciiPrint("%-10a:%d\n","size",_size);
        //AsciiPrint("%-10a:%a\n\n","dhcpack","NULL");
    }
    /* Add the ACPI 1.0 header */
    if(cfg->acpi1_header) {
        //AsciiPrint("creating multiboot_tag_old_acpi\n");
        //AsciiPrint("%-10a:%016lx\n","addr",cursor);
        struct multiboot_tag_old_acpi *acpi=
            (struct multiboot_tag_old_acpi *)cursor;

        acpi->type= MULTIBOOT_TAG_TYPE_ACPI_OLD;
        acpi->size= ALIGN(sizeof(struct multiboot_tag_old_acpi)
                  + sizeof(EFI_ACPI_1_0_ROOT_SYSTEM_DESCRIPTION_POINTER));
        memcpy(&acpi->rsdp[0], cfg->acpi1_header,
               sizeof(EFI_ACPI_1_0_ROOT_SYSTEM_DESCRIPTION_POINTER));

        cursor+= ALIGN(sizeof(struct multiboot_tag_old_acpi)
               + sizeof(EFI_ACPI_1_0_ROOT_SYSTEM_DESCRIPTION_POINTER));
        
        //AsciiPrint("%-10a:%d\n","type",acpi->type);
        //AsciiPrint("%-10a:%d\n","size",acpi->size);
        //AsciiPrint("%-10a:%a\n\n","rsdp","NULL");
    }
    /* Add the ACPI 2.0+ header */
    if(cfg->acpi2_header) {
        //AsciiPrint("creating multiboot_tag_new_acpi\n");
        //AsciiPrint("%-10a:%016lx\n","addr",cursor);
        struct multiboot_tag_new_acpi *acpi=
            (struct multiboot_tag_new_acpi *)cursor;

        acpi->type= MULTIBOOT_TAG_TYPE_ACPI_NEW;
        acpi->size= ALIGN(sizeof(struct multiboot_tag_new_acpi)
                  + sizeof(EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER));
        memcpy(&acpi->rsdp[0], cfg->acpi2_header,
               sizeof(EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER));

        cursor+= ALIGN(sizeof(struct multiboot_tag_new_acpi)
               + sizeof(EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER));

        //AsciiPrint("%-10a:%d\n","type",acpi->type);
        //AsciiPrint("%-10a:%d\n","size",acpi->size);
        //AsciiPrint("%-10a:%a\n\n","rsdp","NULL");
    }
    /* Add the boot driver module. */
    {
        //AsciiPrint("creating multiboot_tag_module_64\n");
        //AsciiPrint("%-10a:%016lx\n","addr",cursor);
        struct multiboot_tag_module_64 *kernel=
            (struct multiboot_tag_module_64 *)cursor;

        kernel->type= MULTIBOOT_TAG_TYPE_MODULE_64;
        kernel->size= ALIGN(sizeof(struct multiboot_tag_module_64)
                    + cfg->boot_driver->args_len+1  + cfg->boot_driver->image_size);
        kernel->mod_start=
            (multiboot_uint64_t)cfg->boot_driver->image_address;
        kernel->mod_end=
            (multiboot_uint64_t)(cfg->boot_driver->image_address +
                                 (cfg->boot_driver->image_size - 1));
        ntstring(kernel->cmdline,
                 cfg->buf + cfg->boot_driver->args_start,
                 cfg->boot_driver->args_len);

        cursor+= ALIGN(sizeof(struct multiboot_tag_module_64)
               + cfg->boot_driver->args_len+1 + cfg->boot_driver->image_size);

        //AsciiPrint("%-10a:%a\n","kind","boot_driver");
        //AsciiPrint("%-10a:%d\n","type",kernel->type);
        //AsciiPrint("%-10a:%d\n","size",kernel->size);
        //AsciiPrint("%-10a:%016lx\n","mod_start",kernel->mod_start);
        //AsciiPrint("%-10a:%016lx\n","mod_end",kernel->mod_end);
        //AsciiPrint("%-10a:%a\n\n","cmdline",kernel->cmdline);
    }
    /* Add the kernel module. */
    {
        //AsciiPrint("creating multiboot_tag_module_64\n");
        //AsciiPrint("%-10a:%016lx\n","addr",cursor);
        struct multiboot_tag_module_64 *kernel=
            (struct multiboot_tag_module_64 *)cursor;

        kernel->type= MULTIBOOT_TAG_TYPE_MODULE_64;
        kernel->size= ALIGN(sizeof(struct multiboot_tag_module_64)
                    + cfg->cpu_driver->args_len+1  + cfg->cpu_driver->image_size);
        kernel->mod_start=
            (multiboot_uint64_t)cfg->cpu_driver->image_address;
        kernel->mod_end=
            (multiboot_uint64_t)(cfg->cpu_driver->image_address +
                                 (cfg->cpu_driver->image_size - 1));
        ntstring(kernel->cmdline,
                 cfg->buf + cfg->cpu_driver->args_start,
                 cfg->cpu_driver->args_len);

        cursor+= ALIGN(sizeof(struct multiboot_tag_module_64)
               + cfg->cpu_driver->args_len+1 + cfg->cpu_driver->image_size);

        //AsciiPrint("%-10a:%a\n","kind","cpu_driver");
        //AsciiPrint("%-10a:%d\n","type",kernel->type);
        //AsciiPrint("%-10a:%d\n","size",kernel->size);
        //AsciiPrint("%-10a:%016lx\n","mod_start",kernel->mod_start);
        //AsciiPrint("%-10a:%016lx\n","mod_end",kernel->mod_end);
        //AsciiPrint("%-10a:%a\n\n","cmdline",kernel->cmdline);
    }
    /* Add the remaining modules */
    for(cmp= cfg->first_module; cmp; cmp= cmp->next) {
        //AsciiPrint("creating multiboot_tag_module_64\n");
        //AsciiPrint("%-10a:%016lx\n","addr",cursor);
        
        struct multiboot_tag_module_64 *module=
            (struct multiboot_tag_module_64 *)cursor;

        module->type= MULTIBOOT_TAG_TYPE_MODULE_64;
        module->size= ALIGN(sizeof(struct multiboot_tag_module_64)
                    + cmp->args_len+1 + cmp->image_size);
        module->mod_start=
            (multiboot_uint64_t)cmp->image_address;
        module->mod_end=
            (multiboot_uint64_t)(cmp->image_address +
                                 (cmp->image_size - 1));
        ntstring(module->cmdline, cfg->buf + cmp->args_start, cmp->args_len);

        cursor+= ALIGN(sizeof(struct multiboot_tag_module_64)
               + cmp->args_len+1 + cmp->image_size);

        //AsciiPrint("%-10a:%a\n","kind","other");
        //AsciiPrint("%-10a:%d\n","type",module->type);
        //AsciiPrint("%-10a:%d\n","size",module->size);
        //AsciiPrint("%-10a:%016lx\n","mod_start",module->mod_start);
        //AsciiPrint("%-10a:%016lx\n","mod_end",module->mod_end);
        //AsciiPrint("%-10a:%a\n\n","cmdline",module->cmdline);
    }
    /* Record the position of the memory map, to be filled in after we've
     * finished doing allocations. */
    //AsciiPrint("creating multiboot_tag_efi_mmap\n");
    //AsciiPrint("%-10a:%016lx\n","addr",cursor);
    cfg->mmap_tag= (struct multiboot_tag_efi_mmap *)cursor;
    cursor+= ALIGN(sizeof(struct multiboot_tag_efi_mmap));
    cfg->mmap_start= cursor;
    //AsciiPrint("%-10a:%016lx\n","mmap_addr",cursor);

    return cfg->multiboot;
}

void print_multiboot_layout(struct hagfish_config *cfg)
{
    void *cursor = cfg->multiboot;
    AsciiPrint("multiboot_header ----------------------------\n");
    {
        struct multiboot_header *data = (struct multiboot_header*)cursor;
        AsciiPrint("%-10a:%016lx\n","addr",data);
        AsciiPrint("%-10a:%d\n","magic",data->magic);
        AsciiPrint("%-10a:%d\n","arch",data->architecture);
        AsciiPrint("%-10a:%d\n","len",data->header_length);
        AsciiPrint("%-10a:%08x\n","checksum",data->checksum);
        cursor+= ALIGN(sizeof(struct multiboot_header));
    }
    AsciiPrint("multiboot_tag_efi64--------------------------\n");
    {
        struct multiboot_tag_efi64 *data = (struct multiboot_tag_efi64*)cursor;
        AsciiPrint("%-10a:%016lx\n","addr",data);
        AsciiPrint("%-10a:%d\n","type",data->type);
        AsciiPrint("%-10a:%d\n","size",data->size);
        AsciiPrint("%-10a:%016lx\n","pointer",data->pointer);
        cursor+= ALIGN(sizeof(struct multiboot_tag_efi64));
    }
    AsciiPrint("multiboot_tag_string-------------------------\n");
    {
        struct multiboot_tag_string *data = (struct multiboot_tag_string*)cursor;
        AsciiPrint("%-10a:%016lx\n","addr",data);
        AsciiPrint("%-10a:%d\n","type",data->type);
        AsciiPrint("%-10a:%d\n","size",data->size);
        AsciiPrint("%-10a:%a\n","cmdline",data->string);
        cursor+= ALIGN(sizeof(struct multiboot_tag_string)
               + cfg->cpu_driver->args_len+1);
    }
    AsciiPrint("multiboot_tag_network------------------------\n");
    {
        struct multiboot_tag_network *data = (struct multiboot_tag_network*)cursor;
        AsciiPrint("%-10a:%016lx\n","addr",data);
        AsciiPrint("%-10a:%d\n","type",data->type);
        AsciiPrint("%-10a:%d\n","size",data->size);
        AsciiPrint("%-10a:%a\n","dhcpack","NULL");
        cursor += ALIGN(sizeof(struct multiboot_tag_network)
         + sizeof(EFI_PXE_BASE_CODE_PACKET));
    }
    if(cfg->acpi1_header) {
        AsciiPrint("multiboot_tag_old_acpi-----------------------\n");
        struct multiboot_tag_old_acpi *data = 
            (struct multiboot_tag_old_acpi *)cursor;
        AsciiPrint("%-10a:%016lx\n","addr",data);
        AsciiPrint("%-10a:%d\n","type",data->type);
        AsciiPrint("%-10a:%d\n","size",data->size);
        AsciiPrint("%-10a:%a\n","rsdp","NULL");
        cursor+= ALIGN(sizeof(struct multiboot_tag_old_acpi)
               + sizeof(EFI_ACPI_1_0_ROOT_SYSTEM_DESCRIPTION_POINTER));
    }
    if(cfg->acpi2_header){
        AsciiPrint("multiboot_tag_new_acpi-----------------------\n");
        struct multiboot_tag_new_acpi *data =
            (struct multiboot_tag_new_acpi *)cursor;
        AsciiPrint("%-10a:%016lx\n","addr",data);
        AsciiPrint("%-10a:%d\n","type",data->type);
        AsciiPrint("%-10a:%d\n","size",data->size);
        AsciiPrint("%-10a:%a\n","dhcpack","rsdp");
        cursor+= ALIGN(sizeof(struct multiboot_tag_new_acpi)
               + sizeof(EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER));
    }
    AsciiPrint("multiboot_tag_module_64----------------------\n");
    {
        struct multiboot_tag_module_64 *data=
            (struct multiboot_tag_module_64 *)cursor;
        AsciiPrint("%-10a:%a\n","kind","boot_driver");
        AsciiPrint("%-10a:%016lx\n","addr",data);
        AsciiPrint("%-10a:%d\n","type",data->type);
        AsciiPrint("%-10a:%d\n","size",data->size);
        AsciiPrint("%-10a:%016lx\n","mod_start",data->mod_start);
        AsciiPrint("%-10a:%016lx\n","mod_end",data->mod_end);
        AsciiPrint("%-10a:%a\n","cmdline",data->cmdline);
        cursor+= ALIGN(sizeof(struct multiboot_tag_module_64)
               + cfg->boot_driver->args_len+1 + cfg->boot_driver->image_size);
        
        data = (struct multiboot_tag_module_64 *)cursor;
        AsciiPrint("%-10a:%a\n","kind","cpu_driver");
        AsciiPrint("%-10a:%016lx\n","addr",data);
        AsciiPrint("%-10a:%d\n","type",data->type);
        AsciiPrint("%-10a:%d\n","size",data->size);
        AsciiPrint("%-10a:%016lx\n","mod_start",data->mod_start);
        AsciiPrint("%-10a:%016lx\n","mod_end",data->mod_end);
        AsciiPrint("%-10a:%a\n","cmdline",data->cmdline);
        cursor+= ALIGN(sizeof(struct multiboot_tag_module_64)
               + cfg->cpu_driver->args_len+1 + cfg->cpu_driver->image_size);
        
        struct component_config *cmp;
        for(cmp= cfg->first_module; cmp; cmp = cmp->next){
            data = (struct multiboot_tag_module_64 *)cursor;
            AsciiPrint("%-10a:%a\n","kind","other");
            AsciiPrint("%-10a:%016lx\n","addr",data);
            AsciiPrint("%-10a:%d\n","type",data->type);
            AsciiPrint("%-10a:%d\n","size",data->size);
            AsciiPrint("%-10a:%016lx\n","mod_start",data->mod_start);
            AsciiPrint("%-10a:%016lx\n","mod_end",data->mod_end);
            AsciiPrint("%-10a:%a\n","cmdline",data->cmdline);
            cursor+= ALIGN(sizeof(struct multiboot_tag_module_64)
               + cmp->args_len+1 + cmp->image_size);
        }
    }
    AsciiPrint("multiboot_tag_efi_mmap-----------------------\n");
    {
        struct multiboot_tag_efi_mmap *data=
            (struct multiboot_tag_efi_mmap *)cursor;
        AsciiPrint("%-10a:%016lx\n","addr",data);
        AsciiPrint("%-10a:%d\n","type",data->type);
        AsciiPrint("%-10a:%d\n","size",data->size);
        AsciiPrint("%-10a:%d\n","descr_size",data->descr_size);
        AsciiPrint("%-10a:%d\n","descr_ver",data->descr_vers);
        AsciiPrint("%-10a:%016lx\n","mmap_addr",data->efi_mmap);
        
        AsciiPrint("efi_mmap_content-----------------------------\n");
        print_meomry_map_addr((uint64_t)data->efi_mmap);
    }
}

EFI_STATUS
relocate_elf(struct region_list *segments, Elf *elf,
             Elf64_Phdr *phdr, size_t phnum, size_t shnum, uint64_t kernel_offset) {
    EFI_STATUS status;
    size_t i;

    DebugPrint(DEBUG_INFO, "Relocating kernel image.\n");

    /* Search for relocaton sections. */
    for(i= 0; i < shnum; i++) {
        Elf_Scn *scn= elf_getscn(elf, i);
        if(!scn) {
            DebugPrint(DEBUG_ERROR, "elf_getscn: %a\n",
                       elf_errmsg(elf_errno()));
            return EFI_LOAD_ERROR;
        }

        Elf64_Shdr *shdr= elf64_getshdr(scn);
        if(!shdr) {
            DebugPrint(DEBUG_ERROR, "elf64_getshdr: %a\n",
                       elf_errmsg(elf_errno()));
            return EFI_LOAD_ERROR;
        }

        if(shdr->sh_type == SHT_REL ||
           shdr->sh_type == SHT_RELA) {
            if(shdr->sh_info != 0) {
                DebugPrint(DEBUG_ERROR,
                    "I expected global relocations, but got"
                    " section-specific ones.\n");
                return EFI_UNSUPPORTED;
            }

            /* Hardcoded for one loadable segment.
                XXX: seems to be not always the case for some ARMv8 builids.
            */
            //ASSERT(phnum == 1);

            Elf64_Addr segment_elf_base= phdr[0].p_vaddr;
            Elf64_Addr segment_load_base= segments->regions[0].base;
            Elf64_Sxword segment_delta= segment_load_base - segment_elf_base;

            /* Walk the section data descriptors. */
            Elf_Data *reldata;
            for(reldata= elf_getdata(scn, NULL);
                reldata;
                reldata= elf_getdata(scn, reldata)) {
                size_t rsize;
                if(shdr->sh_type == SHT_REL) rsize= sizeof(Elf64_Rel);
                else                         rsize= sizeof(Elf64_Rela);

                size_t nrel= reldata->d_size / rsize;

                /* Iterate through the relocations. */
                size_t i;
                for(i= 0; i < nrel; i++) {
                    void *reladdr= reldata->d_buf + i * rsize;
                    Elf64_Addr offset;
                    Elf64_Xword sym, type;
                    Elf64_Sxword addend;

                    if(shdr->sh_type == SHT_REL) {
                        DebugPrint(DEBUG_ERROR,
                                   "SHT_REL unimplemented.\n");
                        return EFI_LOAD_ERROR;
                    }
                    else { /* SHT_RELA */
                        Elf64_Rela *rel= reladdr;

                        offset= rel->r_offset;
                        sym= ELF64_R_SYM(rel->r_info);
                        type= ELF64_R_TYPE(rel->r_info);
                        addend= rel->r_addend;

                        uint64_t *rel_target= (void *)offset + segment_delta;

                        switch(type) {
                            case R_AARCH64_RELATIVE:
                                if(sym != 0) {
                                    DebugPrint(DEBUG_ERROR,
                                               "Relocation references a"
                                               " dynamic symbol, which is"
                                               " unsupported.\n");
                                    return EFI_UNSUPPORTED;
                                }

                                /* Delta(S) + A */
                                *rel_target= addend + segment_delta + kernel_offset;

#if 0
                                AsciiPrint("REL %p -> %llx\n",
                                           rel_target, *rel_target);
#endif
                                break;

                            default:
                                DebugPrint(DEBUG_ERROR,
                                           "Unsupported relocation type %d\n",
                                           type);
                                return EFI_UNSUPPORTED;
                        }
                    }

#if 0
                    AsciiPrint("REL: offset %llx, addend %llx, type %d"
                               ", symbol %d\n",
                               offset, addend, type, sym);
#endif
                }
            }
        }
    }

    return EFI_SUCCESS;
}

EFI_STATUS
prepare_component(struct hagfish_loader *loader, struct component_config *component,
                 struct region_list **load_segments, void ** ret_entry_point,
                 uint64_t kernel_offset) {
    EFI_STATUS status;
    size_t i;

    elf_version(EV_CURRENT);
    Elf *img_elf= elf_memory(component->image_address, component->image_size);
    if(!img_elf) {
        DebugPrint(DEBUG_ERROR, "elf_memory: %a\n", elf_errmsg(elf_errno()));
        return EFI_LOAD_ERROR;
    }

    const char *e_ident= elf_getident(img_elf, NULL);
    if(!e_ident) {
        DebugPrint(DEBUG_ERROR, "elf_getident: %a\n",
                   elf_errmsg(elf_errno()));
        return EFI_LOAD_ERROR;
    }

    if(e_ident[EI_CLASS] != ELFCLASS64 || e_ident[EI_DATA] != ELFDATA2LSB) {
        DebugPrint(DEBUG_ERROR, "Error: Not a 64-bit little-endian ELF\n");
        return EFI_LOAD_ERROR;
    }

    if(e_ident[EI_OSABI] != ELFOSABI_STANDALONE &&
       e_ident[EI_OSABI] != ELFOSABI_NONE) {
        DebugPrint(DEBUG_WARN,
                   "Warning: Compiled for OS ABI %d.  Wrong compiler?\n",
                   e_ident[EI_OSABI]);
    }

    Elf64_Ehdr *ehdr= elf64_getehdr(img_elf);
    if(!ehdr) {
        DebugPrint(DEBUG_ERROR, "elf64_getehdr: %a\n",
                   elf_errmsg(elf_errno()));
        return EFI_LOAD_ERROR;
    }

    if(ehdr->e_type != ET_EXEC) {
        DebugPrint(DEBUG_WARN,
                   "Warning: CPU driver isn't executable.  "
                   "Continuing anyway.\n");
    }

    if(ehdr->e_machine != EM_AARCH64) {
        DebugPrint(DEBUG_ERROR, "Error: Not AArch64\n");
        return EFI_LOAD_ERROR;
    }

    DebugPrint(DEBUG_INFO, "Unrelocated kernel entry point is %x\n",
               ehdr->e_entry);

    size_t phnum;
    status= elf_getphdrnum(img_elf, &phnum);
    if(status) {
        DebugPrint(DEBUG_ERROR, "elf64_getehdr: %a\n",
                   elf_errmsg(elf_errno()));
        return EFI_LOAD_ERROR;
    }
    DebugPrint(DEBUG_LOADFILE, "Found %d program header(s)\n", phnum);

    Elf64_Phdr *phdr= elf64_getphdr(img_elf);
    if(!phdr) {
        DebugPrint(DEBUG_ERROR, "elf64_getphdr: %a\n",
                   elf_errmsg(elf_errno()));
        return EFI_LOAD_ERROR;
    }

    /* Count the loadable segments, to allocate the region list. */
    size_t nloadsegs= 0;
    for(i= 0; i < phnum; i++) {
        if(phdr[i].p_type == PT_LOAD) nloadsegs++;
    }

    struct region_list *segments = malloc(sizeof(struct region_list) +
                                            nloadsegs * sizeof(struct ram_region));
    if(!segments) {
        DebugPrint(DEBUG_ERROR, "malloc: %s\n", strerror(errno));
        return EFI_OUT_OF_RESOURCES;
    }

    *load_segments = segments;

    segments->nregions= 0;

    /* Load the CPU driver from its ELF image. */
    int found_entry_point= 0;
    void *entry_point;
    for(i= 0; i < phnum; i++) {
        DebugPrint(DEBUG_LOADFILE,
                   "Segment %d load address %p, file size %x, memory size %x",
                   i, phdr[i].p_vaddr, phdr[i].p_filesz, phdr[i].p_memsz);
        if(phdr[i].p_type == PT_LOAD) DebugPrint(DEBUG_LOADFILE, " LOAD");
        DebugPrint(DEBUG_LOADFILE, "\n");
        if(phdr[i].p_type != PT_LOAD) continue;

        UINTN p_pages= COVER(phdr[i].p_memsz, PAGE_4k);
        void *p_buf;

        p_buf= allocate_pages(p_pages, EfiBarrelfishCPUDriver);
        if(!p_buf) {
            DebugPrint(DEBUG_ERROR, "allocate_pages: %r\n", status);
            return EFI_OUT_OF_RESOURCES;
        }
        memset(p_buf, 0, p_pages * PAGE_4k);
        DebugPrint(DEBUG_LOADFILE, "Loading into %d pages at %p\n",
                   p_pages, p_buf);

        segments->regions[i].base= (uint64_t)p_buf;
        segments->regions[i].npages= p_pages;
        segments->nregions++;

        memcpy(p_buf, component->image_address + phdr[i].p_offset,
               phdr[i].p_filesz);

        if(ehdr->e_entry >= phdr[i].p_vaddr &&
           ehdr->e_entry - phdr[i].p_vaddr < phdr[i].p_memsz) {
            entry_point=
                (cpu_driver_entry)(p_buf + (ehdr->e_entry - phdr[i].p_vaddr));
            found_entry_point= 1;
        }
    }

    size_t shnum;
    status= elf_getshdrnum(img_elf, &shnum);
    if(status) {
        DebugPrint(DEBUG_ERROR, "elf_getshdrnum: %a\n",
                   elf_errmsg(elf_errno()));
        return EFI_LOAD_ERROR;
    }

    status= relocate_elf(segments, img_elf, phdr, phnum, shnum, kernel_offset);
    if(EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "Relocation failed.\n");
        return EFI_LOAD_ERROR;
    }

    if(!found_entry_point) {
        DebugPrint(DEBUG_ERROR,
                   "Kernel entry point wasn't in any loaded segment.\n");
        return EFI_LOAD_ERROR;
    }

    *ret_entry_point = entry_point + kernel_offset;

    /* Finished with the kernel ELF. */
    elf_end(img_elf);

    return EFI_SUCCESS;
}

EFI_STATUS
prepare_boot_driver(struct hagfish_config *cfg, struct hagfish_loader *loader)
{
    EFI_STATUS status = prepare_component(loader, cfg->boot_driver,
                                          &cfg->boot_driver_segments,
                                          &cfg->boot_driver_entry, 0);
    DebugPrint(DEBUG_INFO,
               "Relocated boot driver entry point is %lp\n",
               cfg->boot_driver_entry);

    return status;
}


EFI_STATUS
prepare_cpu_driver(struct hagfish_config *cfg, struct hagfish_loader *loader)
{

    /* Allocate a stack */
    cfg->kernel_stack= allocate_pages(COVER(cfg->stack_size, PAGE_4k),
                                      EfiBarrelfishCPUDriverStack);
    if(!cfg->kernel_stack) {
        DebugPrint(DEBUG_ERROR, "Failed allocate kernel stack\n");
        return EFI_OUT_OF_RESOURCES;
    }


    EFI_STATUS status = prepare_component(loader, cfg->cpu_driver,
                                          &cfg->cpu_driver_segments,
                                          &cfg->cpu_driver_entry, KERNEL_OFFSET);
    DebugPrint(DEBUG_INFO,
               "Relocated CPU driver entry point is %lp, stack at %lp\n",
               cfg->cpu_driver_entry, cfg->kernel_stack);

    return status;
}

EFI_LOADED_IMAGE_PROTOCOL *
my_image(void) {
    EFI_LOADED_IMAGE_PROTOCOL *hag_image;
    EFI_STATUS status;

    /* Connect to the loaded image protocol. */
    status= gST->BootServices->OpenProtocol(
                gImageHandle, &gEfiLoadedImageProtocolGuid,
                (void **)&hag_image, gImageHandle, NULL,
                EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);

    if(EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "OpenProtocol: %r\n", status);
        return NULL;
    }

    return hag_image;
}

EFI_STATUS
image_done(void) {
    EFI_STATUS status;

    status= gST->BootServices->CloseProtocol(
                gImageHandle, &gEfiLoadedImageProtocolGuid,
                gImageHandle, NULL);
    //if(EFI_ERROR(status)) {
    //    DebugPrint(DEBUG_ERROR, "CloseProtocol: %r\n", status);
    //    return status;

    return EFI_SUCCESS;
}

struct hagfish_config *
load_config(struct hagfish_loader *loader) {
    EFI_STATUS status;

    /* Load the host-specific configuration file. */
    char cfg_filename[256];
    UINTN cfg_size;
    status = loader->config_file_name_fn(loader, cfg_filename, 256);
    if (EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "config file name failed: %r\n", status);
        return NULL;
    }
    DebugPrint(DEBUG_LOADFILE, "Loading \"%a\"\n", cfg_filename);

    /* Get the file size.  Note that even though this call doesn't touch the
     * supplied buffer (argument 3), it still fails if it's null.  Thus the
     * nonsense parameter. */
    status = loader->size_fn(loader, cfg_filename, &cfg_size);
    if(EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "file size: %r\n", status);
        return NULL;
    }
    DebugPrint(DEBUG_LOADFILE, "File \"%a\" has size %dB\n",
               cfg_filename, cfg_size);

    void *cfg_buffer= malloc(cfg_size);
    if(!cfg_buffer) {
        DebugPrint(DEBUG_ERROR, "malloc: %a\n", strerror(errno));
        return NULL;
    }

    status = loader->read_fn(loader, cfg_filename, &cfg_size, cfg_buffer);
    if(EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "read file: %r\n", status);
        return NULL;
    }
    DebugPrint(DEBUG_LOADFILE, "Loaded config at [%p-%p]\n",
               cfg_buffer, cfg_buffer + cfg_size - 1);

    DebugPrint(DEBUG_INFO, "Parsing configuration...");
    /* Parse the configuration file. */
    struct hagfish_config *cfg= parse_config(cfg_buffer, cfg_size);
    if(!cfg) {
        DebugPrint(DEBUG_ERROR, "Failed to parse Hagfish configuration.\n");
        return NULL;
    }
    DebugPrint(DEBUG_INFO, " done\n");

    return cfg;
}

EFI_STATUS
configure_loader(struct hagfish_loader *loader, EFI_HANDLE ImageHandle,
        EFI_SYSTEM_TABLE *SystemTable, EFI_LOADED_IMAGE_PROTOCOL *hag_image, int try_shell) {

    EFI_STATUS status;

    loader->imageHandle = ImageHandle;
    loader->systemTable = SystemTable;
    loader->hagfishImage = hag_image;

    if (!try_shell) {
        // could not connect to shell.
        DebugPrint(DEBUG_INFO, "Could not connect to shell or not enough parameters, assuming PXE boot.\n");
        status = hagfish_loader_pxe_init(loader);
    }
    else{
        DebugPrint(DEBUG_INFO,"try local file system");
        status = hagfish_loader_local_fs_init(loader,L"/menu.lst");
    }

    return status;
}

/**
  Return the pointer to the first occurrence of any character from a list of characters.

  @param[in] String           the string to parse
  @param[in] CharacterList    the list of character to look for
  @param[in] EscapeCharacter  An escape character to skip

  @return the location of the first character in the string
  @retval CHAR_NULL no instance of any character in CharacterList was found in String
**/
CONST CHAR16*
EFIAPI
FindFirstCharacter(
  IN CONST CHAR16 *String,
  IN CONST CHAR16 *CharacterList,
  IN CONST CHAR16 EscapeCharacter
  )
{
  UINT32 WalkChar;
  UINT32 WalkStr;

  for (WalkStr = 0; WalkStr < StrLen(String); WalkStr++) {
    if (String[WalkStr] == EscapeCharacter) {
      WalkStr++;
      continue;
    }
    for (WalkChar = 0; WalkChar < StrLen(CharacterList); WalkChar++) {
      if (String[WalkStr] == CharacterList[WalkChar]) {
        return (&String[WalkStr]);
      }
    }
  }
  return (String + StrLen(String));
}
/**
  Return the next parameter's end from a command line string.

  @param[in] String        the string to parse
**/
CONST CHAR16*
EFIAPI
FindEndOfParameter(
  IN CONST CHAR16 *String
  )
{
  CONST CHAR16 *First;
  CONST CHAR16 *CloseQuote;

  First = FindFirstCharacter(String, L" \"", L'^');

  //
  // nothing, all one parameter remaining
  //
  if (*First == CHAR_NULL) {
    return (First);
  }

  //
  // If space before a quote (or neither found, i.e. both CHAR_NULL),
  // then that's the end.
  //
  if (*First == L' ') {
    return (First);
  }

  CloseQuote = FindFirstCharacter (First+1, L"\"", L'^');

  //
  // We did not find a terminator...
  //
  if (*CloseQuote == CHAR_NULL) {
    return (NULL);
  }

  return (FindEndOfParameter (CloseQuote+1));
}
/**
  Return the next parameter from a command line string.

  This function moves the next parameter from Walker into TempParameter and moves
  Walker up past that parameter for recursive calling.  When the final parameter
  is moved *Walker will be set to NULL;

  Temp Parameter must be large enough to hold the parameter before calling this
  function.

  This will also remove all remaining ^ characters after processing.

  @param[in, out] Walker        pointer to string of command line.  Adjusted to
                                reminaing command line on return
  @param[in, out] TempParameter pointer to string of command line item extracted.
  @param[in]      Length        buffer size of TempParameter.

  @return   EFI_INALID_PARAMETER  A required parameter was NULL or pointed to a NULL or empty string.
  @return   EFI_NOT_FOUND         A closing " could not be found on the specified string
**/
EFI_STATUS
EFIAPI
GetNextParameter(
  IN OUT CHAR16   **Walker,
  IN OUT CHAR16   **TempParameter,
  IN CONST UINTN  Length
  )
{
  CONST CHAR16 *NextDelim;

  if (Walker           == NULL
    ||*Walker          == NULL
    ||TempParameter    == NULL
    ||*TempParameter   == NULL
    ){
    return (EFI_INVALID_PARAMETER);
  }


  //
  // make sure we dont have any leading spaces
  //
  while ((*Walker)[0] == L' ') {
      (*Walker)++;
  }

  //
  // make sure we still have some params now...
  //
  if (StrLen(*Walker) == 0) {
DEBUG_CODE_BEGIN();
    *Walker        = NULL;
DEBUG_CODE_END();
    return (EFI_INVALID_PARAMETER);
  }

  NextDelim = FindEndOfParameter(*Walker);

  if (NextDelim == NULL){
DEBUG_CODE_BEGIN();
    *Walker        = NULL;
DEBUG_CODE_END();
    return (EFI_NOT_FOUND);
  }

  StrnCpyS(*TempParameter, Length, (*Walker), NextDelim - *Walker);

  //
  // Add a CHAR_NULL if we didnt get one via the copy
  //
  if (*NextDelim != CHAR_NULL) {
    (*TempParameter)[NextDelim - *Walker] = CHAR_NULL;
  }

  //
  // Update Walker for the next iteration through the function
  //
  *Walker = (CHAR16*)NextDelim;

  //
  // Remove any non-escaped quotes in the string
  // Remove any remaining escape characters in the string
  //
  for (NextDelim = FindFirstCharacter(*TempParameter, L"\"^", CHAR_NULL) 
    ; *NextDelim != CHAR_NULL 
    ; NextDelim = FindFirstCharacter(NextDelim, L"\"^", CHAR_NULL)
    ) {
    if (*NextDelim == L'^') {

      //
      // eliminate the escape ^
      //
      CopyMem ((CHAR16*)NextDelim, NextDelim + 1, StrSize (NextDelim + 1));
      NextDelim++;
    } else if (*NextDelim == L'\"') {

      //
      // eliminate the unescaped quote
      //
      CopyMem ((CHAR16*)NextDelim, NextDelim + 1, StrSize (NextDelim + 1));
    }
  }

  return EFI_SUCCESS;
}
/**
  Function to populate Argc and Argv.

  This function parses the CommandLine and divides it into standard C style Argc/Argv
  parameters for inclusion in EFI_SHELL_PARAMETERS_PROTOCOL.  this supports space
  delimited and quote surrounded parameter definition.

  All special character processing (alias, environment variable, redirection, 
  etc... must be complete before calling this API.

  @param[in] CommandLine         String of command line to parse
  @param[in, out] Argv           pointer to array of strings; one for each parameter
  @param[in, out] Argc           pointer to number of strings in Argv array

  @return EFI_SUCCESS           the operation was sucessful
  @return EFI_OUT_OF_RESOURCES  a memory allocation failed.
**/
EFI_STATUS
EFIAPI
ParseCommandLineToArgs(
  IN CONST CHAR16 *CommandLine,
  IN OUT CHAR16 ***Argv,
  IN OUT UINTN *Argc
  )
{
  UINTN       Count;
  CHAR16      *TempParameter;
  CHAR16      *Walker;
  CHAR16      *NewParam;
  UINTN       Size;

  ASSERT(Argc != NULL);
  ASSERT(Argv != NULL);

  if (CommandLine == NULL || StrLen(CommandLine)==0) {
    (*Argc) = 0;
    (*Argv) = NULL;
    return (EFI_SUCCESS);
  }

  Size = StrSize(CommandLine);
  TempParameter = AllocateZeroPool(Size);
  if (TempParameter == NULL) {
    return (EFI_OUT_OF_RESOURCES);
  }

  for ( Count = 0
      , Walker = (CHAR16*)CommandLine
      ; Walker != NULL && *Walker != CHAR_NULL
      ; Count++
      ) {
    if (EFI_ERROR(GetNextParameter(&Walker, &TempParameter, Size))) {
      break;
    }
  }

  //
  // lets allocate the pointer array
  //
  (*Argv) = AllocateZeroPool((Count)*sizeof(CHAR16*));
  if (*Argv == NULL) {
    SHELL_FREE_NON_NULL(TempParameter);
    return (EFI_OUT_OF_RESOURCES);
  }

  *Argc = 0;
  Walker = (CHAR16*)CommandLine;
  while(Walker != NULL && *Walker != CHAR_NULL) {
    SetMem16(TempParameter, Size, CHAR_NULL);
    if (EFI_ERROR(GetNextParameter(&Walker, &TempParameter, Size))) {
      SHELL_FREE_NON_NULL(TempParameter);
      return (EFI_INVALID_PARAMETER);
    }

    NewParam = AllocateCopyPool(StrSize(TempParameter), TempParameter);
    if (NewParam == NULL){
      SHELL_FREE_NON_NULL(TempParameter);
      return (EFI_OUT_OF_RESOURCES);
    }
    ((CHAR16**)(*Argv))[(*Argc)] = NewParam;
    (*Argc)++;
  }
  ASSERT(Count >= (*Argc));
  SHELL_FREE_NON_NULL(TempParameter);
  return (EFI_SUCCESS);
}

EFI_STATUS
UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE_PROTOCOL *hag_image;
    int i, try_shell=1;

    gBS->SetWatchdogTimer (0, 0, 0, NULL);

    //AsciiPrint("try_shell = %d\n", try_shell);
    AsciiPrint("Hagfish UEFI loader starting\n");

    DebugPrint(DEBUG_INFO, "UEFI vendor: %s\n", gST->FirmwareVendor);

    /* Get the details of our own process image. */
    hag_image = my_image();
    if(!hag_image) return EFI_SUCCESS;

    DebugPrint(DEBUG_INFO, "Hagfish loaded at %p, size %dB, by handle %p\n",
        hag_image->ImageBase, hag_image->ImageSize, hag_image->DeviceHandle);

#if WAIT_FOR_GDB
    /*
        This waits with the execution until you set the variable wait
        in GDB to zero.

        Enable this in Hagfish.dsc
     */
    volatile int wait = 1;
    volatile int counter = 0;
    AsciiPrint("Waiting for release... (gdb) set variable wait = 0\n");
    while (wait) {
        counter++;
    }
#endif

    struct hagfish_loader loader;
    memset(&loader, 0, sizeof(loader));

    status = configure_loader(&loader, ImageHandle, SystemTable, hag_image, try_shell);
    if (EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "Failed to initialize loader: %r\n", status);
        return EFI_SUCCESS;
    }

    /* Load and parse the configuration file. */
    struct hagfish_config *cfg= load_config(&loader);
    if(!cfg) return EFI_SUCCESS;

    /* looking for ACPI tables */
    status = acpi_find_root_table(cfg);
    if(!EFI_ERROR(status)) {
        status = acpi_parse_madt(cfg);
        if(EFI_ERROR(status)) {
            DebugPrint(DEBUG_ERROR, "ACPI: could not parse MADT. Info not available\n");
        }
    } else {
        DebugPrint(DEBUG_ERROR, "ACPI: root tables not found.\n");
    }

    /* Load the boot driver. */
    DebugPrint(DEBUG_INFO, "Loading the boot driver [");
    if(!load_component(&loader, cfg->boot_driver, cfg->buf)) {
        DebugPrint(DEBUG_ERROR, "\nFailed to load the kernel.\n");
        return EFI_SUCCESS;
    }
    DebugPrint(DEBUG_INFO, "].\n");

    /* Load the kernel. */
    DebugPrint(DEBUG_INFO, "Loading the cpu driver [");
    if(!load_component(&loader, cfg->cpu_driver, cfg->buf)) {
        DebugPrint(DEBUG_ERROR, "\nFailed to load the kernel.\n");
        return EFI_SUCCESS;
    }
    DebugPrint(DEBUG_INFO, "].\n");

    /* Load the modules */
    DebugPrint(DEBUG_INFO, "Loading init images [");
    {
        struct component_config *cmp;

        for(cmp= cfg->first_module; cmp; cmp= cmp->next) {
            if(cmp != cfg->first_module) DebugPrint(DEBUG_INFO, ", ");

            if(!load_component(&loader, cmp, cfg->buf)) {
                DebugPrint(DEBUG_ERROR, "Failed to load module.\n");
                return EFI_SUCCESS;
            }
        }
    }
    DebugPrint(DEBUG_INFO, "].\n");

    /* Print out the discovered RAM regions */
    status= update_ram_regions(cfg);
    if(EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "Failed to get RAM regions.\n");
        return EFI_SUCCESS;
    }
    print_ram_regions(cfg->ram_regions);

    /* Build the direct-mapped page tables for the kernel. */
    status= build_page_tables(cfg);
    if(EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "Failed to create initial page table.\n");
        return EFI_SUCCESS;
    }

    status = prepare_boot_driver(cfg, &loader);
    if(EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "Failed to prepare boot driver.\n");
        return EFI_SUCCESS;
    }

    /* Load the CPU driver from its ELF image, and relocate it. */
    status= prepare_cpu_driver(cfg, &loader);
    if(EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "Failed to prepare CPU driver.\n");
        return EFI_SUCCESS;
    }

    /* Create the multiboot header. */
    if(!create_multiboot_info(cfg, &loader)) {
        DebugPrint(DEBUG_ERROR, "Failed to create multiboot structure.\n");
        return EFI_SUCCESS;
    }

    /* Finished with loading. */
    status= loader.done_fn(&loader);
    if(EFI_ERROR(status)) return EFI_SUCCESS;
    /* loader is now invalid. */

    /* Finished with the loaded image protocol. */
    status= image_done();
    if(EFI_ERROR(status)) return EFI_SUCCESS;
    /* hag_image is now invalid. */

    status= arch_probe();
    if(EFI_ERROR(status)) return EFI_SUCCESS;

    /* Save the kernel entry point and other pointers (we're about to free
     * cfg).  As these copies are sitting on our stack, they'll be freed when
     * the CPU driver recycles Hagfish's memory regions. */
    void *kernel_entry= cfg->boot_driver_entry;
    void *multiboot= cfg->multiboot;
    void *kernel_stack= cfg->kernel_stack;
    size_t stack_size= cfg->stack_size;
    void *root_table= get_root_table(cfg);

    ASSERT(kernel_entry);
    ASSERT(multiboot);
    ASSERT(kernel_stack);
    ASSERT(stack_size > 0);
    ASSERT(root_table);

    /* Exit EFI boot services. */
    AsciiPrint("Terminating boot services and jumping to image at %p\n",
               kernel_entry);
    AsciiPrint("New stack pointer is %p   [%p..%p]  0x%p kB\n",
               kernel_stack + stack_size - 16, kernel_stack,
               kernel_stack + stack_size, stack_size >> 10);
    AsciiPrint("Multiboot2 pointer is %p\n", multiboot);

    free_bookkeeping(cfg);

    /* The last thing we do is to grab the final memory map, including any
     * allocations and deallocations we've done, as per the UEFI spec
     * recommendation.  This fills in the space we set aside in the multiboot
     * structure. */
    status = update_memory_map();
    if(EFI_ERROR(status)) return EFI_SUCCESS;

    // Relocate EFI's memory map to the kernel virtual address space.
    status = relocate_memory_map();
    if(EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "relocate memory map: %r\n", status);
        return EFI_SUCCESS;
    }
    //print_memory_map(0);
    //print_multiboot_layout(cfg);

    /* Free all dynamically-allocated configuration that we're not passing to
     * the CPU driver. */

    /* Fill in the tag.  We can't use GetMemoryMap to fill these directly, as
     * the multiboot specification requires them to be 32 bit, while EFI may
     * return 64-bit values.  Note that 'mmap_tag' points *inside* the
     * structure pointed to by 'multiboot'. */
    cfg->mmap_tag->type= MULTIBOOT_TAG_TYPE_EFI_MMAP;
    cfg->mmap_tag->size= sizeof(struct multiboot_tag_efi_mmap) + mmap_size;
    cfg->mmap_tag->descr_size= mmap_d_size;
    cfg->mmap_tag->descr_vers= mmap_d_ver;
    memcpy(cfg->mmap_start, mmap, mmap_size);

    //print_multiboot_layout(cfg);

    status= gST->BootServices->ExitBootServices(
        gImageHandle, mmap_key);
    if(EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "ExitBootServices: ERROR %r, %x\n", status, mmap_key);
        return status;
    }

    /*** EFI boot services are now terminated, we're on our own. */

    /* Do MMU configuration, switch page tables. */
    arch_init(root_table);

    /* Jump to the start of the loaded image - doesn't return.

       ASSUMPTIONS:
         - the code starts executing in LOW address.
         - The exeception level is the highest that is supported.
         - stack pointer and pointers to multiboot are in LOW memory
     */
    SwitchStack((SWITCH_STACK_ENTRY_POINT)kernel_entry,
                (void *)(uintptr_t)(MULTIBOOT2_BOOTLOADER_MAGIC),
                (void *)multiboot,
                (void *)(kernel_stack + stack_size - 16));

    status = set_memory_map();
    if(EFI_ERROR(status)) {
        DebugPrint(DEBUG_ERROR, "set memory map: %r\n", status);
        return EFI_SUCCESS;
    }

    return EFI_SUCCESS;
}
