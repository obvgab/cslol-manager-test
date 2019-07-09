#include "wxy.h"
#include <map>
#include "xxhash.h"
#include "zlib.h"

void decompressStr(std::string& str) {
    str.insert(str.begin(), 'x');
    char buffer[1024];
    z_stream strm = {};
    if (inflateInit2_(
                &strm,
                15,
                ZLIB_VERSION,
                static_cast<int>(sizeof(z_stream))) != Z_OK) {
        throw File::Error("Failed to init uncompress stream!");
    }
    strm.next_in = reinterpret_cast<Bytef*>(&str[0]);
    strm.avail_in = static_cast<unsigned int>(str.size());
    strm.next_out = reinterpret_cast<Bytef*>(&buffer[0]);
    strm.avail_out = static_cast<unsigned int>(sizeof(buffer));
    inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    str.resize(strm.total_out);
    std::copy(buffer, buffer + strm.total_out, &str[0]);
}

static const char prefixDeploy[] = "deploy/";
static const char prefixProjects[] = "projects/";

static void decryptStr(std::string& str) {
    int32_t i = 0;
    int32_t nameSize = static_cast<int32_t>(str.size());
    for(auto& c : str) {
        auto& b = reinterpret_cast<uint8_t&>(c);
        b = b ^ static_cast<uint8_t>(i * 17 + (nameSize - i) * 42);
        i++;
    }
}

static uint64_t xxhashStr(std::string& str, bool isProject = false) {
    XXH64_state_s xxstate{};
    XXH64_reset(&xxstate, 0);
    if(isProject) {
        XXH64_update(&xxstate, prefixProjects, sizeof(prefixProjects) - 1);
    } else {
        XXH64_update(&xxstate, prefixDeploy, sizeof(prefixDeploy) - 1);
    }
    std::string lowstr;
    lowstr.resize(str.size());
    std::transform(str.begin(), str.end(), lowstr.begin(), ::tolower);
    XXH64_update(&xxstate, lowstr.c_str(), lowstr.size());

    return XXH64_digest(&xxstate);
}


void read(const File &file, WxySkin &wxy) {
    std::array<char, 4> magic;
    file.read(magic);
    if(magic != std::array{'W', 'X', 'Y', 'S'}) {
        throw File::Error("Not a .wxy file!");
    }
    file.read(wxy.wooxyFileVersion);
    if(wxy.wooxyFileVersion < 6) {
        return read_old(file, wxy);
    } else {
        return read_oink(file, wxy);
    }
}


void read_old(const File &file, WxySkin &wxy) {
    file.read_sized_string(wxy.skinName);
    file.read_sized_string(wxy.skinAuthor);
    file.read_sized_string(wxy.skinVersion);

    if(wxy.wooxyFileVersion >= 3)
    {
        file.read_sized_string(wxy.category);
        file.read_sized_string(wxy.subCategory);
        int32_t imageCount = 0;
        file.read(imageCount);
        for(int32_t i = 1; i <= imageCount; i++) {
            int32_t imageSize = 0;
            file.read(imageSize);
            file.seek_cur(imageSize + 1);
        }
    }

    if(wxy.wooxyFileVersion < 3) {
        int32_t imageSize = 0;
        file.read(imageSize);
        file.seek_cur(imageSize + 1);
    }


    std::vector<std::string> projectsList;

    int32_t projectCount;
    file.read(projectCount);
    for(int32_t i = 0; i < projectCount; i++) {
        auto& str = projectsList.emplace_back();
        file.read_sized_string(str);
        decompressStr(str);
    }

    if(wxy.wooxyFileVersion >= 4) {
        int32_t deleteCount;
        file.read(deleteCount);
        for(int32_t i = 0; i < deleteCount; i++) {
            int32_t projectIndex;
            file.read(projectIndex);
            if(projectIndex >= projectCount || projectIndex < 0) {
                throw File::Error("delete file doesn't reference valid projcet!");
            }
            auto& skn = wxy.deleteList.emplace_back();
            skn.project =  projectsList[static_cast<size_t>(i)];;
            file.read_sized_string(skn.fileGamePath);
            decompressStr(skn.fileGamePath);
        }
    }

    int32_t fileCount;
    file.read(fileCount);
    for(int32_t i = 0; i < fileCount; i++) {
        int32_t projectIndex;
        file.read(projectIndex);
        if(projectIndex >= projectCount || projectIndex < 0) {
            throw File::Error("delete file doesn't reference valid projcet!");
        }
        auto& skn = wxy.filesList.emplace_back();
        skn.project = projectsList[static_cast<size_t>(projectIndex)];
        file.read_sized_string(skn.fileGamePath);
        file.read(skn.uncompresedSize);
        file.read(skn.compressedSize);
        if(wxy.wooxyFileVersion != 1) {
            file.read(skn.checksum);
        }
        skn.compressionMethod = skn.compressedSize == skn.uncompresedSize ? 0 : 2;
        decompressStr(skn.fileGamePath);
    }

    int32_t offset = file.tell();
    for(int32_t i = 0; i < fileCount; i++) {
        auto& skn = wxy.filesList[static_cast<size_t>(i)];
        skn.offset = offset;
        offset += skn.compressedSize;
        if(wxy.wooxyFileVersion <= 4) {
            offset -= 1;
        } else {
            offset -= 2;
        }
    }
}

void read_oink(const File &file, WxySkin &wxy)
{
    std::array<char, 4> magic;
    file.read(magic);
    if(magic != std::array{'O', 'I', 'N', 'K'}) {
        throw File::Error("Not a .wxy file!");
    }

    struct {
        uint16_t name;
        uint16_t author;
        uint16_t version;
        uint16_t category;
        uint16_t subCategory;
    } cLens; // compressed lengths
    file.rread(&cLens, 1);
    file.seek_cur(cLens.name + cLens.author + cLens.version + cLens.category + cLens.subCategory);

    int32_t previewContentCount;
    file.read(previewContentCount);
    for(int32_t i = 0; i < previewContentCount; i++) {
        uint8_t type;
        file.read(type);
        if(type == 0) {
            int32_t size = 0;
            file.read(size);
            file.seek_cur(size);
        }
    }

    uint16_t contentCount;
    file.read(contentCount);

    std::vector<std::string> projectsList;
    for(int32_t i = 0; i < contentCount; i++) {
        uint16_t pLength;
        file.read(pLength);
        auto& p = projectsList.emplace_back();
        p.resize(pLength);
        file.read(p);
    }

    for(int32_t c = 0; c < contentCount; c++) {
        int32_t fileCount;
        file.read(fileCount);

        std::vector<std::pair<uint64_t, std::string>> fileNames;

        for(int32_t f = 0; f < fileCount; f++) {
            uint16_t nameLength;
            auto& kvp = fileNames.emplace_back();
            file.read(nameLength);
            kvp.second .resize(nameLength);
            file.read(kvp.second);
            if(wxy.wooxyFileVersion > 6) {
                decryptStr(kvp.second);
            }
            kvp.first = xxhashStr(kvp.second);
        }

        std::sort(fileNames.begin(), fileNames.end(), [](auto const& l, auto const& r){
            return l.first < r.first;
        });

        for(int32_t f = 0; f < fileCount; f++) {
            auto& skn = wxy.filesList.emplace_back();
            file.read(skn.checksum2);
            file.read(skn.checksum);
            file.read(skn.adler);
            file.read(skn.uncompresedSize);
            file.read(skn.compressedSize);
            file.read(skn.compressionMethod);

            if(wxy.wooxyFileVersion > 6) {
                skn.adler = skn.adler
                        ^ static_cast<uint32_t>(skn.uncompresedSize)
                        ^ static_cast<uint32_t>(skn.compressedSize)
                        ^ 0x2Au;
            }

            skn.project = projectsList[static_cast<size_t>(c)];
            skn.fileGamePath = fileNames[static_cast<size_t>(f)].second;
        }
    }
    int32_t offset = file.tell();
    for(auto& skn: wxy.filesList) {
        skn.offset = offset;
        offset += skn.compressedSize;
    }
}
