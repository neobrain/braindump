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

#include <3ds.h>

#include "ncch.h"

static Result GetTitleInformation(u8* mediatype, uint64_t* tid)
{
    Result ret = 0;

    if(mediatype)
    {
        Handle localFsHandle;

        // Create temporary FS handle to get the proper media type
        ret = srvGetServiceHandle(&localFsHandle, "fs:USER");
        if(ret)return ret;

        ret = FSUSER_Initialize(localFsHandle);
        if(ret)return ret;

        ret = FSUSER_GetMediaType(mediatype);

        svcCloseHandle(localFsHandle);
    }

    if(tid)
    {
        aptOpenSession();
        ret = APT_GetProgramID(tid);
        aptCloseSession();
    }

    return ret;
}

class WriteResult {
    Result res;

public:
    WriteResult(Result res) : res(res) {}
friend std::ostream& operator<<(std::ostream& os, const WriteResult& result);
std::ostream& operator<<(std::ostream& os) {
    std::ios::fmtflags f( os.flags() );
    os << std::hex << std::showbase << std::setw(10) << std::setfill('0') << res;
    os.flags(f);
    return os;
}
};

std::ostream& operator<<(std::ostream& os, const WriteResult& result) {
    std::ios::fmtflags f( os.flags() );
    os << std::hex << std::showbase << std::setw(10) << std::setfill('0') << result.res;
    os.flags(f);
    return os;
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
    Result ret = FSUSER_OpenFileDirectly(&file_handle,
                                         fs_archive,
                                         (FS_path) { PATH_BINARY, sizeof(data), (u8*)&data },
                                         FS_OPEN_READ,
                                         FS_ATTRIBUTE_NONE);
    if (ret != 0) {
        std::cout << "Couldn't open \"ExeFS/" + name + "\" for reading (error " << WriteResult(ret) << ")" << std::endl;
        return {};
    }

    uint64_t size;
    ret = FSFILE_GetSize(file_handle, &size);
    if (ret != 0 || !size) {
        FSFILE_Close(file_handle);
//        throw std::runtime_error("Couldn't get file size of \"ExeFS/" + name + "\" (error " + ResultToString(ret) + ")");
        return {};
    }

    std::vector<uint8_t> content(size);

    uint32_t bytes_read;
    ret = FSFILE_Read(file_handle, &bytes_read, 0, content.data(), size);
    if (ret != 0 || bytes_read != size) {
        FSFILE_Close(file_handle);
//        throw std::runtime_error("Couldn't read \"ExeFS/" + name + "\" (error " + ResultToString(ret) + ")");
        return {};
    } else {
    }

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
        std::cout << "\tDumping code.bin... " << std::flush;
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
        std::cout << "\tDumping banner.bin... " << std::flush;
        auto contents = ReadTitleContent(title_id, mediatype, ContentType::EXEFS, "banner");
        if (contents.empty())
            return 0;
        *exefs_section_header_it++ = WriteSection(contents, "banner", exefs_file, exefs_header_end);
        std::cout << "done!" << std::endl;
    }

    {
        std::cout << "\tDumping icon.bin..." << std::flush;
        auto contents = ReadTitleContent(title_id, mediatype, ContentType::EXEFS, "icon");
        if (contents.empty())
            return 0;
        *exefs_section_header_it++ = WriteSection(contents, "icon", exefs_file, exefs_header_end);
        std::cout << "done!" << std::endl;
    }

    {
        std::cout << "\tDumping logo.bin... " << std::flush;
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

int main() {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    uint64_t title_id;
    uint8_t mediatype;
    GetTitleInformation(&mediatype, &title_id);
    std::cout << "Title ID: " << std::hex << std::setw(18) << std::setfill('0') << std::showbase << title_id << std::endl;

    std::stringstream filename_ss;
    filename_ss << "sdmc:/" << std::hex << std::setw(16) << std::setfill('0') << title_id << "/";
    std::cout << "Dumping to \"" << filename_ss.str() << "\"" << std::endl;

    bool success = 0;

    {
        std::cout << "Dumping ExeFS..." << std::endl;
        std::ofstream out_file;
        out_file.open(filename_ss.str() + "exefs.bin", std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
        success = (0 != DumpExeFS(out_file, title_id, mediatype));
    }

    if (success)
        std::cout << "Done! Press Start to exit." << std::endl;
    else
        std::cout << "Failure during dumping. Output data is incomplete!" << std::endl;

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
