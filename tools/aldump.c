// aldump.c - dump/lookup tool for the pre-NG FO4 Address Library bin
// (format V0: uint64 count, then count x {uint64 id, uint64 offset}).
// Usage: aldump.exe <version-bin> [id ...]
// With ids: prints id -> offset (or MISSING) for each. Without: summary.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct { uint64_t id; uint64_t off; } Mapping;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: aldump.exe <version-bin> [id ...]\n");
        return 2;
    }

    FILE* f = NULL;
    if (fopen_s(&f, argv[1], "rb") != 0 || !f)
    {
        printf("cannot open %s\n", argv[1]);
        return 1;
    }
    uint64_t count = 0;
    if (fread(&count, sizeof(count), 1, f) != 1)
    {
        printf("cannot read header\n");
        fclose(f);
        return 1;
    }
    Mapping* map = (Mapping*)malloc((size_t)count * sizeof(Mapping));
    if (!map || fread(map, sizeof(Mapping), (size_t)count, f) != count)
    {
        printf("cannot read table\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    if (argc == 2)
    {
        printf("entries: %llu\n", (unsigned long long)count);
        printf("first id: %llu  last id: %llu\n",
               (unsigned long long)map[0].id,
               (unsigned long long)map[count - 1].id);
        return 0;
    }

    int missing = 0;
    for (int i = 2; i < argc; i++)
    {
        uint64_t id = _strtoui64(argv[i], NULL, 0);
        // table is sorted by id - binary search
        uint64_t lo = 0, hi = count - 1, found = UINT64_MAX;
        while (count > 0 && lo <= hi)
        {
            uint64_t mid = lo + (hi - lo) / 2;
            if (map[mid].id == id) { found = mid; break; }
            if (map[mid].id < id) lo = mid + 1;
            else if (mid == 0) break;
            else hi = mid - 1;
        }
        if (found != UINT64_MAX)
            printf("%llu -> offset 0x%llX\n",
                   (unsigned long long)id, (unsigned long long)map[found].off);
        else
        {
            printf("%llu -> MISSING\n", (unsigned long long)id);
            missing++;
        }
    }
    printf("missing: %d/%d\n", missing, argc - 2);
    free(map);
    return 0;
}
