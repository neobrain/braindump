#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <vector>
#include <inttypes.h>

#include <sys/stat.h>
#include <errno.h>

#include <3ds.h>

#include "ncch.h"

// Utility function to convert a value to a fixed-width string of (sizeof(T)*2+2) digits, e.g. "0x0123" for a uint16_t argument.
template<typename T>
static std::string fixed_width_hex(T value) {
    std::stringstream ss;

    // If the given value is of type (un)signed char, cast it to unsigned instead
    using core_type = typename std::remove_cv<typename std::make_unsigned<T>::type>::type;
    using target_type = typename std::conditional<std::is_same<unsigned char, core_type>::value, unsigned, core_type>::type;
    auto value_as_integer = static_cast<target_type>(static_cast<core_type>(value));

    ss << "0x" << std::hex << std::setw(sizeof(T)*2) << std::setfill('0') << value_as_integer;
    return ss.str();
}

class WriteResult {
    Result res;

public:
    WriteResult(Result res) : res(res) {}
	friend std::ostream& operator<<(std::ostream& os, const WriteResult& result);
};

std::ostream& operator<<(std::ostream& os, const WriteResult& result) {
    std::ios::fmtflags f( os.flags() );
    os << fixed_width_hex(result.res);
    os.flags(f);
    return os;
}

static std::string ResultToString(Result res) {
    std::stringstream ss;
    ss << WriteResult(res);
    return ss.str();
}

static Result MYFSUSER_GetMediaType(Handle fsuHandle, u8* mediatype) {
    u32* cmdbuf = getThreadCommandBuffer();

    cmdbuf[0] = IPC_MakeHeader(0x868,0,0); // 0x8680000

    Result ret = 0;
    if((ret = svcSendSyncRequest(fsuHandle)))
        return ret;

    if(mediatype)
        *mediatype = cmdbuf[2];

    return cmdbuf[1];
}

static Result GetTitleInformation(u8* mediatype, uint64_t* tid) {
    Result ret = 0;

    if(mediatype) {
        Handle localFsHandle;

        // Create temporary FS handle to get the proper media type
        ret = srvGetServiceHandleDirect(&localFsHandle, "fs:USER");
        if(ret) {
            svcCloseHandle(localFsHandle);
            std::cout << "srvGetServiceHandleDirect error: " << ResultToString(ret) << std::endl;
            return ret;
        }

        ret = FSUSER_Initialize(localFsHandle);
        if(ret) {
            svcCloseHandle(localFsHandle);
            std::cout << "FSUSER_Initialize error: " << ResultToString(ret) << std::endl;
            return ret;
        }

        ret = MYFSUSER_GetMediaType(localFsHandle, mediatype);
        if (ret != 0) {
            svcCloseHandle(localFsHandle);
            std::cout << "FSUSER_GetMediaType error: " << ResultToString(ret) << std::endl;
            return ret;
        }

        svcCloseHandle(localFsHandle);
    }

    if(tid) {
        aptOpenSession();
        ret = APT_GetProgramID(tid);
        if (ret != 0)
            std::cout << "APT_GetProgramID error: " << ResultToString(ret) << std::endl;
        aptCloseSession();
    }

    return ret;
}

enum class ContentType : uint32_t {
    ROMFS = 0,
    EXEFS = 2,
};

static std::vector<uint8_t> ReadTitleContent(uint64_t title_id, uint8_t media_type, ContentType type, const std::string& name) {
    uint32_t archivePath[] = { (uint32_t)(title_id & 0xFFFFFFFF), (uint32_t)(title_id >> 32), media_type, 0x00000000};
    FS_Path fs_archive_path = { PATH_BINARY, 0x10, (u8*)archivePath };
    FS_Archive fs_archive = { 0x2345678a, fs_archive_path };

    struct LowPathData {
        uint32_t unk[3];
        std::array<char, 8> filename; // NOTE: Archive 0x2345678a expects this particular size!
    } data = { { 0, 0, static_cast<uint32_t>(type) }, {} };

    // Copy filename including null terminator
    assert(name.size() + 1 <= data.filename.size());
    std::fill(data.filename.begin(), data.filename.end(), 0);
    std::copy(name.c_str(), name.c_str() + name.size() + 1, data.filename.begin());

    Handle file_handle;
    Result ret = FSUSER_OpenFileDirectly(//nullptr,
                                         &file_handle,
                                         fs_archive,
                                         (FS_Path) { PATH_BINARY, sizeof(data), (u8*)&data },
                                         FS_OPEN_READ,
                                         0);

    if (ret != 0) {
        std::cout << "Couldn't open \"ExeFS/" << name << "\" for reading (error " << ResultToString(ret) << ")" << std::endl;
        return {};
    }

    uint64_t size;
    ret = FSFILE_GetSize(file_handle, &size);
    if (ret != 0 || !size) {
        std::cout << "Couldn't get file size for \"ExeFS/" << name << "\" (error " << ResultToString(ret) << ")" << std::endl;
        FSFILE_Close(file_handle);
        return {};
    } else {
        std::cout << (size / 1024) << " KiB... " << std::flush;
    }

    std::vector<uint8_t> content(size);

    uint32_t bytes_read;
    ret = FSFILE_Read(file_handle, &bytes_read, 0, content.data(), size);
    if (ret != 0 || bytes_read != size) {
        std::cout << "Expected to read " << size << " bytes, read" << bytes_read << std::endl;
        FSFILE_Close(file_handle);
        return {};
    } else {
    }

    FSFILE_Close(file_handle);
    return content;
}

static uint32_t RoundUpToMediaUnit(uint32_t value) {
    return (value + 0x1FF) / 0x200 * 0x200;
}

static uint32_t BytesToMediaUnits(uint32_t value) {
    return RoundUpToMediaUnit(value) / 0x200;
}

static uint32_t RoundUpToPageSize(uint32_t value) {
    return (value + 0xFFF) / 0x1000 * 0x1000;
}

template<typename Container>
static ExeFs_SectionHeader WriteSection(const Container& cont, const std::string& section_name, std::ofstream& exefs_file, std::ofstream::pos_type exefs_header_end) {
    // Write section data to file
    const auto section_begin = exefs_file.tellp();
    std::copy(cont.begin(), cont.end(), std::ostream_iterator<uint8_t>(exefs_file));
    const auto section_end = exefs_file.tellp();

    // Pad with zeros to media unit size
    uint32_t size = section_end - section_begin;
    uint32_t padding = RoundUpToMediaUnit(size) - size;
    std::generate_n(std::ostream_iterator<char>(exefs_file), padding, [] { return 0; }); // TODO: Use WriteDummyBytes instead

    // Build and return header - don't include padding in the reported size
    ExeFs_SectionHeader ret{{}, (uint32_t)(section_begin - exefs_header_end), size };
    std::strcpy(ret.name, section_name.c_str());
    return ret;
}

// Returns the size of the decompressed .code section
static uint32_t DumpExeFS(std::ofstream& exefs_file, uint64_t title_id, uint8_t mediatype) {
    // Generate dummy ExeFS header to fill in later
    const auto exefs_header_begin = exefs_file.tellp();
    std::generate_n(std::ostream_iterator<uint8_t>(exefs_file), sizeof(ExeFs_Header), []{return 0;}); // TODO: Use WriteDummyBytes instead
    const auto exefs_header_end = exefs_file.tellp();

    // Write content sections
    ExeFs_Header exefs_header;
    auto exefs_section_header_it = std::begin(exefs_header.section);

    uint32_t size_decompressed_code;

    {
        // Load decompressed code data
        std::cout << "\tDumping code... " << std::flush;
        auto contents = ReadTitleContent(title_id, mediatype, ContentType::EXEFS, ".code");
        if (contents.empty())
            return 0;
        *exefs_section_header_it++ = WriteSection(contents, ".code", exefs_file, exefs_header_end);
        std::cout << "done!" << std::endl;

        u8* size_diff_ptr = &contents[contents.size() - 4];
        u32 size_diff = size_diff_ptr[0] | (size_diff_ptr[1] << 8) | (size_diff_ptr[2] << 16) | (size_diff_ptr[3] << 24);
        size_decompressed_code = contents.size() + size_diff;
    }

    {
        std::cout << "\tDumping banner... " << std::flush;
        auto contents = ReadTitleContent(title_id, mediatype, ContentType::EXEFS, "banner");
        if (contents.empty())
            return 0;
        *exefs_section_header_it++ = WriteSection(contents, "banner", exefs_file, exefs_header_end);
        std::cout << "done!" << std::endl;
    }

    {
        std::cout << "\tDumping icon... " << std::flush;
        auto contents = ReadTitleContent(title_id, mediatype, ContentType::EXEFS, "icon");
        if (contents.empty())
            return 0;
        *exefs_section_header_it++ = WriteSection(contents, "icon", exefs_file, exefs_header_end);
        std::cout << "done!" << std::endl;
    }

    {
        std::cout << "\tDumping logo... " << std::flush;
        auto contents = ReadTitleContent(title_id, mediatype, ContentType::EXEFS, "logo");
        if (contents.empty())
            return 0;
        *exefs_section_header_it++ = WriteSection(contents, "logo", exefs_file, exefs_header_end);
        std::cout << "done!" << std::endl;
    }

    // Seek back and write ExeFS header
    auto end_pos = exefs_file.tellp();
    exefs_file.seekp(exefs_header_begin);
    // TODO: Compute file hashes!
    exefs_file.write(reinterpret_cast<char*>(&exefs_header), sizeof(exefs_header));
    exefs_file.seekp(end_pos);

    return size_decompressed_code;
}

static Result
MYFSUSER_OpenFileDirectly(Handle fsuHandle,
                        Handle     *out,
                        FS_Archive archive,
                        FS_Path    fileLowPath,
                        u32        openFlags,
                        u32        attributes) noexcept {
    u32 *cmdbuf = getThreadCommandBuffer();

    cmdbuf[ 0] = IPC_MakeHeader(0x803,8,4); // 0x8030204
    cmdbuf[ 1] = 0;
    cmdbuf[ 2] = archive.id;
    cmdbuf[ 3] = archive.lowPath.type;
    cmdbuf[ 4] = archive.lowPath.size;
    cmdbuf[ 5] = fileLowPath.type;
    cmdbuf[ 6] = fileLowPath.size;
    cmdbuf[ 7] = openFlags;
    cmdbuf[ 8] = attributes;
    cmdbuf[ 9] = IPC_Desc_StaticBuffer(archive.lowPath.size,2);
    cmdbuf[10] = (u32)archive.lowPath.data;
    cmdbuf[11] = IPC_Desc_StaticBuffer(fileLowPath.size,0);
    cmdbuf[12] = (u32)fileLowPath.data;

    Result ret = 0;
    if((ret = svcSendSyncRequest(fsuHandle)))
        return ret;

    if(out)
        *out = cmdbuf[3];

    return cmdbuf[1];
}

static bool DumpRomFS(std::ofstream& out_file, uint64_t title_id, uint8_t mediatype) {
    bool success = false;

    // Write the magic word and some padding bytes to act as a dummy info block
    out_file.write("IVFC", 4);
    std::generate_n(std::ostream_iterator<uint8_t>(out_file), 0xFFC, []{return 0;}); // TODO: Use WriteDummyBytes instead

    // Read level 3 partition data
    char arch_path[] = "";
    FS_Path fs_archive_path = FS_Path{ PATH_EMPTY, 1, (u8*)arch_path };
    FS_Archive fs_archive = { ARCHIVE_ROMFS, fs_archive_path };
    char low_path[0xc];
    memset(low_path, 0, sizeof(low_path));

    Handle local_fs_handle;
    Result ret = srvGetServiceHandleDirect(&local_fs_handle, "fs:USER");
    if (ret != 0) {
        std::cout << "Failed to get fs:USER handle (error " << ResultToString(ret) << ")" << std::endl;
        return success;
    }


    ret = FSUSER_Initialize(local_fs_handle);
    Handle file_handle;
    if (ret != 0) {
        std::cout << "Failed to initialize fs:USER handle (error " << ResultToString(ret) << ")" << std::endl;
        goto cleanup2;
    }

    ret = MYFSUSER_OpenFileDirectly(local_fs_handle,
                                    &file_handle,
                                    fs_archive,
                                    (FS_Path) { PATH_BINARY, sizeof(low_path), (u8*)low_path },
                                    FS_OPEN_READ,
                                    0);

    if (ret != 0) {
        std::cout << "Couldn't open RomFS for reading (error " << ResultToString(ret) << ")" << std::endl;
        goto cleanup2;
    }

    {
    uint64_t size;
    uint64_t offset = 0;
    ret = FSFILE_GetSize(file_handle, &size);
    if (ret != 0 || !size) {
        std::cout << "Couldn't get RomFS size (error " << ResultToString(ret) << ")" << std::endl;
        goto cleanup;
    }

    std::vector<char> read_buffer(1024*1024);
    while (offset != size) {
        uint32_t bytes_read;

        ret = FSFILE_Read(file_handle, &bytes_read, offset, read_buffer.data(), read_buffer.size() * sizeof(read_buffer[0]));
        if (ret != 0 || bytes_read == 0) {
            std::cout << "Error while reading RomFS (error " << ResultToString(ret) << ")" << std::endl;
            goto cleanup;
        }
        out_file.write(read_buffer.data(), bytes_read);
        if (!out_file.good()) {
            std::cout << "Error while writing output... is your SD card full?" << std::endl;
            goto cleanup;
        }
        offset += bytes_read;

        std::cout << "\rDumping RomFS... " << (offset / 1024) << "/" << (size / 1024) << " KiB... " << std::flush;
    }
    success = true;

    }


cleanup:
    FSFILE_Close(file_handle);

cleanup2:
    svcCloseHandle(local_fs_handle);

    return success;
}

// Get the size of the memory region that contains the virtual address "address" (as returned by svcQueryMemory).
// We use this to estimate the size of the various application sections.
static uint32_t GetRegionSize(uint32_t address) {
    MemInfo mem_info;
    PageInfo page_info;
    Result ret = svcQueryMemory(&mem_info, &page_info, address);
    if (ret != 0) {
        return 0;
    }

    return mem_info.size;
}

// Write "num" placeholder bytes into the output stream
static void WriteDummyBytes(std::ofstream& file, unsigned num) {
    std::generate_n(std::ostream_iterator<char>(file), num, [] { return 0; });
}

// Append dummy bytes to "stream" until the offset with respect to the given "base" is a multiple of the media unit size.
static void PadToNextMediaUnit(std::ofstream& stream, std::ofstream::pos_type base) {
    auto cur_pos = stream.tellp();
    auto diff = cur_pos - base;
    WriteDummyBytes(stream, RoundUpToMediaUnit(diff) - diff);
}

// Encode four characters as a (little-endian) uint32_t value
static inline uint32_t MakeMagic(char a, char b, char c, char d) {
    return a | b << 8 | c << 16 | d << 24;
}

const bool dump_standalone_exefs = false;
const bool dump_standalone_romfs = false;
const bool dump_full_image = true;
const bool dump_fcram = false;

int main(int argc, char **argv) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    std::cout << "Hi! Welcome to braindump <3" << std::endl << std::endl;

    uint64_t title_id;
    uint8_t mediatype;
    Result ret = GetTitleInformation(&mediatype, &title_id);
    if (ret != 0) {
        // TODO: Error
        return 1;
    }
    std::cout << "Title ID: " << fixed_width_hex(title_id) << ", media type " << fixed_width_hex(mediatype) << std::endl;

    std::stringstream filename_ss;
    filename_ss << "sdmc:/" << std::hex << std::setw(16) << std::setfill('0') << title_id;
    std::cout << "Dumping to \"" << filename_ss.str() << "\"" << std::endl;

    int ret2 = mkdir(filename_ss.str().c_str(), 0755);
    if (ret2 != 0 && ret2 != EEXIST) {
        // TODO: Error
    }


    bool success = true;

    // Dump a copy of FCRAM
    if (dump_fcram) {
        std::ofstream out_file;
        out_file.open(filename_ss.str() + "/fcram.bin", std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);

        uint8_t* buf = (uint8_t*)linearAlloc(0x10000);
        for (uint8_t* src = (uint8_t*)0x14000000; src < (uint8_t*)(0x14000000 + 0x06800000); src += 0x10000) {
            GSPGPU_FlushDataCache(src, 0x10000);
            Result res = GX_TextureCopy((u32*)src, 0x0, (u32*)buf, 0x0, 0x10000, 8);
            if (res != 0)
                std::cout << "ERROR!" << std::endl;
            gspWaitForPPF();
            GSPGPU_InvalidateDataCache((void*)buf, 0x10000);
            std::cout << "\rDumping FCRAM: " << std::hex << (u32)src << " " << std::hex << (u32)buf << std::dec << std::flush;

            out_file.write((char*)buf, 0x10000);
        }
        linearFree((void*)buf);
    }

    // Dump ExeFS to its own file
    if (dump_standalone_exefs) {
        std::cout << "Dumping ExeFS... be patient!" << std::endl;
        std::ofstream out_file;
        out_file.open(filename_ss.str() + "/exefs.bin", std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
        success &= (0 != DumpExeFS(out_file, title_id, mediatype));
        std::cout << " done!" << std::endl;
    }

    // Dump RomFS to its own file
    if (dump_standalone_romfs) {
        std::cout << "Dumping RomFS..." << std::flush;
        std::ofstream out_file;
        out_file.open(filename_ss.str() + "/romfs.bin", std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
        success &= DumpRomFS(out_file, title_id, mediatype);
        std::cout << " done!" << std::endl;
    }

    // Dump a full NCCH of the current target title
    if (dump_full_image) {
        std::ofstream out_file;
        out_file.open(filename_ss.str() + ".cxi", std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);

        // Write placeholder headers to be filled later
        auto ncch_pos = out_file.tellp();
        WriteDummyBytes(out_file, sizeof(NCCH_Header));

        auto exheader_pos = out_file.tellp();
        WriteDummyBytes(out_file, sizeof(ExHeader_Header));

        PadToNextMediaUnit(out_file, ncch_pos);

        // Dump ExeFS and RomFS first (since their sizes are needed to generate the ExHeader)
        std::cout << "Dumping ExeFS... be patient!" << std::endl;
        auto exefs_pos = out_file.tellp();
        auto decompressed_code_size = DumpExeFS(out_file, title_id, mediatype);
        success &= (0 != decompressed_code_size);
        auto exefs_end = out_file.tellp();
        std::cout << " done!" << std::endl;
        PadToNextMediaUnit(out_file, ncch_pos);

        std::cout << "Dumping RomFS..." << std::flush;
        auto romfs_pos = out_file.tellp();
        success &= (0 != DumpRomFS(out_file, title_id, mediatype));
        auto romfs_end = out_file.tellp();
        std::cout << " done!" << std::endl;
        PadToNextMediaUnit(out_file, ncch_pos);

        auto ncch_end = out_file.tellp();

        // Generate a fake ExHeader:
        // There is (or rather, seems to be) no way to access the actual ExHeader,
        // so we just craft a reasonable "fake" one here based on the metadata
        // that we *can* access and some heuristics.
        //
        // TODO: The SMDH (i.e. "ExeFS/icon") provides useful information with regards to the application name and other stuff!
        // TODO: We might get the product code of the current title using AM:GetTitleProductCode
        // TODO: Other potentially useful service calls: PM_GetTitleExheaderFlags, APT:GetAppletInfo, APT:GetProgramInfo

        ExHeader_Header exheader;
        memset(&exheader, 0, sizeof(exheader));

        // Program segment information:
        // - Assume text starts at 0x00100000
        // - Assume text is followed by ro
        // - Assume ro is followed by data
        // - Assume bss size is the difference between the total size of the text/ro/data segments and the size of the decompressed .code data
        // - Assume old application stack is still queryable at 0x0FFFFFFC
        // TODO: bss size is still off by a few bytes. This could be resolved by parsing the code binary (which always (?) starts with a bl to bss_clear for official content).
        auto& codeset = exheader.codeset_info;
        const unsigned page_size = 0x1000;
        // codeset.name = TODO; // e.g. "CubicNin"
        codeset.flags.flag = 1; // bit0: CompressExefsCode
        codeset.text.address = 0x00100000;
        codeset.text.code_size = GetRegionSize(codeset.text.address);
        codeset.text.num_max_pages = RoundUpToPageSize(codeset.text.code_size) / page_size;
        codeset.ro.address = codeset.text.address + codeset.text.num_max_pages * page_size;
        codeset.ro.code_size = GetRegionSize(codeset.ro.address);
        codeset.ro.num_max_pages = RoundUpToPageSize(codeset.ro.code_size) / page_size;
        codeset.data.address = codeset.ro.address + codeset.ro.num_max_pages * page_size;

        uint32_t data_and_bss_size = GetRegionSize(codeset.data.address);
        codeset.bss_size = codeset.text.code_size + codeset.ro.code_size + data_and_bss_size - decompressed_code_size;

        codeset.data.code_size = data_and_bss_size - codeset.bss_size;
        codeset.data.num_max_pages = RoundUpToPageSize(codeset.data.code_size) / page_size;
        codeset.stack_size = GetRegionSize(0x0FFFFFFC);

        exheader.arm11_system_local_caps.program_id = title_id;

        // Initialize ARM11 kernel capabilities to "unused" by default, then fill selected array members
        auto& arm11_caps_descriptor = exheader.arm11_kernel_caps.descriptors;
        std::fill(std::begin(arm11_caps_descriptor),
                  std::end(arm11_caps_descriptor),
                  0xFFFFFFFF);

        // SVCs: Grant full access to everything \o/
        for (unsigned svc_table_index = 0; svc_table_index < 7; ++svc_table_index) {
            const uint32_t all_svcs = 0xffffff;
            arm11_caps_descriptor[svc_table_index] = (0b11110 << 27) | (svc_table_index << 24) | all_svcs;
        }

        // Write fake ExHeader to file
        out_file.seekp(exheader_pos);
        out_file.write((char*)&exheader, sizeof(exheader));


        // Generate a fake NCCH header, since
        // - we cannot get the actual NCCH header
        // - the actual NCCH header usually refers to the encrypted data anyway, while we store unencrypted data.
        NCCH_Header header;
        memset(&header, 0, sizeof(header));

        header.magic = MakeMagic('N', 'C', 'C', 'H');
        header.version = 2;
        header.program_id = title_id;

        // TODO: If possible, detect New3DS-only titles and set the proper flag here
        header.flags.content_platform = NCCHContentPlatform::Old3DS;
        header.flags.content_type = NCCHContentType::Data | NCCHContentType::Executable;
        header.flags.crypto = NCCHCrypto::NoCrypto;

        header.extended_header_size = sizeof(exheader) - sizeof(exheader.access_desc);

        header.exefs_offset = BytesToMediaUnits(exefs_pos - ncch_pos);
        header.exefs_size = BytesToMediaUnits(exefs_end - exefs_pos);

        header.romfs_offset = BytesToMediaUnits(romfs_pos - ncch_pos);
        header.romfs_size = BytesToMediaUnits(romfs_end - romfs_pos);

        header.content_size = BytesToMediaUnits(ncch_end - ncch_pos);

        // Write fake NCCH header
        out_file.seekp(ncch_pos);
        out_file.write((char*)&header, sizeof(header));
    }

    if (success)
        std::cout << std::endl << "Done! Thanks for being awesome!" << std::endl << "Press Start to exit." << std::endl;
    else
        std::cout << std::endl << "Failure during dumping. Output data is incomplete!" << std::endl;

    while (aptMainLoop())
    {
        //Scan all the inputs. This should be done once for each frame
        hidScanInput();

        //hidKeysDown returns information about which buttons have been just pressed (and they weren't in the previous frame)
        u32 kDown = hidKeysDown();

        if (kDown & KEY_START) break; // break in order to return to hbmenu

        // Flush and swap framebuffers
        gfxFlushBuffers();
        gfxSwapBuffers();

        //Wait for VBlank
        gspWaitForVBlank();
    }

    gfxExit();
}
