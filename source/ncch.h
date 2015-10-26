// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

////////////////////////////////////////////////////////////////////////////////////////////////////
/// NCCH header (Note: "NCCH" appears to be a publically unknown acronym)

enum class NCCHContentPlatform : u8 {
    Old3DS = 1,
    New3DS = 2,
};

enum class NCCHContentType : u8 {
    Data         =  0x1,
    Executable   =  0x2,
    SystemUpdate =  0x4,
    Manual       =  0x8,
    Trial        = 0x10,
};

inline NCCHContentType operator|(NCCHContentType a, NCCHContentType b) {
    return static_cast<NCCHContentType>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

enum class NCCHCrypto : u8 {
    FixedCryptoKey = 0x1,
    NoMountRomFS = 0x2,
    NoCrypto = 0x4,
    NewKeyYGen = 0x20,
};

struct NCCH_Header {
    u8 signature[0x100];
    u32 magic;
    u32 content_size;
    u8 partition_id[8];
    u16 maker_code;
    u16 version;
    u8 reserved_0[4];
    uint64_t program_id;
    u8 reserved_1[0x10];
    u8 logo_region_hash[0x20];
    u8 product_code[0x10];
    u8 extended_header_hash[0x20];
    u32 extended_header_size;
    u8 reserved_2[4];
    struct {
        u8 unk[3];
        u8 enable_crypto;
        NCCHContentPlatform content_platform;
        NCCHContentType content_type;
        u8 content_unit_size; // mediaunitsize = 0x200 * pow2(content_unit_size)
        NCCHCrypto crypto;
	} flags;
    u32 plain_region_offset;
    u32 plain_region_size;
    u32 logo_region_offset;
    u32 logo_region_size;
    u32 exefs_offset;
    u32 exefs_size;
    u32 exefs_hash_region_size;
    u8 reserved_3[4];
    u32 romfs_offset;
    u32 romfs_size;
    u32 romfs_hash_region_size;
    u8 reserved_4[4];
    u8 exefs_super_block_hash[0x20];
    u8 romfs_super_block_hash[0x20];
};

static_assert(sizeof(NCCH_Header) == 0x200, "NCCH header structure size is wrong");

////////////////////////////////////////////////////////////////////////////////////////////////////
// ExeFS (executable file system) headers

struct ExeFs_SectionHeader {
    char name[8];
    u32 offset;
    u32 size;
};

struct ExeFs_Header {
    ExeFs_SectionHeader section[8];
    u8 reserved[0x80];
    u8 hashes[8][0x20];
};
static_assert(sizeof(ExeFs_Header) == 0x200, "ExeFs header structure size is wrong");

////////////////////////////////////////////////////////////////////////////////////////////////////
// ExHeader (executable file system header) headers

struct ExHeader_SystemInfoFlags {
    u8 reserved[5];
    u8 flag;
    u8 remaster_version[2];
};

struct ExHeader_CodeSegmentInfo {
    u32 address;
    u32 num_max_pages;
    u32 code_size;
};

struct ExHeader_CodeSetInfo {
    u8 name[8];
    ExHeader_SystemInfoFlags flags;
    ExHeader_CodeSegmentInfo text;
    u32 stack_size;
    ExHeader_CodeSegmentInfo ro;
    u8 reserved[4];
    ExHeader_CodeSegmentInfo data;
    u32 bss_size;
};

struct ExHeader_DependencyList {
    u8 program_id[0x30][8];
};

struct ExHeader_SystemInfo {
    u64 save_data_size;
    u8 jump_id[8];
    u8 reserved_2[0x30];
};

struct ExHeader_StorageInfo {
    u8 ext_save_data_id[8];
    u8 system_save_data_id[8];
    u8 reserved[8];
    u8 access_info[7];
    u8 other_attributes;
};

struct ExHeader_ARM11_SystemLocalCaps {
    uint64_t program_id;
    u32 core_version;
    u8 reserved_flags[2];
    u8 flags0;
    u8 priority;
    u8 resource_limit_descriptor[0x10][2];
    ExHeader_StorageInfo storage_info;
    u8 service_access_control[0x20][8];
    u8 ex_service_access_control[0x2][8];
    u8 reserved[0xf];
    u8 resource_limit_category;
};

struct ExHeader_ARM11_KernelCaps {
    u32 descriptors[28];
    u8 reserved[0x10];
};

struct ExHeader_ARM9_AccessControl {
    u8 descriptors[15];
    u8 descversion;
};

struct ExHeader_Header {
    ExHeader_CodeSetInfo codeset_info;
    ExHeader_DependencyList dependency_list;
    ExHeader_SystemInfo system_info;
    ExHeader_ARM11_SystemLocalCaps arm11_system_local_caps;
    ExHeader_ARM11_KernelCaps arm11_kernel_caps;
    ExHeader_ARM9_AccessControl arm9_access_control;
    struct {
        u8 signature[0x100];
        u8 ncch_public_key_modulus[0x100];
        ExHeader_ARM11_SystemLocalCaps arm11_system_local_caps;
        ExHeader_ARM11_KernelCaps arm11_kernel_caps;
        ExHeader_ARM9_AccessControl arm9_access_control;
    } access_desc;
};

static_assert(sizeof(ExHeader_Header) == 0x800, "ExHeader structure size is wrong");


struct RomFSHeader {
    u8 magic[4];
};

struct RomFSInfoHeader {
    u8 headersize[4];
    struct RomFSSectionHeader
    {
        u8 offset[4];
        u8 size[4];
    } section[4];

    u8 dataoffset[4];
}; // at offset 0x1000
