#define _CRT_SECURE_NO_WARNINGS
#include "modoverlay.hpp"
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <span>

using namespace LCS;


namespace {
    inline constexpr uint16_t Any = 0x0100u;

    // capture offset of given byte
    template <uint16_t C = Any>
    inline constexpr uint16_t Cap = C | 0x0200u;

    // returns function that searches for the given signature
    // result of search is array with first offset + any offsets captured
    template <uint16_t... ops>
    inline constexpr auto Pattern() {
        return [](std::span<const char> data) constexpr noexcept {
            std::array<char const *, ((ops & 0x0200 ? 1 : 0) + ... + 1)> result = {};
            if (data.size() >= sizeof...(ops)) {
                auto const end = data.data() + (data.size() - sizeof...(ops));
                for (auto *i = data.data(); i != end; i++) {
                    auto c = i;
                    if ((((uint8_t)*c++ == (ops & 0xFF) || (ops & 0x100)) && ...)) {
                        auto *o = i;
                        size_t r = 0;
                        result[r++] = o;
                        ((ops & 0x200 ? result[r++] = o++ : o++), ...);
                        return result;
                    }
                }
            }
            return result;
        };
    }

    constexpr auto FindOpen = Pattern<
            0x6A, 0x03,
            0x68, 0x00, 0x00, 0x00, 0xC0,
            0x68, Any, Any, Any, Any,
            0xFF, 0x15, Cap<Any>, Any, Any, Any
            >();

    constexpr auto FindRet = Pattern<
            0x57,
            0x8B, 0xCB,
            0xE8, Any, Any, Any, Any,
            Cap<0x84>, 0xC0,
            0x75, 0x12
            >();

    constexpr auto FindWOpen = Pattern<
            0x6A, 0x00,
            0x6A, Any,
            0x68, Any, Any, 0x12, 0x00,
            Any,
            0xFF, 0x15, Cap<Any>, Any, Any, Any
            >();

    constexpr auto FindFree = Pattern<
            0xA1, Cap<Any>, Any, Any, Any,
            0x85, 0xC0,
            0x74, 0x09,
            0x3D, Any, Any, Any, Any,
            0x74, 0x02,
            0xFF, 0xE0,
            Cap<0xFF>, 0x74, 0x24, 0x04,
            0xE8, Any, Any, Any, Any
            >();

    inline PtrStorage bytes2ptr(char const* bytes) {
        PtrStorage result;
        memcpy(&result, bytes, sizeof(result));
        return result;
    }
}

void ModOverlay::save(std::filesystem::path const& filename) const noexcept {
    FILE* file = {};
#ifdef WIN32
    _wfopen_s(&file, filename.c_str(),  L"wb");
#else
    file = fopen(filename.c_str(), "wb");
#endif
    if (file) {
        auto str = to_string();
        fwrite(str.data(), 1, str.size(), file);
        fclose(file);
    }
}

void ModOverlay::load(std::filesystem::path const& filename) noexcept {
    FILE* file = {};
#ifdef WIN32
    _wfopen_s(&file, filename.c_str(),  L"rb");
#else
    file = fopen(filename.c_str(), "rb");
#endif
    if (file) {
        auto buffer = std::string();
        fseek(file, 0, SEEK_END);
        auto end = ftell(file);
        fseek(file, 0, SEEK_SET);
        buffer.resize((size_t)end);
        fread(buffer.data(), 1, buffer.size(), file);
        from_string(buffer);
    }
}

char const ModOverlay::SCHEMA[] = "lcs-overlay v5 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X";
char const ModOverlay::INFO[] = "lcs-overlay v5 checksum off_open off_open_ref off_wopen off_ret off_free_ptr off_free_fn";

std::string ModOverlay::to_string() const noexcept {
    char buffer[sizeof(SCHEMA) * 4] = {};
    int size = sprintf(buffer, SCHEMA, checksum, off_open, off_open_ref, off_wopen, off_ret, off_free_ptr, off_free_fn);
    return std::string(buffer, (size_t)size);
}

void ModOverlay::from_string(std::string const &buffer) noexcept {
    if (sscanf(buffer.c_str(), SCHEMA, &checksum, &off_open, &off_open_ref, &off_wopen, &off_ret, &off_free_ptr, &off_free_fn) != 7) {
        checksum = 0;
        off_open = 0;
        off_open_ref = 0;
        off_wopen = 0;
        off_ret = 0;
        off_free_ptr = 0;
        off_free_fn = 0;
    }
}

bool ModOverlay::check(LCS::Process const &process) const {
    return checksum && off_open && off_open_ref && off_wopen && off_ret && off_free_ptr && off_free_fn && process.Checksum() == checksum;
}

void ModOverlay::scan(LCS::Process const &process) {
    auto data = process.Dump();

    // Find call to fopen
    auto open_call_offset = FindOpen(data);
    if (!open_call_offset[0]) {
        throw std::runtime_error("Failed to find fopen call!");
    }

    off_open_ref = (PtrStorage)(open_call_offset[1] - data.data());
    off_open = process.Debase(bytes2ptr(open_call_offset[1]));

    // Find call to wfopen
    auto wopen_call_offset = FindWOpen(data);
    if (!wopen_call_offset[0]) {
        throw std::runtime_error("Failed to find wfopen call!");
    }
    off_wopen = process.Debase(bytes2ptr(wopen_call_offset[1]));

    // Find ret
    auto ret_offset = FindRet(data);
    if (!ret_offset[0]) {
        throw std::runtime_error("Failed to find ret offset!");
    }
    off_ret = (PtrStorage)(ret_offset[1] - data.data());

    // Find EVP_PKEY_METHOD array
    auto free_ptr_offset = FindFree(data);
    if (!free_ptr_offset[0]) {
        throw std::runtime_error("Failed to find pmeth arr offset!");
    }

    off_free_ptr = process.Debase(bytes2ptr(free_ptr_offset[1]));
    off_free_fn = (PtrStorage)(free_ptr_offset[2] - data.data());

    checksum = process.Checksum();
}

namespace {
    struct ImportTrampoline {
        uint8_t data[64] = {};
        static ImportTrampoline make(Ptr<uint8_t> where) {
            ImportTrampoline result = {
                { 0xB8u, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE0, }
            };
            memcpy(result.data + 1, &where.storage, sizeof(where.storage));
            return result;
        }
    };

    struct CodePayload {
        // Pointers to be consumed by shellcode
        Ptr<uint8_t> org_open_ptr = {};
        Ptr<char16_t> prefix_open_ptr = {};
        Ptr<uint8_t> wopen_ptr = {};
        Ptr<uint8_t> org_free_ptr = {};
        PtrStorage find_ret_addr = {};
        PtrStorage hook_ret_addr = {};

        // Actual data and shellcode storage
        uint8_t hook_open_data[0x80] = {
            0xc8, 0x00, 0x10, 0x00, 0x53, 0x57, 0x56, 0xe8,
            0x00, 0x00, 0x00, 0x00, 0x5b, 0x81, 0xe3, 0x00,
            0xf0, 0xff, 0xff, 0x8d, 0xbd, 0x00, 0xf0, 0xff,
            0xff, 0x8b, 0x73, 0x04, 0x66, 0xad, 0x66, 0xab,
            0x66, 0x85, 0xc0, 0x75, 0xf7, 0x83, 0xef, 0x02,
            0x8b, 0x75, 0x08, 0xb4, 0x00, 0xac, 0x3c, 0x2f,
            0x75, 0x02, 0xb0, 0x5c, 0x66, 0xab, 0x84, 0xc0,
            0x75, 0xf3, 0x8d, 0x85, 0x00, 0xf0, 0xff, 0xff,
            0xff, 0x75, 0x20, 0xff, 0x75, 0x1c, 0xff, 0x75,
            0x18, 0xff, 0x75, 0x14, 0xff, 0x75, 0x10, 0xff,
            0x75, 0x0c, 0x50, 0xff, 0x53, 0x08, 0x83, 0xf8,
            0xff, 0x75, 0x17, 0xff, 0x75, 0x20, 0xff, 0x75,
            0x1c, 0xff, 0x75, 0x18, 0xff, 0x75, 0x14, 0xff,
            0x75, 0x10, 0xff, 0x75, 0x0c, 0xff, 0x75, 0x08,
            0xff, 0x13, 0x5e, 0x5f, 0x5b, 0xc9, 0xc2, 0x1c,
            0x00
        };
        uint8_t hook_free_data[0x80] = {
            0xc8, 0x00, 0x00, 0x00, 0x53, 0x56, 0xe8, 0x00,
            0x00, 0x00, 0x00, 0x5b, 0x81, 0xe3, 0x00, 0xf0,
            0xff, 0xff, 0x8b, 0x73, 0x10, 0x89, 0xe8, 0x05,
            0x80, 0x01, 0x00, 0x00, 0x83, 0xe8, 0x04, 0x39,
            0xe8, 0x74, 0x09, 0x3b, 0x30, 0x75, 0xf5, 0x8b,
            0x73, 0x14, 0x89, 0x30, 0x8b, 0x43, 0x0c, 0x5e,
            0x5b, 0xc9, 0xff, 0xe0
        };
        ImportTrampoline org_open_data = {};
        char16_t prefix_open_data[0x400] = {};
    };
}

void ModOverlay::wait_patchable(const Process &process, uint32_t timeout) {
    auto ptr_open = process.Rebase(off_open);
    auto ptr_open_ref = process.Rebase(off_open_ref);
    process.WaitPtrEq(ptr_open_ref, PtrStorage(ptr_open), 1, timeout);
    auto ptr_free = process.Rebase(off_free_ptr);
    process.WaitPtrNotEq(ptr_free, PtrStorage(0), 1, timeout);
    auto ptr_wopen = process.Rebase(off_wopen);
    process.WaitPtrNotEq(ptr_wopen, PtrStorage(0), 1, timeout);
}

void ModOverlay::patch(LCS::Process const &process, std::filesystem::path const& prefix) const {
    if (!check(process)) {
        throw std::runtime_error("Config invalid to patch this executable!");
    }
    std::u16string prefix_str = prefix.generic_u16string();
    for (auto& c: prefix_str) {
        if (c == u'/') {
            c = u'\\';
        }
    }
    prefix_str = u"\\\\?\\" + prefix_str;
    if (!prefix_str.ends_with(u"\\")) {
        prefix_str.push_back(u'\\');
    }
    if (prefix_str.size() * sizeof(char16_t) >= sizeof(CodePayload::prefix_open_data)) {
        throw std::runtime_error("Prefix too big!");
    }

    // Prepare pointers
    auto mod_code = process.Allocate<CodePayload>();
    auto ptr_open = Ptr<Ptr<ImportTrampoline>>(process.Rebase(off_open));
    auto org_open = Ptr<ImportTrampoline>{};
    process.Read(ptr_open, org_open);
    auto ptr_wopen = Ptr<Ptr<uint8_t>>(process.Rebase(off_wopen));
    auto wopen = Ptr<uint8_t>{};
    process.Read(ptr_wopen, wopen);
    auto find_ret_addr = process.Rebase(off_ret);
    auto ptr_free = Ptr<Ptr<uint8_t>>(process.Rebase(off_free_ptr));
    auto org_free_ptr = Ptr<uint8_t>(process.Rebase(off_free_fn));

    // Prepare payload
    auto payload = CodePayload {};
    payload.org_open_ptr = Ptr(mod_code->org_open_data.data);
    payload.prefix_open_ptr = Ptr(mod_code->prefix_open_data);
    payload.wopen_ptr = wopen;
    payload.org_free_ptr = org_free_ptr;
    payload.find_ret_addr = find_ret_addr;
    payload.hook_ret_addr = find_ret_addr + 0x16;

    process.Read(org_open, payload.org_open_data);
    std::copy_n(prefix_str.data(), prefix_str.size(), payload.prefix_open_data);

    // Write payload
    process.Write(mod_code, payload);
    process.MarkExecutable(mod_code);

    // Write hooks
    process.Write(ptr_free, Ptr(mod_code->hook_free_data));
    process.Write(org_open, ImportTrampoline::make(mod_code->hook_open_data));
}
