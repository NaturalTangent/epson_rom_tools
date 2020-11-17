/*
makerom - Andy Anderson 2020

Quick and dirty tool to build ROM images for Epson PX-8 ROM capsules (and probably PX-4, EHT-10).

Currently hard-coded for;
* 256kbit PROM (e.g. 27C256).
* M format (loaded into TPA for execution).
* Requires all files in current directory (i.e. the file-name splitting code will break if directories are specified).

To compile on linux;

    g++ makerom.cpp -o makerom

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

const uint8_t MAGIC = 0xe5;
const uint8_t MAGIC_P = 0x50; // Not supported
const uint8_t MAGIC_M = 0x37;

const uint8_t CAPACITY_64kbit = 0x08;
const uint8_t CAPACITY_128kbit = 0x10;
const uint8_t CAPACITY_256kbit = 0x20;
const uint8_t CAPACITY_512kbit = 0x40; // Not supported
const uint8_t CAPACITY_1024kbit = 0x80; // Not supported

const uint8_t MAX_DIR_ENTRIES = 0x20;

const uint8_t DIR_ENTRY_INVALID = 0xe5;
const uint8_t DIR_ENTRY_VALID = 0x00;

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

static void fatal(const char* msg, const char* param = NULL)
{
    std::cerr << msg;
    
    if(param)
    {
        std::cerr << " : " << param;
    } 
    
    std::cerr << std::endl;

    exit(-1);
}

static void usage()
{
    std::cout << "Usage: makerom <romfile> <file1> [file2 [file3 [file..x]]]\n" << std::endl;
}

bool split_file_name(const std::string& full, uint8_t name[8], uint8_t type[3])
{
    std::string::size_type pos = full.find_last_of('.');

    if(pos == std::string::npos)
    {
        fatal("Input files must be 8.3", full.c_str());
    }

    std::string sname = full.substr(0, pos);
    std::string sext = full.substr(pos+1);

    if(sname.length() < 1 || sname.length() > 8 || sext.length() < 1 || sext.length() > 3)
    {
        fatal("Input files must be 8.3", full.c_str());
    }

    memset(name, ' ', 8);
    memset(type, ' ', 3);
    memcpy(name, sname.c_str(), sname.length());
    memcpy(type, sext.c_str(), sext.length());

    return true;
}

int main(int argc, char* argv[])
{
    if(argc < 2)
    {
        usage();
        exit(-1);
    }

    std::string outName = argv[1];

    std::fstream outFile;
    outFile.open(outName);
    if(outFile)
    {
        fatal("Output file already exists.", outName.c_str());
    }

    outFile.close();


    // Initialise ROM header
    std::vector<uint8_t> directory((sizeof(DirEntry) * MAX_DIR_ENTRIES), DIR_ENTRY_INVALID);
    DirEntry* dirBase = (DirEntry*)directory.data();
    RomHeader* hdr = (RomHeader*)directory.data(); // DirEntry 0 is used as the ROM header
    hdr->id[0] = MAGIC;
    hdr->id[1] = MAGIC_M;
    hdr->capacity = CAPACITY_256kbit; // 27256 (32KB)
    memcpy(hdr->system_name, "H80", 3);
    memset(hdr->rom_name, ' ', sizeof(hdr->rom_name));
    memcpy(hdr->rom_name, outName.c_str(), outName.length() > sizeof(hdr->rom_name) ? sizeof(hdr->rom_name) : outName.length());
    hdr->dir_entries = 4;
    hdr->v = 'V';
    hdr->version[0] = '1';
    hdr->version[1] = '0';
    memcpy(hdr->month, "11", 2);
    memcpy(hdr->day, "16", 2);
    memcpy(hdr->year, "20", 2);

    // Invalidate all dir entries
    for(int i=1; i<MAX_DIR_ENTRIES; ++i)
    {
        dirBase[i].validity = DIR_ENTRY_INVALID;
    }

    std::vector<uint8_t> file_area;
    uint8_t currentDirectory = 0;
    uint8_t nextAllocation = 1;

    // Process each file
    for(int iFile=2; iFile<argc; ++iFile)
    {
        if(++currentDirectory > MAX_DIR_ENTRIES-1)
        {
            fatal("Out of directory space.");
        }

        // open file
        std::ifstream inFile(argv[iFile], std::ios::in | std::ios::binary);
        if(!inFile)
        {
            fatal("failed to open input file.", argv[iFile]);
        }

        uint8_t name[8];
        uint8_t type[3];
        split_file_name(argv[iFile], name, type);

        // read into buffer
        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());

        // Calculate number of 1K chunks
        size_t chunks = (buffer.size() + 1023) / 1024;

        // pad file to 1KB boundary
        if(buffer.size() < (chunks * 1024))
        {
            size_t diff = (chunks*1024) - buffer.size();
            buffer.resize(buffer.size() + diff);
            memset(&buffer[buffer.size()-diff], (int)diff, 0);
        }

        // Make space for the file
        int allocationIndex = 0;
        int nextLogicalExtent = 0;
        int file_area_offset = (int)file_area.size();
        int buffer_offset = 0;
        file_area.resize(file_area.size() + (chunks*1024));

        // Reserve a directory entry
        memset(&dirBase[currentDirectory], 0, sizeof(DirEntry));
        memcpy(&dirBase[currentDirectory].file_name, name, 8);
        memcpy(&dirBase[currentDirectory].file_type, type, 3);
        dirBase[currentDirectory].logical_extent = nextLogicalExtent++;

        // Append to file area in 1K chunks
        for(size_t iChunk=0; iChunk<chunks; ++iChunk)
        {
            if(allocationIndex >= 16)
            {
                // Need to extend into next directory entry
                if(++currentDirectory > MAX_DIR_ENTRIES-1)
                {
                    fatal("Out of directory space.");
                }

                memset(&dirBase[currentDirectory], 0, sizeof(DirEntry));
                allocationIndex = 0;
                memcpy(&dirBase[currentDirectory].file_name, name, 8);
                memcpy(&dirBase[currentDirectory].file_type, type, 3);
                dirBase[currentDirectory].logical_extent = nextLogicalExtent++;
            }

            dirBase[currentDirectory].record_count += (1024/128);
            dirBase[currentDirectory].allocation_map[allocationIndex++] = nextAllocation++;

            memcpy(&file_area[file_area_offset], &buffer[buffer_offset], 1024);
            buffer_offset += 1024;
            file_area_offset += 1024;
        }

    }

    // Update the header to reflect the files that have been stored
    hdr->dir_entries = ((currentDirectory + 3) / 4) * 4;
    uint16_t checksum = (uint16_t)file_area.size();
    hdr->checksum[0] = checksum & 0xff;
    hdr->checksum[1] = (checksum >> 8) & 0xff;

    uint16_t romSize = 0x8000; // TODO - base this on hdr->capacity

    if(((hdr->dir_entries * sizeof(DirEntry)) + file_area.size()) > romSize)
    {
        fatal("Out of ROM space.");
    }

    std::vector<uint8_t> rom(romSize, 0xff);
    memcpy(rom.data(), dirBase, hdr->dir_entries * sizeof(DirEntry));
    memcpy(rom.data() + (hdr->dir_entries * sizeof(DirEntry)), file_area.data(), file_area.size()); 

    // 27256 ROMs require to convert physical to logical addresses
    if(hdr->capacity == CAPACITY_256kbit)
    {
        std::vector<uint8_t> temp = rom;
        memcpy(&rom[0], &temp[0x4000], 0x4000);
        memcpy(&rom[0x4000], &temp[0], 0x4000);
    }

    // Write the ROM to disk
    outFile.open(outName, std::ios::out | std::ios::binary);
    if(!outFile)
    {
        fatal("Failed to open output file for writing.", outName.c_str());
    }

    outFile.write((char*)rom.data(), rom.size());

    if(!outFile.good())
    {
        fatal("Failed to write to ouput file.", outName.c_str());
    }

    outFile.close();

    return 0;
}