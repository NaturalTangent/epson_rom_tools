/*
dumprom - Andy Anderson 2020

Quick and dirty tool to extract files from Epson PX-8 ROM capsules (and probably PX-4, EHT-10).

Currently hard-coded for;
* M format (loaded into TPA for execution).
* Requires all files in current directory (i.e. the file-name splitting code will break if directories are specified).

To compile on linux;

    g++ dumprom.cpp -o dumprom

Reference documentation;
* PX-8 OS Reference Manual - chapter 15
* EHT-10 Development Tool User's Guide - Appendix 1

*/

#include <cstdint>
#include <cassert>
#include <string>
#include <cstring>
#include <vector>
#include <fstream>
#include <iostream>
#include <streambuf>

#ifdef _MSC_VER
#define PACK_PRE __pragma (pack( push, 1))
#define PACK_POST __pragma (pack( pop ))
#define PACK_ATTRIBUTE
#else
#define PACK_PRE
#define PACK_POST
#define PACK_ATTRIBUTE __attribute__((packed))
#endif

PACK_PRE
struct RomHeader
{
    uint8_t id[2]; // 0xE5, (0x37=M format, 0x50=P format)
    uint8_t capacity; // 0x08=64kbits, 0x10=128kbits, 0x20=256kbits, 0x40=512kbits, 0x80=1mbits
    uint8_t checksum[2];
    uint8_t system_name[3];
    uint8_t rom_name[14];
    uint8_t dir_entries; // number of entries + 1 (then rounded up to a multiple of 4)
    uint8_t v;
    uint8_t version[2];
    uint8_t month[2];
    uint8_t day[2];
    uint8_t year[2];
} PACK_ATTRIBUTE;

struct DirEntry
{
    uint8_t validity; // 0x00=valid 0xE5=invalid
    uint8_t file_name[8];
    uint8_t file_type[3];
    uint8_t logical_extent;
    uint16_t zero;
    uint8_t record_count; // 0 to 128. number of 128 byte records controlled by the dir entry
    uint8_t allocation_map[16]; // The IDs of each 1K block used by the file
} PACK_ATTRIBUTE;
PACK_POST


static uint8_t* file_area_offset(const RomHeader* const hdr)
{
    assert(hdr->dir_entries%4 == 0);
    assert(hdr->dir_entries >= 0 && hdr->dir_entries <=0x20);

    return ((uint8_t*)hdr) + (hdr->dir_entries * sizeof(DirEntry));
}

static DirEntry* dir_entry_offset(const RomHeader* const hdr, const uint8_t dir_entry)
{
    assert(dir_entry >= 1);
    assert(dir_entry <= hdr->dir_entries);

    return (DirEntry*)(((uint8_t*)hdr) + dir_entry * sizeof(DirEntry));
}

static uint8_t* block_address(uint8_t* fileBase, const uint8_t blockNo)
{
    assert(blockNo >=1);

    return fileBase + ((blockNo-1) * 1024);
}

static void fatal(const char* msg)
{
    std::cerr << msg << std::endl;
    exit(-1);
}

static std::string trim(const std::string& s)
{
    std::string::size_type pos = s.find_first_of(' ');

    if(pos == std::string::npos)
    {
        return s;
    }

    return s.substr(0, pos);
}

static void dump_files(const uint8_t* romBase, const uint32_t romSize)
{
    const RomHeader* header = (RomHeader*)romBase;

    if((header->id[0] != 0xE5) && (header->id[1] != 0x37))
    {
        fatal("Not a valid rom file.");
    }


    uint8_t* fileBase = file_area_offset(header);

    std::ofstream outFile;

    // Enumerate files
    uint8_t dirNo = 1;
    std::string fileName;
    std::string extension;
    uint8_t extentNo = 0;

    while(dirNo <= header->dir_entries)
    {
        DirEntry* dir = dir_entry_offset(header, dirNo);

        if(dir->validity == 0x00)
        {
            if(dir->logical_extent == 0)
            {
                // close current file
                outFile.close();

                extentNo = 0;
                fileName = std::string(dir->file_name, dir->file_name+sizeof(DirEntry::file_name));
                extension = std::string(dir->file_type, dir->file_type+sizeof(DirEntry::file_type));

                fileName = trim(fileName);
                extension = trim(extension);

                // open new file
                outFile.open(fileName + "." + extension, std::ios::out | std::ios::binary);
                if(!outFile) fatal("Could not open output file.");

                // Write each block in the allocation map
                for(uint8_t i=0; i<16; ++i)
                {
                    if(dir->allocation_map[i])
                    {
                        outFile.write((char*)block_address(fileBase, dir->allocation_map[i]), 1024);
                    }
                }

            }
            else
            {
                // TODO - Warning if file name is different

                // TODO - Warning if logical_extent != extentNo+1

                extentNo = dir->logical_extent;

                // Write each block in the allocation map
                for(uint8_t i=0; i<16; ++i)
                {
                    if(dir->allocation_map[i])
                    {
                        outFile.write((char*)block_address(fileBase, dir->allocation_map[i]), 1024);
                    }
                }
            }
        }

        ++dirNo;
    }

    // Close file
    outFile.close();

}

static void usage()
{
    std::cout << "Usage: dumprom <romfile>\n" << std::endl;
}

int main(int argc, char* argv[])
{
    assert(sizeof(RomHeader) == 32);
    assert(sizeof(DirEntry) == 32);

    if(argc != 2)
    {
        usage();
        exit(-1);
    }

    std::string fileName = argv[1];

    std::ifstream inFile(fileName, std::ios::in | std::ios::binary);

    if(!inFile)
    {
        fatal("failed to open input file.");
    }

    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(inFile)),
                 std::istreambuf_iterator<char>());

    if(buffer.size() == 0x8000)
    {
        // convert physical to logical addresses
        std::vector<uint8_t> temp = buffer;
        memcpy(&buffer[0], &temp[0x4000], 0x4000);
        memcpy(&buffer[0x4000], &temp[0], 0x4000);
    }

    dump_files(buffer.data(), (uint32_t)buffer.size());

    return 0;
}