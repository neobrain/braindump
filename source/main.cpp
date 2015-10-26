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

class WriteResult {
    Result res;

public:
    WriteResult(Result res) : res(res) {}
	friend std::ostream& operator<<(std::ostream& os, const WriteResult& result);
};

std::ostream& operator<<(std::ostream& os, const WriteResult& result) {
    std::ios::fmtflags f( os.flags() );
    os << std::hex << std::showbase << std::setw(10) << std::setfill('0') << result.res;
    os.flags(f);
    return os;
}

std::string ResultToString(Result res) {
    std::stringstream ss;
    ss << WriteResult(res);
    return ss.str();
}

static Result
MYFSUSER_GetMediaType(Handle fsuHandle, u8* mediatype)
{
    u32* cmdbuf = getThreadCommandBuffer();

    cmdbuf[0] = IPC_MakeHeader(0x868,0,0); // 0x8680000

    Result ret = 0;
    if((ret = svcSendSyncRequest(fsuHandle)))
        return ret;

    if(mediatype)
        *mediatype = cmdbuf[2];

    return cmdbuf[1];
}

static Result GetTitleInformation(u8* mediatype, uint64_t* tid)
{
    Result ret = 0;

    if(mediatype)
    {
        Handle localFsHandle;

        // Create temporary FS handle to get the proper media type
        ret = srvGetServiceHandleDirect(&localFsHandle, "fs:USER");
        if(ret)return ret;

        ret = FSUSER_Initialize(localFsHandle);
        if(ret)return ret;

        ret = MYFSUSER_GetMediaType(localFsHandle, mediatype);
        if (ret != 0)
            printf("FSUSER_GetMediaType error: %x\n", ResultToString(ret).c_str());

        svcCloseHandle(localFsHandle);
    }

    if(tid)
    {
        aptOpenSession();
        ret = APT_GetProgramID(tid);
        if (ret != 0)
            printf("APT_GetProgramID error: %x\n", ResultToString(ret).c_str());
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
    FS_path fs_archive_path = { PATH_BINARY, 0x10, (u8*)archivePath };
    FS_archive fs_archive = { 0x2345678a, fs_archive_path };

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
                                         (FS_path) { PATH_BINARY, sizeof(data), (u8*)&data },
                                         FS_OPEN_READ,
                                         FS_ATTRIBUTE_NONE);

    if (ret != 0) {
        printf("Couldn't open \"ExeFS/%s\" for reading (error %s)\n", name.c_str(), ResultToString(ret).c_str());
        return {};
    }

    uint64_t size;
    ret = FSFILE_GetSize(file_handle, &size);
    if (ret != 0 || !size) {
        printf("Couldn't get file size for \"ExeFS/%s\" (error %s)\n", name.c_str(), ResultToString(ret).c_str());
        FSFILE_Close(file_handle);
        return {};
    } else {
        printf("%" PRIu64 " KiB... ", size / 1024);
        fflush(stdout);
    }

    std::vector<uint8_t> content(size);

    uint32_t bytes_read;
    ret = FSFILE_Read(file_handle, &bytes_read, 0, content.data(), size);
    if (ret != 0 || bytes_read != size) {
        printf("Expected to read %" PRIu64 " bytes, read %" PRIu32 "\n", size, bytes_read);
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

template<typename Container>
static ExeFs_SectionHeader WriteSection(const Container& cont, const std::string& section_name, std::ofstream& exefs_file, std::ofstream::pos_type exefs_header_end) {
    // Write section data to file
    const auto section_begin = exefs_file.tellp();
    std::copy(cont.begin(), cont.end(), std::ostream_iterator<uint8_t>(exefs_file));
    const auto section_end = exefs_file.tellp();

    // Pad with zeros to media unit size
    uint32_t size = section_end - section_begin;
    uint32_t padding = RoundUpToMediaUnit(size) - size;
    std::generate_n(std::ostream_iterator<char>(exefs_file), padding, [] { return 0; });

    // Build and return header - don't include padding in the reported size
    ExeFs_SectionHeader ret{{}, (uint32_t)(section_begin - exefs_header_end), size };
    std::strcpy(ret.name, section_name.c_str());
    return ret;
}

// Returns the size of the decompressed .code section
static uint32_t DumpExeFS(std::ofstream& exefs_file, uint64_t title_id, uint8_t mediatype) {
    // Generate dummy ExeFS header to fill in later
    const auto exefs_header_begin = exefs_file.tellp();
    std::generate_n(std::ostream_iterator<uint8_t>(exefs_file), sizeof(ExeFs_Header), []{return 0;});
    const auto exefs_header_end = exefs_file.tellp();

    // Write content sections
    ExeFs_Header exefs_header;
    auto exefs_section_header_it = std::begin(exefs_header.section);

    uint32_t size_decompressed_code;

    {
        printf("\tDumping code... ");
        fflush(stdout);
        auto contents = ReadTitleContent(title_id, mediatype, ContentType::EXEFS, ".code");
        if (contents.empty())
            return 0;
        *exefs_section_header_it++ = WriteSection(contents, ".code", exefs_file, exefs_header_end);
        printf("done!\n");

        u8* size_diff_ptr = &contents[contents.size() - 4];
        u32 size_diff = size_diff_ptr[0] | (size_diff_ptr[1] << 8) | (size_diff_ptr[2] << 16) | (size_diff_ptr[3] << 24);
        size_decompressed_code = contents.size() + size_diff;
    }

    {
        printf("\tDumping banner... ");
        fflush(stdout);
        auto contents = ReadTitleContent(title_id, mediatype, ContentType::EXEFS, "banner");
        if (contents.empty())
            return 0;
        *exefs_section_header_it++ = WriteSection(contents, "banner", exefs_file, exefs_header_end);
        printf("done!\n");
    }

    {
        printf("\tDumping icon... ");
        fflush(stdout);
        auto contents = ReadTitleContent(title_id, mediatype, ContentType::EXEFS, "icon");
        if (contents.empty())
            return 0;
        *exefs_section_header_it++ = WriteSection(contents, "icon", exefs_file, exefs_header_end);
        printf("done!\n");
    }

    {
        printf("\tDumping logo... ");
        fflush(stdout);
        auto contents = ReadTitleContent(title_id, mediatype, ContentType::EXEFS, "logo");
        if (contents.empty())
            return 0;
        *exefs_section_header_it++ = WriteSection(contents, "logo", exefs_file, exefs_header_end);
        printf("done!\n");
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
                        FS_archive archive,
                        FS_path    fileLowPath,
                        u32        openFlags,
                        u32        attributes) noexcept
{
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
    std::generate_n(std::ostream_iterator<uint8_t>(out_file), 0xFFC, []{return 0;});

    char arch_path[] = "";
    FS_path fs_archive_path = FS_path{ PATH_EMPTY, 1, (u8*)arch_path };
    FS_archive fs_archive = { ARCH_ROMFS, fs_archive_path };
    char low_path[0xc];
    memset(low_path, 0, sizeof(low_path));

    Handle local_fs_handle;
    Result ret = srvGetServiceHandleDirect(&local_fs_handle, "fs:USER");
    if (ret != 0) {
        printf("Failed to get fs:USER handle (error %s)\n", ResultToString(ret).c_str());
        return success;
    }


    ret = FSUSER_Initialize(local_fs_handle);
    Handle file_handle;
    if (ret != 0) {
        printf("Failed to initialize fs:USER handle (error %s)\n", ResultToString(ret).c_str());
        goto cleanup2;
    }

    ret = MYFSUSER_OpenFileDirectly(local_fs_handle,
                                    &file_handle,
                                    fs_archive,
                                    (FS_path) { PATH_BINARY, sizeof(low_path), (u8*)low_path },
                                    FS_OPEN_READ,
                                    FS_ATTRIBUTE_NONE);

    if (ret != 0) {
        printf("Couldn't open RomFS for reading (error %s)\n", ResultToString(ret).c_str());
        goto cleanup2;
    }

    {
    uint64_t size;
    uint64_t offset = 0;
    ret = FSFILE_GetSize(file_handle, &size);
    if (ret != 0 || !size) {
        printf("Couldn't get RomFS size (error %s)\n", ResultToString(ret).c_str());
        goto cleanup;
    }

    while (offset != size) {
        static std::array<char, 1024> read_buffer;
        uint32_t bytes_read;

        ret = FSFILE_Read(file_handle, &bytes_read, offset, read_buffer.data(), sizeof(read_buffer));
        if (ret != 0 || bytes_read == 0) {
            printf("Error while reading RomFS (error %s)\n", ResultToString(ret).c_str());
            goto cleanup;
        }
        out_file.write(read_buffer.data(), bytes_read);
        if (!out_file.good()) {
            printf("Error while writing output... is your SD card full?\n");
            goto cleanup;
        }
        offset += bytes_read;

        printf("\rDumping RomFS... %" PRIu64 "/%" PRIu64 " KiB... ", offset / 1024, size / 1024);
        fflush(stdout);
    }
    success = true;

    }


cleanup:
    FSFILE_Close(file_handle);

cleanup2:
    svcCloseHandle(local_fs_handle);

    return success;
}

int main(int argc, char **argv) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    printf("Hi! Welcome to braindump <3\n\n");

    uint64_t title_id;
    uint8_t mediatype;
    GetTitleInformation(&mediatype, &title_id);
    printf("Title ID: 0x%016" PRIx64 ", media type %" PRIx8 "\n", title_id, mediatype);

    std::stringstream filename_ss;
    filename_ss << "sdmc:/" << std::hex << std::setw(16) << std::setfill('0') << title_id << "/";
    printf("Dumping to \"%s\"\n", filename_ss.str().c_str());

    int ret2 = mkdir(filename_ss.str().c_str(), 0755);
    if (ret2 != 0 && ret2 != EEXIST) {
        // TODO: Error
    }


    bool success = true;

    {
        printf("Dumping ExeFS...\n");
        std::ofstream out_file;
        out_file.open(filename_ss.str() + "exefs.bin", std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
        success &= (0 != DumpExeFS(out_file, title_id, mediatype));
    }

    {
        printf("Dumping RomFS...");
        fflush(stdout);
        std::ofstream out_file;
        out_file.open(filename_ss.str() + "romfs.bin", std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
        success &= DumpRomFS(out_file, title_id, mediatype);
        printf(" done!\n");
    }

    if (success)
        printf("\nDone! Thanks for being awesome!\nPress Start to exit.\n");
    else
        printf("\nFailure during dumping. Output data is incomplete!\n");

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
