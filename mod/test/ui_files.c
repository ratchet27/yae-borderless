/* Writes ui_ini.lua with the Display Mode addition appended, as ui.c adds it
 * to the game's Lua state after ui_ini.lua runs.
 * Usage: ui_files.exe <original ui_ini.lua> <output>
 * Check the output with test/check_layout.lua. Run it from the game folder
 * so the language is detected from gameres. */
#include "../src/ui.c"
#include <stdio.h>

void log_line(const char *format, ...)
{
    (void)format;
}

int main(int argc, char **argv)
{
    char *data;
    DWORD size;
    FILE *out;
    if (argc < 3 || !read_whole(argv[1], &data, &size) || !(out = fopen(argv[2], "wb")))
        return 1;
    fwrite(data, 1, size, out);
    fputs(layout_addition(), out);
    fclose(out);
    return 0;
}
