#include "test_helpers.h"
#include "../src/platform/config.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

TEST(Config, ParsesHexColors) {
    ASSERT_EQ(config_parse_color("#1a2b3c"), 0x1A2B3C);
    ASSERT_EQ(config_parse_color("ddeeff"), 0xDDEEFF);
    ASSERT_EQ(config_parse_color("#abcd"), 0);
    ASSERT_EQ(config_parse_color(NULL), 0);
}

TEST(Config, LoadsTomlOverrides) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "wisp_test_config.toml");

    write_text_file(path,
        "[general]\n"
        "shell = \"pwisp.exe\"\n"
        "scrollback = 1234\n"
        "confirm_exit = false\n"
        "default_cwd = \"C:\\\\work\"\n"
        "\n"
        "[font]\n"
        "family = \"Consolas\"\n"
        "size = 15.5\n"
        "ligatures = false\n"
        "\n"
        "[cursor]\n"
        "style = \"underline\"\n"
        "blink = false\n"
        "blink_rate_ms = 777\n"
        "\n"
        "[colors]\n"
        "background = \"#112233\"\n"
        "foreground = \"#aabbcc\"\n"
        "\n"
        "[tabs]\n"
        "enabled = false\n"
        "position = \"bottom\"\n"
        "max_tabs = 7\n"
    );

    Config cfg;
    config_defaults(&cfg);
    ASSERT_TRUE(config_load(&cfg, path));
    ASSERT_STR_EQ(cfg.general.shell, "pwisp.exe");
    ASSERT_EQ(cfg.general.scrollback, 1234);
    ASSERT_FALSE(cfg.general.confirm_exit);
    ASSERT_STR_EQ(cfg.general.default_cwd, "C:\\work");
    ASSERT_EQ(wcscmp(cfg.font.family, L"Consolas"), 0);
    ASSERT_TRUE(cfg.font.size > 15.4f && cfg.font.size < 15.6f);
    ASSERT_FALSE(cfg.font.ligatures);
    ASSERT_EQ(cfg.cursor.style, CURSOR_UNDERLINE);
    ASSERT_FALSE(cfg.cursor.blink);
    ASSERT_EQ(cfg.cursor.blink_rate_ms, 777);
    ASSERT_EQ(cfg.colors.background, 0x112233);
    ASSERT_EQ(cfg.colors.foreground, 0xAABBCC);
    ASSERT_FALSE(cfg.tabs.enabled);
    ASSERT_STR_EQ(cfg.tabs.position, "bottom");
    ASSERT_EQ(cfg.tabs.max_tabs, 7);

    DeleteFileA(path);
}
