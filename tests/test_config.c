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

/* ── String escaping (TOML basic vs literal strings) ──────────────────────── */

TEST(Config, LiteralStringKeepsBackslashes) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "wisp_test_config_literal.toml");

    /* Single-quoted: every byte verbatim, which is how a Windows path should
     * normally be written. */
    write_text_file(path,
        "[general]\n"
        "shell = 'C:\\Users\\x\\emit.exe'\n"
        "default_cwd = 'C:\\Users\\x'\n"
    );

    Config cfg;
    config_defaults(&cfg);
    ASSERT_TRUE(config_load(&cfg, path));
    ASSERT_EQ(config_parse_error_count(), 0);
    ASSERT_STR_EQ(cfg.general.shell, "C:\\Users\\x\\emit.exe");
    ASSERT_STR_EQ(cfg.general.default_cwd, "C:\\Users\\x");

    DeleteFileA(path);
}

TEST(Config, BasicStringHonoursDefinedEscapes) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "wisp_test_config_basic.toml");

    /* The C literals below are doubled, so the file on disk reads
     *     shell = "C:\\Users\\x\\emit.exe"
     * which is the escaped spelling of C:\Users\x\emit.exe. */
    write_text_file(path,
        "[general]\n"
        "shell = \"C:\\\\Users\\\\x\\\\emit.exe\"\n"
        "default_cwd = \"C:\\\\Users\\\\x\"\n"
        "bell = \"a\\tb\"\n"
        "title = \"q\\\"\\u00e9\\\\z\"\n"
    );

    Config cfg;
    config_defaults(&cfg);
    ASSERT_TRUE(config_load(&cfg, path));
    ASSERT_EQ(config_parse_error_count(), 0);
    ASSERT_STR_EQ(cfg.general.shell, "C:\\Users\\x\\emit.exe");
    ASSERT_STR_EQ(cfg.general.default_cwd, "C:\\Users\\x");
    ASSERT_STR_EQ(cfg.general.bell, "a\tb");
    /* \u00e9 is U+00E9, two UTF-8 bytes. */
    ASSERT_STR_EQ(cfg.general.title, "q\"\xC3\xA9\\z");

    DeleteFileA(path);
}

TEST(Config, UnknownEscapeIsReportedNotDropped) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "wisp_test_config_badescape.toml");

    /* The unescaped Windows path that used to arrive as C:Usersxemit.exe. */
    write_text_file(path,
        "[general]\n"
        "bell = \"audio\"\n"
        "shell = \"C:\\Users\\x\\emit.exe\"\n"
    );

    Config cfg;
    config_defaults(&cfg);
    ASSERT_TRUE(config_load(&cfg, path));

    /* Reported, with the offending line and key named. \U is the 8-hex-digit
     * escape, so "\Users" fails as a malformed one rather than an unknown one;
     * either way it is reported instead of losing its backslash. */
    ASSERT_EQ(config_parse_error_count(), 1);
    ASSERT_NOT_NULL(strstr(config_parse_error_first(), ":3:"));
    ASSERT_NOT_NULL(strstr(config_parse_error_first(), "escape \\U needs 8 hex digits"));
    ASSERT_NOT_NULL(strstr(config_parse_error_first(), "'shell'"));

    /* ...and the key left alone rather than taking a mangled value. An empty
     * shell means auto-detect, so the failure stays recoverable. */
    ASSERT_STR_EQ(cfg.general.shell, "");

    /* Other keys on other lines still apply. */
    ASSERT_STR_EQ(cfg.general.bell, "audio");

    DeleteFileA(path);
}

TEST(Config, TruncatedUnicodeEscapeIsReported) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "wisp_test_config_badunicode.toml");

    /* On disk: title = "x\u00z9" — 'z' is not a hex digit. */
    write_text_file(path,
        "[general]\n"
        "title = \"x\\u00z9\"\n"
    );

    Config cfg;
    config_defaults(&cfg);
    ASSERT_TRUE(config_load(&cfg, path));
    ASSERT_EQ(config_parse_error_count(), 1);
    ASSERT_NOT_NULL(strstr(config_parse_error_first(), "escape \\u needs 4 hex digits"));
    ASSERT_STR_EQ(cfg.general.title, "Wisp - ${cwd}");

    DeleteFileA(path);
}

TEST(Config, UndefinedEscapeLetterIsReported) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "wisp_test_config_badletter.toml");

    /* On disk: title = "x\qy" — \q is not an escape TOML defines. */
    write_text_file(path,
        "[general]\n"
        "title = \"x\\qy\"\n"
    );

    Config cfg;
    config_defaults(&cfg);
    ASSERT_TRUE(config_load(&cfg, path));
    ASSERT_EQ(config_parse_error_count(), 1);
    ASSERT_NOT_NULL(strstr(config_parse_error_first(), "unknown escape \\q"));
    ASSERT_STR_EQ(cfg.general.title, "Wisp - ${cwd}");

    DeleteFileA(path);
}

TEST(Config, SurrogateEscapeIsReported) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "wisp_test_config_surrogate.toml");

    /* On disk: title = "x\ud800y" — a lone surrogate is not a scalar value. */
    write_text_file(path,
        "[general]\n"
        "title = \"x\\ud800y\"\n"
    );

    Config cfg;
    config_defaults(&cfg);
    ASSERT_TRUE(config_load(&cfg, path));
    ASSERT_EQ(config_parse_error_count(), 1);
    ASSERT_NOT_NULL(strstr(config_parse_error_first(), "not a Unicode scalar value"));
    ASSERT_STR_EQ(cfg.general.title, "Wisp - ${cwd}");

    DeleteFileA(path);
}

TEST(Config, HashInsideStringIsNotAComment) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "wisp_test_config_hash.toml");

    write_text_file(path,
        "[general]\n"
        "default_cwd = 'C:\\Users\\x#1'  # trailing comment\n"
    );

    Config cfg;
    config_defaults(&cfg);
    ASSERT_TRUE(config_load(&cfg, path));
    ASSERT_EQ(config_parse_error_count(), 0);
    ASSERT_STR_EQ(cfg.general.default_cwd, "C:\\Users\\x#1");

    DeleteFileA(path);
}
