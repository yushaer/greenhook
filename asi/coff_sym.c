/* Fast PE COFF symbol table resolver for unstripped MinGW/LLVM PE binaries. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "coff_sym.h"

#pragma pack(push, 1)
typedef struct {
    union {
        char ShortName[8];
        struct {
            uint32_t Zeroes;
            uint32_t Offset;
        } Name;
    } N;
    uint32_t Value;
    int16_t SectionNumber;
    uint16_t Type;
    uint8_t StorageClass;
    uint8_t NumberOfAuxSymbols;
} CoffSymRec;
#pragma pack(pop)

_Static_assert(sizeof(CoffSymRec) == 18, "COFF symbol record must be 18 bytes");

size_t coff_resolve_batch(void *hmodule, CoffSymReq *reqs, size_t count) {
    if (!hmodule || !reqs || !count) return 0;

    size_t remaining = 0;
    for (size_t k = 0; k < count; k++) {
        if (!reqs[k].addr && reqs[k].name && *reqs[k].name) remaining++;
    }
    if (!remaining) return count;

    wchar_t path[MAX_PATH * 2];
    DWORD plen = GetModuleFileNameW((HMODULE)hmodule, path, (DWORD)(sizeof path / sizeof path[0]));
    if (!plen || plen >= (DWORD)(sizeof path / sizeof path[0])) return 0;

    HANDLE hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER fsz = {0};
    if (!GetFileSizeEx(hf, &fsz) || fsz.QuadPart < 0x100) {
        CloseHandle(hf);
        return 0;
    }
    const uint64_t file_bytes = (uint64_t)fsz.QuadPart;

    HANDLE hm = CreateFileMappingW(hf, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hm) {
        CloseHandle(hf);
        return 0;
    }

    const uint8_t *base = (const uint8_t *)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!base) {
        CloseHandle(hm);
        CloseHandle(hf);
        return 0;
    }

    size_t resolved = 0;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic == IMAGE_DOS_SIGNATURE && (uint64_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) <= file_bytes) {
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
        const DWORD ptr_sym = nt->FileHeader.PointerToSymbolTable;
        const DWORD num_sym = nt->FileHeader.NumberOfSymbols;
        const WORD num_sec = nt->FileHeader.NumberOfSections;
        const uint64_t sym_table_end = (uint64_t)ptr_sym + (uint64_t)num_sym * 18ULL;
        const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);

        if (nt->Signature == IMAGE_NT_SIGNATURE && ptr_sym && num_sym &&
            (uint64_t)((const uint8_t *)sec - base) + (uint64_t)num_sec * sizeof(IMAGE_SECTION_HEADER) <= file_bytes &&
            sym_table_end + 4ULL <= file_bytes) {

            const uint8_t *sym_ptr = base + ptr_sym;
            const uint8_t *strtab = base + sym_table_end;
            uint32_t strtab_sz = 0;
            memcpy(&strtab_sz, strtab, 4);
            if (sym_table_end + strtab_sz > file_bytes) strtab_sz = (uint32_t)(file_bytes - sym_table_end);

            const uintptr_t mod_base = (uintptr_t)hmodule;

            for (DWORD i = 0; i < num_sym && remaining > 0; ) {
                const CoffSymRec *sym = (const CoffSymRec *)(sym_ptr + (uint64_t)i * 18ULL);
                if (sym->SectionNumber > 0 && sym->SectionNumber <= (int16_t)num_sec &&
                    (sym->StorageClass == IMAGE_SYM_CLASS_EXTERNAL ||
                     sym->StorageClass == IMAGE_SYM_CLASS_STATIC)) {

                    char short_buf[9];
                    const char *sname = NULL;
                    if (sym->N.Name.Zeroes != 0) {
                        memcpy(short_buf, sym->N.ShortName, 8);
                        short_buf[8] = 0;
                        sname = short_buf;
                    } else if (sym->N.Name.Offset >= 4 && sym->N.Name.Offset < strtab_sz) {
                        const char *cand = (const char *)(strtab + sym->N.Name.Offset);
                        const size_t max_len = strtab_sz - sym->N.Name.Offset;
                        if (strnlen(cand, max_len) < max_len) sname = cand;
                    }

                    if (sname && sname[0] != '.' && sname[0] != '$') {
                        for (size_t k = 0; k < count; k++) {
                            if (!reqs[k].addr && !strcmp(sname, reqs[k].name)) {
                                const uint64_t rva = (uint64_t)sec[sym->SectionNumber - 1].VirtualAddress + (uint64_t)sym->Value;
                                reqs[k].addr = (void *)(mod_base + (uintptr_t)rva);
                                resolved++;
                                remaining--;
                                break;
                            }
                        }
                    }
                }
                i += 1u + (DWORD)sym->NumberOfAuxSymbols;
            }
        }
    }

    UnmapViewOfFile(base);
    CloseHandle(hm);
    CloseHandle(hf);
    return resolved;
}

void *coff_resolve_one(void *hmodule, const char *name) {
    CoffSymReq req = {name, NULL};
    coff_resolve_batch(hmodule, &req, 1);
    return req.addr;
}
