/* Transliterated from the test blocks in Ghostty's
 * src/terminal/kitty/graphics_image.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same names, same inputs, same assertions, in
 * upstream's order. Where an assertion differs it carries a `Wisp:` note
 * naming the upstream assertion it replaces.
 *
 * Wisp, the test-harness seams:
 *   - `testing.tmpDir(.{})` is the local `TmpDir` below: a uniquely named
 *     directory under %TEMP% with the handful of std.Io.Dir operations
 *     upstream's tests use (writeFile, realPath, realPathFile, access,
 *     createDir, openDir, cleanup).
 *   - `@embedFile("testdata/...")` cannot be reproduced: the port's
 *     build wires test resources through `tests/res/*.inc`, and adding
 *     one is out of scope for this change. The three test vectors are
 *     embedded below as base64 of the exact upstream bytes and decoded
 *     at startup, so the tests run on upstream's data unchanged.
 *   - `error.SkipZigTest` for a test upstream skips on Windows is an
 *     empty body with a note; the port targets Windows only.
 *   - `testing.allocator`'s leak check is `zigstd::testing_state().live`,
 *     asserted by the trailing `zz_Wisp_no_leaks` test.
 */

#include <direct.h>
#include <process.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_helpers.h"
#include "../terminal/kitty/graphics_image.hpp"
#include "../terminal/sys.hpp"
#include "../zigstd/allocator.hpp"

using namespace wisp;

namespace gi = wisp::terminal::kitty::graphics_image;
namespace cmdns = wisp::terminal::kitty::graphics;
namespace kwin = wisp::terminal::kitty::windows;
namespace sysns = wisp::terminal::sys;

typedef gi::Error Error;
typedef gi::LoadingImage LoadingImage;
typedef gi::Image Image;
typedef gi::LoadingImage::Limits Limits;
typedef cmdns::Transmission Transmission;
typedef cmdns::Command Command;

static zigstd::Allocator testAlloc() { return zigstd::testing_allocator(); }

/* ── Wisp: base64 of the upstream testdata files. ───────────────────── */

static int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode a split base64 literal into a heap buffer owned by the caller.
 * Returns the byte length; `*out` is allocated with `alloc`. */
static size_t decodeB64(zigstd::Allocator alloc, const char *const *parts, size_t n_parts,
                        uint8_t **out) {
    size_t chars = 0;
    for (size_t i = 0; i < n_parts; i += 1) chars += strlen(parts[i]);
    uint8_t *buf = alloc.alloc(chars / 4 * 3 + 3);
    size_t len = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n_parts; i += 1) {
        for (const char *p = parts[i]; *p != 0; p += 1) {
            if (*p == '=') continue;
            const int v = b64Value(*p);
            if (v < 0) continue;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                buf[len] = (uint8_t)((acc >> bits) & 0xFF);
                len += 1;
            }
        }
    }
    *out = buf;
    return len;
}

#define B64_PARTS(a) (a), (sizeof(a) / sizeof((a)[0]))

/* RAII for a decoded test vector. */
struct Vector {
    zigstd::Allocator alloc;
    uint8_t *data;
    size_t len;
    size_t cap;
    Vector(zigstd::Allocator a, const char *const *parts, size_t n) : alloc(a) {
        size_t chars = 0;
        for (size_t i = 0; i < n; i += 1) chars += strlen(parts[i]);
        cap = chars / 4 * 3 + 3;
        len = decodeB64(a, parts, n, &data);
    }
    ~Vector() { alloc.free(data, cap); }
};

/* ── Wisp: testing.tmpDir(.{}) ──────────────────────────────────────── */

struct TmpDir {
    char path[1024];
    size_t path_len;
    const char *names[16];
    size_t names_len;
    const char *dirs[4];
    size_t dirs_len;

    TmpDir() : path_len(0), names_len(0), dirs_len(0) {
        static unsigned counter = 0;
        counter += 1;
        const char *base = getenv("TEMP");
        if (base == nullptr) base = getenv("TMP");
        if (base == nullptr) base = ".";
        const int n = snprintf(path, sizeof path, "%s\\wisp-kitty-gfx-%u-%u", base,
                               (unsigned)_getpid(), counter);
        path_len = (size_t)(n < 0 ? 0 : n);
        _mkdir(path);
    }

    ~TmpDir() { cleanup(); }

    /* Wisp: std.Io.Dir.writeFile */
    void writeFile(const char *sub_path, const void *data, size_t len) {
        char full[1200];
        snprintf(full, sizeof full, "%s\\%s", path, sub_path);
        FILE *f = fopen(full, "wb");
        if (f == nullptr) return;
        if (len > 0) fwrite(data, 1, len, f);
        fclose(f);
        if (names_len < 16) {
            names[names_len] = sub_path;
            names_len += 1;
        }
    }

    /* Wisp: std.Io.Dir.createDir */
    void createDir(const char *sub_path) {
        char full[1200];
        snprintf(full, sizeof full, "%s\\%s", path, sub_path);
        _mkdir(full);
        if (dirs_len < 4) {
            dirs[dirs_len] = sub_path;
            dirs_len += 1;
        }
    }

    void join(const char *sub_path, char *buf, size_t buf_len) const {
        snprintf(buf, buf_len, "%s\\%s", path, sub_path);
    }

    /* Wisp: std.Io.Dir.access */
    bool access(const char *full) const { return _access(full, 0) == 0; }
    bool accessSub(const char *sub_path) const {
        char full[1200];
        join(sub_path, full, sizeof full);
        return access(full);
    }

    void cleanup() {
        char full[1200];
        for (size_t i = 0; i < names_len; i += 1) {
            join(names[i], full, sizeof full);
            remove(full);
        }
        for (size_t i = 0; i < dirs_len; i += 1) {
            char inner[1200];
            join(dirs[i], inner, sizeof inner);
            /* Files written into a sub directory are recorded under their
             * "<dir>\\<name>" sub path, so only the directory is left. */
            _rmdir(inner);
        }
        _rmdir(path);
        names_len = 0;
        dirs_len = 0;
    }
};

/* Wisp: `buf[0..try dir.realPath(io, &buf)]` and `realPathFile`. Both go
 * through the port's file_io, which is what the implementation uses. */
static size_t realPathOf(zigstd::Allocator alloc, const char *p, char *buf, size_t buf_len) {
    size_t out = 0;
    if (!gi::file_io::realPathAt(alloc, p, strlen(p), buf, buf_len - 1, &out)) return 0;
    /* Wisp: the slice upstream gets back is not NUL terminated; the C
     * helpers here (fopen, _access) need one, so terminate in place. */
    buf[out] = 0;
    return out;
}

/* Wisp: builds the `command.Command` literal upstream's tests write out.
 * `data` is duped with `alloc` so `cmd.deinit(alloc)` frees it, as
 * upstream's `try alloc.dupe` does. */
static Command makeTransmit(zigstd::Allocator alloc, Transmission t, const char *data,
                            size_t data_len) {
    Command cmd;
    cmd.control.tag = Command::Action::transmit;
    cmd.control.transmit = t;
    if (data_len > 0) {
        cmd.data = alloc.dupe((const uint8_t *)data, data_len);
        cmd.data_len = data_len;
    }
    return cmd;
}

static Command makeTransmitZ(zigstd::Allocator alloc, Transmission t, const char *data) {
    return makeTransmit(alloc, t, data, strlen(data));
}

/* Wisp: `defer cmd.deinit(alloc)` */
struct CmdGuard {
    Command *cmd;
    zigstd::Allocator alloc;
    CmdGuard(Command *c, zigstd::Allocator a) : cmd(c), alloc(a) {}
    ~CmdGuard() { cmd->deinit(alloc); }
};

/* Wisp: `defer loading.deinit(alloc)` */
struct LoadingGuard {
    LoadingImage *img;
    zigstd::Allocator alloc;
    bool armed;
    LoadingGuard(LoadingImage *i, zigstd::Allocator a) : img(i), alloc(a), armed(true) {}
    ~LoadingGuard() {
        if (armed) img->deinit(alloc);
    }
};

/* Wisp: `defer img.deinit(alloc)` */
struct ImageGuard {
    Image *img;
    zigstd::Allocator alloc;
    ImageGuard(Image *i, zigstd::Allocator a) : img(i), alloc(a) {}
    ~ImageGuard() { img->deinit(alloc); }
};

/* image-rgb-zlib_deflate-128x96-2147483647-raw.data, 30455 bytes */
static const char *const b64_zlib_128x96[] = {
    "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",
    "f4HXvI7Dyt8FjgUvD7nAAYWBYkgwLBGOI6GIXhiPM+dfv/Dxp59//f3FH6748ESSzJw7NTV1U1Pqra2dnZ09rXa6ubmVxmBd",
    "vOz95bff//3zL9/54L/e/Mt7b7z73tnX38CcPo328sKQPFEEYFgg8B4Yj3Mo4hkwHVCsJxiuSII9BOmGJjjBMc4ILBSLg+Hw",
    "WM+zSIKnOwqHInrBsEQkwROB94Bhic4wlCME4QRFuiIwLnC0gzvcFYmAYNAYD+I7H76fkBRrXpjb213/8fH93389+sdvj44O",
    "NvY2F81zqtXlmdmZiQWLVtnfV5hz66N33yVhMHgs7vzZc9fS01KSE69fTcnJvFFXXdav6OxsaywryS/Iv1NYkFdfW30373ZS",
    "cjSF6s/gUCOiZeHRMRExsVHxyek3b8WmpQbS6e/89eOPv/rytb+8D8ESwH58nr8TCuWMRgONgETaIeC2cNgJf1sYygmKBgcr",
    "BE1EEk57nH/9/IW3zl9468yrF17/y/tffvcDlcXhi6URMfHFZZXjKu3CwtKjRz8+fvzkwYOHev0MsKSPKZtb24pKK8qqam/d",
    "LYhNShVFRPpTae998sm5N98888prb7//4RvvfvDGux+8/s6HL134C6hpUKynG4oIwXjAcF4uSBwM7wlq1Ctvvf3uX//2/t8/",
    "e+nNt71efv2Ndz/48wcf/f3r7z7+4uuX//wOkuDpDLxnpJ0r1BWBcUVgnGBQ3GmvS34+5bXV23ubh4/u/+P3nx7c3/7916P/",
    "3//6+f7OysH2Msh/zjhpNIz3d3ZcjY999exZIhpNwOE/++TT7s722urKhpqq5vrapvqqzrbGns6Wof4e9dT4gmUOPJqQ97ZV",
    "VhenXE2QhYtEstCImFhpRAzQArdzUzNvRqemMETCcxfecoKj7GAIOxjCAYFyRmNB/XFGo10wGBcMkA6Qvw0MfsLfDYFC4AiE",
    "02c9z798/pXX3/3go0+//Prvn3/5wcd//eGKtyw0/FbO7cbmFpVGu7C0bLLM379//+joaH19fW9vb2lhwTA9PTszo9fpjEaj",
    "Wq3WaDRKpVLeq+gb6C+vrODyeXQ2Sxoelnotg80XyCKjP/jr31964y13JNYNiYVhiW4IDBSNd0diXWAo0tmXWXxxSUVZ/8hQ",
    "U3trTt6dG9lZqdfS0zKu1zQ0llfXJKSk0tmcv332+blXXoEgEHgPDxQOB0VAL7x1QdEnX1pZOtjf3N/b+PXpw/2d1f/555P/",
    "9d8/rS0aDvdW1pdm1pdmLCa1crSns6EyJVr2zoVX3n7j5QuvvxYTHdnc0jhjNGhUk/298gWTaUo51tHSvGSxGLRay+ysUqns",
    "7+/v7evq7GppbKnPL8yLTUyJiImXRsSFRSdmF+Tfq6mOTIr/6ItPPV55CYrHOiLRJy0AwndGo4+NEAplD4fbwWBggE9DEz3O",
    "vPLa+x//7fNvvrvk48cVimMTU2ITU8KjY27nF4wqxxeWlheXV9Y3t/YO7m9u7zx+/Hh/f39vb29ra2tjbc08N6fTaPQ63cbG",
    "htFo1Ol0BoNBp5+eNuinDfqJqcnBsZEpnWZCrRkaU7b1KGKTUi/5BhJOn4dhiYCVReHwXufwXufefv8jCpNb09i6sLK0trVh",
    "Xpw3L86r9bqRCaVKNz0zZ9LoDUNjyv7hkTsFheLQMG//gAByyF8//eztd98uLC5Y31zf3t1+8uODvd31R4c72xuL//t/nv7y",
    "9MHW6tzh3srqgn7RpDFMjw0PdjRXlWSmxvGYVP8rPzDo1Py7d2bnjKtrK8uLC6ZZ4/L8/OjgQHlJcU9He9HdvII7t/Pz8ysq",
    "Kpqaa+/m52TnZN7Oy8nIvJWWcTMsOlEWGR+VlFTZ2FBUWeZHJX9+8YfTr7/qhiM4oTB2MIQ1BcfiAxohZzTaAYH4A/8z586/",
    "/+FH8QlJd/Lyb+fd6RvoX1paUqlU09PTWq12a2vr8PDw4YMHj4+O7u/vHx0ePjy6/+jx4YPD/YP7u5vrK0sLZoNObdRrlxfM",
    "swbdrEE3NzM9rZky6rUz05q5meml5cW9/d0506zJPGcwzoyNK1vbO25mZotkoa+88cbb737A4gnKyqsNM6at7f3Dhz8ePT7a",
    "v7+/u7+7trG2vLK0tLxomTevra8uLS8uLgG1sLA4393T2d3TWVJSlJ+fl5Icr54aNxp066tLTx7dP3qwu7u1fLC79uuT+z8d",
    "7T55uPXo/vrGsnFGN7a+NKMc7q65dyszLTIxVpKaGKaQt2vUyuWVhaNHD3a3t/Q6bXV5hXJ4JDczhxIY8vab73mRzoWEhAgE",
    "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",
    "aIwrCu2MQDogYU5oBDh5HZBQByT0n7//sr+7tbZs2VhdWF80ry2YVi2zCzPT5mmNUT1p0Ki2VpaOHuwfHuzu7WzOm2e31tYW",
    "zWadSmWamelub9NrNKvz8+NDQ5rJifm5uY2VFb1GMzzYPzo8qJ6aGBsZUnR1DvYqFF0dFfeKi+7eqS4vrasu6+5oBjf9zrbG",
    "7o5meVerorttoLdrsK97bLB/pL93UNEzOTo8Ntjb097c29U2IO+UtzUMKzrmZ1QLRrVqVDGsaB1WtI71d4wPtQ33Nk6OdKxY",
    "VDur0waV4uH27O6KdsOs3LSMW7RyzXCjdqSp9HbCvdz4vtaijpaqno66ob6WSWXP3pb5/u6CVj0wMtRecDcjJopPJV+mki8z",
    "qAGUYJ/gIF8aNYjF9vP2+dzP7xsazUcqpoVKGfEJgvgEQUKyIDaBG5MsTbgafjHA+69ffXr+zfeQxLPgqZqNO8IehnZBYtzQ",
    "ODBcERgnKNIeAreHwO2sN6mC5w8PH+zvbK1vbyzPmwzLZqPZoF2bn1u1zFr0Woteu7m8+GAHKP6fnzza3d7YWFveXl83aLXz",
    "c3NTyjHt1KRmcqKppkbR0THc3zelHJscG+2X94DwZ2cMplnjksWyaDZPKkdqKsruFebXVJQ1N1TXVZfVVN5rqq9qa65rb6lv",
    "bqjubGtUdLf19rTLO9oUne39PV193Z193e3NdVWKztYBeWdXc21vR9OCUb1gVE8O98jbaofkLcOK1tH+5tH+ZtVY18GWaXtF",
    "px7r2FnSLM2M6JXt2pGWvtYiRXNBS2VWVUFqRjy3ruRaTcXdfnlzb3f9cH/r4f7Solk1NaHo7226eT1OJqGzGH58brCAS6NT",
    "/MnBfnRaMJcXyGT5UihXgoMvioWU6EheVDQ7PkEQHceOiefEpsi4Esq5C69gvAgo0jkXBN56Yw8C/KSGMwJIgSsKC/J3hCDs",
    "3GF27jBb62VKkP/B7pY1A0uAmZ8zmA3aOa1q0aifN+hMOvXk6LDZML2zuXZ4sPtgf2d3a319eXnJYrHMzo6PDGunJnu7uxpr",
    "a1oa6lsbG2orK5rqajtbW/oUPf298tFhYMhOjo0atNqRgb6Gmqr66srCvNs5WRl5uZklhXeqyosbaitam2rra8prq0rrqio6",
    "Wpr6e7qUQwNDvfKO5sb2poYBebdRpzLqVCa9Zlo1bpyeXF0waiYHlcM9Q31tY0NdU6OdmvGe4d7GuenhqZHWGbVicWZwzTym",
    "GW4e6a5sr7ldW5xxLzcxK0UiYV7Kux5ZWXq7s7Va0VXX19NgmZ2Y1gxOjHU31RVnpEcLeEFcdqCQHyIU0qlUXyrVn8OhiMVs",
    "LpdKoVxhsQJEYmpoGCsiip+SFhkWI4lKCPMPCXz1rTcgOIw7Fm0HQ9hAYDYw+Cno8QmbdQVGHW8BSJQD1ArfDQo8DXL8nO2N",
    "1Qf7O+sr86tL5nnj9JJpZlYzZdFrF416o3pyqFeunRyfm5neWl852N3a3VpfXVy0zM7qNZrJsdHh/r6RgX55Z0fFvZLi/Lsl",
    "BfmlRYW3s7OyMm8UFea3Nje2tzYP9ir6erq721ury0vzcrLzcrJLi+9WlBaWleQX5N3Ky80EyFeXVZYVVZXdq60s72xpmhgZ",
    "6mxpaqmvbW2oGx8e1EyM6qaUU2NDBs2kyaDaWDZpJgfVEwPjIz0To/LJkY6J4faBnjpFR1V/V8WwombJODSnkWtHWoa7KqoL",
    "03Ouht5Kk0lZl8mXP44WBRbdvVFdntfRUjE+2jUy0DbY29zX05B/Oz3zRjyPE8DjBPE4QQIBjUr1pdMDeTyaWMwWCpkcTpBM",
    "xhJLaPEJkpg4MYsTGEj1ffPd19AkHMYDDyPgnJFwBwTqlDv0RQj0FBRmC0fYIZAn14ZA/o4wOFj/z/PfWF3a2VzbXV+eN05b",
    "ZnQmvWbeoJvVTOnGR3Xjo5OjwyrlqNkwbdRpzLMGk1EPkh8fGZ4YHQFbQDk8NKCQ11dXVZeX5eXcSk1MuJqWkpV5I+9O7t28",
    "20V380oK8gvu5JaXFBXm3c5IS06Mi0xPTbienhwdIY0IFd2+daM4Py8vJ7umoqyxtrqloW5A0dPSUCfvbB/qU0yMDY8O9qsn",
    "lerJ0UnlkEmvWZ2fsxg1owM9Y0Pd/fKW0f7WrpaKYXlDd3PZQHdle0N+Z8OdycE61WC9UlFVfCv+RqIwShDEDvw64LuPOUHf",
    "JUWLC3OvVd+7PT05qBlXNFYXVJXdzsyIS0sJ57D8BDwyEAIajebHYATxeLSwMF5oKFckocnCWCIZIyJG8M3lrzCeaCiG4IbE",
    "gpeEHJFoK1I3W5j7i+5wGygS/GwFeArkBEe5IDHOCLQDFHFM3h1xyg1+CoI4BUEsL5jNs4Z547RRpzJoJlXKYc3Y8MRgn1Y5",
    "YvXz8p721qFeuV49ZZmbMeq1k2OjIwP9iq7O9uamrrbWmory5vq65vq6AYW8u72ttKgwNTHhWnpaSXFh2b3i6xnpqYkJmRnX",
    "QsUCJpXMooXQQ4Kowf7UYP9QCT81KTY6Qiris+KjI9KS4u8V5leV3aupKKssLWmsre5qa+lsbe5qa1EODw739/bJO0aHeseH",
    "+2en1csWw8RIn7yzob25qrO5fFDe2N1c1tFQ0liV096Q31hxs6Ykvb+tuKUyKztVmhbN4pG/Y/p/SfP5ghP0nZAVlBonu5OZ",
    "omiv0070Vpfm5mYlX00OjwznsJm+gPjwQ/h8KpMZyOFQhEJmeDhfImFJZIyIKF5MgkQgoZ1+1csF4ewCR9u7wezckPbuKLDO",
    "7eCQ/6+z4ykIwhaGckBiHVE48Cwa5O8ERzlAEafcoUCPuMKAcAdSsGCenTUAZW/Sa2an1Xr1xKxmak6r0o2PAokYGRpU9DRU",
    "V26tLM2bjOCpWr+8u1/eXV9d2d3e2lRX01xfW1V2r6OlqaGmqrqyLP/u7dycrKLCu3U1lRVlJVnXM/JybiXFRVODA8IkQhGP",
    "TSMH8Dl0qYibBKzvkeFS0e3szNTEuPSUxKK7d8pLi0tLClua6ttbmxprq+urK0cG+uSd7X09XepJZUdrw1B/j3py1KhXD/Z1",
    "NjdU9nU2NFQWNNXktzeW1JVnNVXnNlfdys+OLbmTlJEoDOX586g/0P2/Dbr0Gc3vW6rvN2yyt5ARGB8hyMtKrS3Lu5udlpoY",
    "GhvJFwspPE4AlxvM45F5PAqPRxGJGFIpWywhS2UUWQQ7NJIjCOX+4PstDA+YfFsIxvq5G7wDwtMWggECCj/lDrWBYOzguOP7",
    "85+7ng52io078kVX+PFndtyAFwFWzrmZ9UXz5OjgtGpcOzm2uWTZXLJY9Fr9pLKvu7OhunJ8eHBydBg8PdBrVZPKkeH+3vrq",
    "SpB5TUVZwZ3cprqaprqa2uqKwoK8vDs5t3Ozi4vyqypKS4sKryYn5Wbd5DJpoWJBcnxMTKRMLGBHhIpiImXpKYnR4bKczBsg",
    "f2BA3MmpLL9XXlpcVQFM5MrSkpaGusFeubyzfbBXruhuU02MDPZ1D/X3jA7Je3ta2+rL+jobyotuVpfeqq/IbqzKaarMLrmT",
    "dCNFfDNVEsrzZwR+Rb7yhffXH4V4fxV06XOy9zdUv+8Z5CvJMZKkaHGUlB0qpstENAEvSMALYrMDeVb9kUhYAHwxE+QvDWdF",
    "xgqoHPL7f/sLDE+wh0Bt3NF2UOyfXDDWzwBi7GE48Eo0eEfH8X1Tz9/PgCI6w3G2EBTIH4RvD8OBG9b4cP/YYK9yqE/R2Tpv",
    "nF5fNG8sWbZXF/t7ukoL81vqa1XK0fHhwdGBPvWkcmxoYGxooKejraWhrrG2uqS4oLgov7y0GCzdyvJ7xUX52Vk3cnOy8u7k",
    "FBfl37yRnn41mc2iRUbIYqLDo6PCkpPihAJOTHR4Wmri9Yy0mzfS01IThQJedFREWmpiYUHendu3yu4VVVeWFRXera+tVvR0",
    "tbU09vf2KLo6Zg06cF8eG+5TdLc1VhU215TUlN3Oy04pzU+vvpfZUHmr+t6Nguz460niCCGZS7kYdOWLEN+vgy5/RfX7nuz3",
    "g88PX/j88Fmw77fMEG8xhywT0aVCmpAfIhJQBAKKUEgVi+kiEY0vpAjFNLEshC8KFIfzpZFCKpd39vU33HGnbdzRL7ph/uSC",
    "+pML6pQ71h5KsAbJHko6vucNcSxK4K25zmisKwrrjAA8EqA/bpBT7lDwTiG9ekqvnurrblcph4d6u3u72kCnMaOdMupUQ73y",
    "8uLCkvy84T7FgLx7fHiwu721vrqyOD+vvrqyX94NcKmvKSkuKLtXdDsXqN68Ozk5tzKzMjNuXL96PSMt8+a1a+kp6VeTo6PC",
    "OGx6WKg4KjI0VCaKiQ4PD5NEhEvTUhOvpadcTUsS8LkMOjUpMfZaekp21o38u7cL8u9Y26G07F5xR1tza3ODvLNdNTE2MTao",
    "mhgZGVTIu1prSu9UleRWFGffSIuqKLpeVnCt+t6NyuKM4tyknGsRMVKajBvApXpT/b8NuvyV73efkv1+CPT+1u8SkAK/S1+E",
    "+H/PoHgDQfNhM/1ZrAAwGAw/JjuAyyezeb5CSbAwlCsM5fqQQ3CnzzghiafcUH/g7wgnOcI9nRBexzc2w5HP8wdSgEA7wYGk",
    "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",
    "ykN7osqWp0JhxOy29eUpk+4u1lZJtl1eDVc1C76tr0Yaa/VDe6rHFCdWb0oTKGmjLdSfW9qprOVqtjd+PfrLn35Y/JHffPD+",
    "O0+fP5oN2rtWAWNQNQwso5AstmOJbJncDtHAoQQCcRRuMzMMCTrtrNmIGwyUIBC8QBMkgtEISsEUBwomLJxGg3Fwfeuu4Pyp",
    "3X8nlodb++JuHSgNzY25M1YxZNr23TYargKhuiLe0VbmZKKtTVSp+szRnNnHZ/7uvrc5Xc/3iFQDjhSB2sRQnfL1uZjtg+mu",
    "rjJlbNsvGzdu2raWnOHbibq+Onb2jwPji/XGXIwUtTs5VbigdoVvf/T+r77/9otPPn73D7//9vXXHk9G3U63XixlsrlUNBba",
    "DHjdHofFYmJZGsdRBNHDiIaiYZrBWI4gCAzHUQzBMQSnr1+cATHZKNsGuJ0UknU+UiSTHSzdI9I9NNPHgnldMK/z55Y8qVvR",
    "uq4wIkM1WaSx1j51Jbu0PwtaQyum0K2tCuDPLXnTt+t931c/PP7Xv/7xX378wzef/eqzD958/en+dJA1cwiLaXB4zWrEc0nP",
    "1gZvM9EcCSGgliUxm5nhaL1BQIwiajIxPI+zHIkTMErBCKnHaK3VyXamnnLHUhsaSl0uXSXjBSTbQdoHhta+qzQ0hwpcZewt",
    "jJhIDdztAbs9oDwjog1VKI9k20KshO5Wic6eJ9cSqxOuPhN2a0hpwGW7eHXKV2b44MIoTUELfXEnD+HW/8URupVqLACpTrho",
    "SRsrA9GSzrlzM1bWPXl49u1XH//1z7//y59+OD3Ze/rkQbO1KIFMNpnYjYYjW5sBr8VqIKkF+Aup1WsYFud4SjSw/+3cCALr",
    "QYKEUQykeLXJjm5EUfumxpdQemKLnjlS00aqSKSKuOPqzQyU7eILGaqs7na0kebdWPtepKFxJW/bQgpnVO3LrmZHVDAHB7L6",
    "o/P2V9//6t/+9U9/+dP3v//2w28++9VbL4679UR0x0UgcpbUJCLeeMTpc7NOm2gUSOl0tHTIdmHNDJjZzLLX1YrhEEwCMAnQ",
    "gsrppUanxlIfro6B0kDT3bOWukymjh1eBepTY7yi38nBjbmzc2Srz42pvnanuhKu6pIdNFJRcp7/zej76UZiuXXAlsdY98jS",
    "O7a6Ii/n+/pkCzp6vjk88Q9P/I/eToXy6ubcNTjZ9O9qcPM/epLLmzm5L3c3WJYtzFdJYU/8U7By9/fff/mf/+Wf/5//+7/+",
    "+MffjQbtTz957+Bw2u01iqVcPBHZ2vavu2zSLjAMQzqdRg9rr4/zMQYjJ+FPkTiK6CkapWiUEbW8CdyIovmW05dQmjdvO2O3",
    "3Lt34w08XIEjFTLdEYtDOtcj0n0wVF0LlF6JtpY2csuO+A1vEnRG1Ru5tfyE2W1y0Sr1wacvfvzPX/6X//THv/75dz/+/vPv",
    "v3rvN798kE9uJCLedRtjEpBsMpiMu7c2jW6niWdQ6b6MKJJOp1HkYaO40GJyYckRnNDDJIDSEMHKDVYoWlxNVBSphizfVcYK",
    "QKnLtGfm/oG9POSLfTbXFeszx+xqY3a1UZzC3syN7ZI60ydyfcQZeWUrIy8N6coEj9eUpSGd7xHmzX9wR18Jl1XpDryVBgtd",
    "w6O3U/5dWbRE5LvGzaTW4L1pCLy0kVmNtTUb+SV3ainWAmMdeaix8q8//vB//dtf/o9/+8u//vUP7/zi+WvPLh/cP241ysVC",
    "OhrZCgZ8606rdIeXpjClQgZBAEku8p/jKYpEry+noDgGMRzMi5jZoXd6iUiO8UWgYEazmVJtFQDD5o1cb0GqkRJYHHKJpmq3",
    "pW7sM8mW1ptaCeQVvsxtW+RnnvQ9S/gV09ZLyQ68vWvINzZ++MPX//X//Jf/9Nfvf/zTN3/+w6d/+v0nb7911Wlkgpu24KZt",
    "c8OSy+wUctvbQavNJmCYDsUg0cAaDJTZzIoizjB6hkFwHCAZGCVBPaHCWYAxqbcTtuNHW7WhEC/LmzMq18R2y2C6rm/NxFKX",
    "CWc1g1P7/Mo7vE/U9rTVPWir+Kp589VoGdptKUsT+OCppTbX12ZYuLRcm5jCedCfkrljd03+G76EPFUDGhO2f+SJlVB/atW+",
    "c9OXljGefzBtv+qI3d3t4f6C0l+RbTeVOw3YGr/3X/76h3/7l3/+7utPPnzvl19/8eGLpw/uXxwNes1qOZeIhzb9HoPIQqBG",
    "uj2NIqDkvGgGpxlcuk/K0BhFIpyAGkyk3Y1thgyxAlcbempTc6HPF4ZCbsBnO46dPCtNbrcK91IdbboDZLpgpAbstpGt0ooj",
    "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",
    "bayoyreRwYH38DK8/8B/eBX48N1nn330xrvvPHn0YG82ab71xtXrz6++/uLDjz789cX54XTcm08HlVJ2e2vD73fa7aLdLkoG",
    "32ikeY5kmb+d97DaGZ/fEgiL9W4klMen98OFER3IK2JNbf/CcvZ6JpiDTRt3My2hf85leup4fXkj9dJOfqUwgAXvP+024P6Z",
    "NV7XeZMK3P4PuQEUKsvqQ9v+/dC33374hz988cP3ny8aoY9fXN4fVPOhUSd3MG5Xc/HAht1qog0iQ5GIVqdckd199cZPIb3m",
    "b5cBVQoQ1KmhFQhXGlxAubczvmQqM20gI+udWo6exmeXO+kWNTr3XbwV6p6Y6/vU6IG5OiWa+8zwzNaY8+NLKtG8U93Td07J",
    "wkSxkf1JqqPND/XTS095zG6nsUzDOD31z8435xeB7tyZrNGZJlceMaG8Kt2gM02mf+w8fy26f+mOl9Sh7FptRFf75sPLcLwI",
    "NsaGLz5547uv3vngN08fXc7Gw9qLZ+evv/bwqy8//MVbz0+O57NJfz4d1KrZeHwrHt/a3vZKmzImI2c0sDxHciwhCouP71jn",
    "bA4mEKEr7c1gRtM7dqX74HrildLQ2T0KVuZksLjiS96K11XVKVoa6bdza6kmFG/czfRW47W1cGm5OiN3mxpn4u5OVRuqaFtH",
    "5uHx9vnz/LvvPv3zn7/413/53R9++PyH7966OKmOOvH9cfbhyX45FQttuX1us0FkMBRUyFfu3H71pZ//w+raknSVWKVSqNVK",
    "meoWiClQYdkX4XItdHhsm5y5Kn2m1BfCebA+Xq+NnGfPE/WJdXAm9E646hgYn7OjE2uuBWebQrLKZHtovL4Qr/aRWJ0Tgwvj",
    "/qPtaBm2b92wBl7p7blPHsVOHm315tZim6gP+dbUVOzQe/eD5a7YORSHZ5Zsi93JgrkeUZ3y2Q43PNvI9U3zh5HHV9OvPnvr",
    "3XcePXt8MJ+2RoPqL956+vFHv3rj9SdXl6fPn10d7I8b9Xw2G/P57IGASzpXYLWIVotoNLCiQIvCohBEI+p08a5NOLQrZNp0",
    "Y27tnArpPtg/2W7t+4PFlWhDFSqtbmbuplqqygQZndtzXbQ618Ubd+O1tXRbvZG6vdvUpAeEL7ta2xNLE7Y2cg2Otr744u0f",
    "f/zyxz9985c/f/vVZ0/Pjsone6Wzg8r+oD2ol3ZjgfC2x+kwswyukK/cvPHzn7/8jz9/+R9lK3dVylWNRqVWK1eUN3XIGsLf",
    "C2dtuRba3TfNLjzj0/XOgWO3inX3NwZHgd6huz6xXry5cfLC3dpDGjN9oYM2JmypZ66PHbU5503cjdXU/TNz91TonYmDU0++",
    "x1XHQn1qmJxsnj/dnZ95hweO0ZEjW0eaE2P/wF7uipsxZbgoL/TRzv5679BdHNKNPUNjbj94HK5OHaOLrf83AAD//xdIsOQ=",
};

/* image-rgb-none-20x15-2147483647-raw.data, 900 bytes */
static const char *const b64_rgb_20x15[] = {
    "DRoeCxgcCxcjEh4qDBgkCxcjChYiCxcjCRclBRMhBxIXHysvTVNRbHJwcXB2Li0zCBYXEyEiCxkaDBobChcbCBUZDxsnBBAc",
    "EBwoChYiCxcjDBgkDhwqBxUjDBccm6aqy9HP1NrYzs3UsK+2IjAxCBYXCBYXBxUWFBoaDxUVICYqIyktERcZDxUXDxUVEhgY",
    "DhUTCxIQGh8XusC4zM7FvL61q6elmZWTTVtcDBobDRscCxkaKS8vaW9vxMnOur/EiY+RaW5wICYmW2FhfYOBQEZEnqSc4ebe",
    "qauilZaOsa2rm5eVcH5/GigpChgZCBYX0NHP3d7c3tzbx8XExsTEvry8wL241dLN0tDF0tDF29nM4d/StbKpzMrAUk5DZmJX",
    "eYSGKTU3ER0fDRkb1tfVysvJ0tDPsa+tr6ytop+gmZaRqaahuritw8G2urirqKaZiYZ9paKZZmJXamZbOkZIDhocBxMVBBAS",
    "xMDBtrKzqqanoZ2ejYeLeHF2eXFvhn58npePta6ml5CKgXp0W1hPaWZdZWdSYmRPFiYADR0AFCQAEyMAt7O0lJCRf3t8eHR1",
    "Zl9kY1xhYVpYbGRieXJqeHFpdW1oc2tmcG1kX1xTbW9ajY96jp55kaF8kKB7kaF8sK6rcnFtX11cXFpZW1pWWFdTXVpTXltU",
    "aGJgY11bY11da2Vla25dam1ccHtTnqmBorVtp7pypLdvobRsh4aCaGdjWFZVXFpZYWBcZ2ZiaGVeZGFaY11bYlxaV1FRZ2Fh",
    "dHdmbG9egItjo66GpLdvq752rL93rsF5kpKIZ2ddWFxTW19WbnZdipJ6cnhaaW9RaGhgV1ZPY2Jga2poanFQd35dk6Vpn7B0",
    "oLFvorNxm6xqmKlnv760enpwVlpRW19Wc3til5+Hl55/k5p7iIiAcnJqd3Z0bm1rcHdWh45tipxgladrkaJglKVjkaJgkqNh",
    "09DJiYZ/YmZdY2deeYZYjJlrj51ijpxhztHClJaIdHNvdHNvanNHi5RpmaxnjKBbmqhrmadqkJ5hi5lcxsO8jImCaGtiYmZd",
    "g5Bikp9xjJpfjpxh1djJqq2eamllZ2Zid4BVmKF2kqZhh5tWlaNmlaNmjpxfjJpdw729rqiodnZ0cHBuiplij55nj6FVjJ5S",
    "zdC9t7qncW1sXlpZh45iqbCEmKllmapmmqlqnq1unaxtoK9w",
};

/* image-png-none-50x76-2147483647-raw.data, 86 bytes */
static const char *const b64_png_50x76[] = {
    "iVBORw0KGgoAAAANSUhEUgAAADIAAABMAQMAAAAV2XZDAAAAA1BMVEUrLzc5wwm5AAAADklEQVR4AWOgIRgFowAAAmAAATnE",
    "D6oAAAAASUVORK5CYII=",
};

/* ── Upstream tests ─────────────────────────────────────────────────── */

/* Wisp: upstream's anonymous `test { _ = kitty_windows; }`, which only
 * pulls windows.zig's tests into the run. Those are ported in
 * tests/test_kitty_windows_port.cpp, so there is nothing to assert here
 * beyond the reference itself. */
TEST(kitty_graphics_image, decl_refs) {
    ASSERT_TRUE(kwin::checkPath("C:\\tmp\\image.data", 17) == kwin::Error::none);
}

TEST(kitty_graphics_image, temporary_file_path_must_be_inside_directory) {
#define IN_DIR(d, p) gi::isPathInDir(d, strlen(d), p, strlen(p))
    ASSERT_TRUE(IN_DIR("/tmp", "/tmp/tty-graphics-protocol-image.data"));
    ASSERT_TRUE(IN_DIR("/tmp/", "/tmp/tty-graphics-protocol-image.data"));
    ASSERT_TRUE(IN_DIR("/tmp", "/tmp"));

    ASSERT_TRUE(!IN_DIR("", "/tmp/tty-graphics-protocol-image.data"));
    ASSERT_TRUE(!IN_DIR("/tmp", "/tmpX/tty-graphics-protocol-image.data"));
    ASSERT_TRUE(!IN_DIR("/dev/shm", "/dev/shm-evil/tty-graphics-protocol-image.data"));
    ASSERT_TRUE(!IN_DIR("/custom/tmp", "/custom/tmp-suffix/tty-graphics-protocol-image.data"));
#undef IN_DIR
}

TEST(kitty_graphics_image, shared_memory_names_follow_POSIX_rules) {
#define VALID(n, m) gi::validSharedMemoryName(n, sizeof(n) - 1, m)
    ASSERT_TRUE(VALID("/kitty", 8));
    ASSERT_TRUE(VALID("/1234567", 8));

    ASSERT_TRUE(!VALID("", 8));
    ASSERT_TRUE(!VALID("/", 8));
    ASSERT_TRUE(!VALID("kitty", 8));
    ASSERT_TRUE(!VALID("/kitty/image", 16));
    ASSERT_TRUE(!VALID("/kitty\x00image", 16));
    ASSERT_TRUE(!VALID("/12345678", 8));
#undef VALID
}

TEST(kitty_graphics_image, image_load_rejects_invalid_POSIX_shared_memory_names) {
    const zigstd::Allocator alloc = testAlloc();

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::shared_memory;
    t.width = 1;
    t.height = 1;
    t.image_id = 31;
    Command cmd = makeTransmitZ(alloc, t, "kitty-without-leading-slash");
    CmdGuard cmd_guard(&cmd, alloc);

    Limits limits;
    limits.file = false;
    limits.temporary_file_enabled = false;
    limits.shared_memory = true;

    LoadingImage loading;
    /* Wisp: upstream asserts error.InvalidData, which is what the POSIX
     * shared-memory path returns for a name that fails
     * validSharedMemoryName. Upstream skips this test entirely on
     * Windows (`builtin.target.os.tag == .windows`), where
     * readSharedMemory returns error.UnsupportedMedium before the name
     * is ever looked at. This port targets Windows only, so the test
     * asserts that value instead of being dropped. */
    ASSERT_TRUE(Error::UnsupportedMedium == LoadingImage::init(alloc, &cmd, limits, &loading));
}

TEST(kitty_graphics_image, shared_memory_range_with_offset_and_size) {
    LoadingImage loading;
    loading.image.width = 1;
    loading.image.height = 1;
    loading.image.format = Transmission::Format::rgb;
    loading.quiet = Command::Quiet::no;
    loading.temporary_directory = nullptr;

    LoadingImage::SharedMemoryRange explicit_;
    {
        Transmission t;
        t.offset = 2;
        t.size = 3;
        ASSERT_TRUE(Error::none == loading.sharedMemoryRange(t, 5, &explicit_));
    }
    ASSERT_TRUE((size_t)2 == explicit_.start);
    ASSERT_TRUE((size_t)5 == explicit_.end);

    LoadingImage::SharedMemoryRange implicit_;
    {
        Transmission t;
        t.offset = 2;
        ASSERT_TRUE(Error::none == loading.sharedMemoryRange(t, 5, &implicit_));
    }
    ASSERT_TRUE((size_t)2 == implicit_.start);
    ASSERT_TRUE((size_t)5 == implicit_.end);
}

TEST(kitty_graphics_image, shared_memory_range_rejects_out_of_bounds_offset) {
    LoadingImage loading;
    loading.image.width = 1;
    loading.image.height = 1;
    loading.image.format = Transmission::Format::rgb;
    loading.quiet = Command::Quiet::no;
    loading.temporary_directory = nullptr;

    Transmission t;
    t.offset = 4;
    LoadingImage::SharedMemoryRange range;
    ASSERT_TRUE(Error::InvalidData == loading.sharedMemoryRange(t, 3, &range));
}

TEST(kitty_graphics_image, shared_memory_range_validates_dimensions_before_multiplication) {
    LoadingImage loading;
    loading.image.width = 0xFFFFFFFFu;
    loading.image.height = 0xFFFFFFFFu;
    loading.image.format = Transmission::Format::rgba;
    loading.quiet = Command::Quiet::no;
    loading.temporary_directory = nullptr;

    Transmission t;
    LoadingImage::SharedMemoryRange range;
    ASSERT_TRUE(Error::DimensionsTooLarge == loading.sharedMemoryRange(t, 1, &range));
}

/* This specifically tests we ALLOW invalid RGB data because Kitty
 * documents that this should work. */
TEST(kitty_graphics_image, image_load_with_invalid_RGB_data) {
    const zigstd::Allocator alloc = testAlloc();

    /* <ESC>_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA<ESC>\ */
    Transmission t;
    t.format = Transmission::Format::rgb;
    t.width = 1;
    t.height = 1;
    t.image_id = 31;
    Command cmd = makeTransmitZ(alloc, t, "AAAA");
    CmdGuard cmd_guard(&cmd, alloc);
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, Limits::direct(), &loading));
    LoadingGuard loading_guard(&loading, alloc);
}

TEST(kitty_graphics_image, image_load_with_image_too_wide) {
    const zigstd::Allocator alloc = testAlloc();

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.width = gi::max_dimension + 1;
    t.height = 1;
    t.image_id = 31;
    Command cmd = makeTransmitZ(alloc, t, "AAAA");
    CmdGuard cmd_guard(&cmd, alloc);
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, Limits::direct(), &loading));
    LoadingGuard loading_guard(&loading, alloc);
    Image img;
    ASSERT_TRUE(Error::DimensionsTooLarge == loading.complete(alloc, &img));
}

TEST(kitty_graphics_image, image_load_with_image_too_tall) {
    const zigstd::Allocator alloc = testAlloc();

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.height = gi::max_dimension + 1;
    t.width = 1;
    t.image_id = 31;
    Command cmd = makeTransmitZ(alloc, t, "AAAA");
    CmdGuard cmd_guard(&cmd, alloc);
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, Limits::direct(), &loading));
    LoadingGuard loading_guard(&loading, alloc);
    Image img;
    ASSERT_TRUE(Error::DimensionsTooLarge == loading.complete(alloc, &img));
}

TEST(kitty_graphics_image, image_load_rgb_zlib_compressed_direct) {
    const zigstd::Allocator alloc = testAlloc();
    Vector vec(alloc, B64_PARTS(b64_zlib_128x96));

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::direct;
    t.compression = Transmission::Compression::zlib_deflate;
    t.height = 96;
    t.width = 128;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, (const char *)vec.data, vec.len);
    CmdGuard cmd_guard(&cmd, alloc);
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, Limits::direct(), &loading));
    LoadingGuard loading_guard(&loading, alloc);
    Image img;
    ASSERT_TRUE(Error::none == loading.complete(alloc, &img));
    ImageGuard img_guard(&img, alloc);

    /* should be decompressed */
    ASSERT_TRUE(img.compression == Transmission::Compression::none);
}

TEST(kitty_graphics_image, image_load_rgb_not_compressed_direct) {
    const zigstd::Allocator alloc = testAlloc();
    Vector vec(alloc, B64_PARTS(b64_rgb_20x15));

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::direct;
    t.compression = Transmission::Compression::none;
    t.width = 20;
    t.height = 15;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, (const char *)vec.data, vec.len);
    CmdGuard cmd_guard(&cmd, alloc);
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, Limits::direct(), &loading));
    LoadingGuard loading_guard(&loading, alloc);
    Image img;
    ASSERT_TRUE(Error::none == loading.complete(alloc, &img));
    ImageGuard img_guard(&img, alloc);

    /* should be decompressed */
    ASSERT_TRUE(img.compression == Transmission::Compression::none);
}

TEST(kitty_graphics_image, image_load_rgb_zlib_compressed_direct_chunked) {
    const zigstd::Allocator alloc = testAlloc();
    Vector vec(alloc, B64_PARTS(b64_zlib_128x96));

    /* Setup our initial chunk */
    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::direct;
    t.compression = Transmission::Compression::zlib_deflate;
    t.height = 96;
    t.width = 128;
    t.image_id = 31;
    t.more_chunks = true;
    Command cmd = makeTransmit(alloc, t, (const char *)vec.data, 1024);
    CmdGuard cmd_guard(&cmd, alloc);
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, Limits::direct(), &loading));
    LoadingGuard loading_guard(&loading, alloc);

    /* Read our remaining chunks */
    {
        size_t pos = 1024;
        while (pos < vec.len) {
            size_t size = vec.len - pos;
            if (size > 1024) size = 1024;
            ASSERT_TRUE(Error::none == loading.addData(alloc, vec.data + pos, size));
            pos += size;
            if (size < 1024) break;
        }
    }

    /* Complete */
    Image img;
    ASSERT_TRUE(Error::none == loading.complete(alloc, &img));
    ImageGuard img_guard(&img, alloc);
    ASSERT_TRUE(img.compression == Transmission::Compression::none);
}

TEST(kitty_graphics_image, image_load_rgb_zlib_compressed_direct_chunked_with_zero_initial_chunk) {
    const zigstd::Allocator alloc = testAlloc();
    Vector vec(alloc, B64_PARTS(b64_zlib_128x96));

    /* Setup our initial chunk */
    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::direct;
    t.compression = Transmission::Compression::zlib_deflate;
    t.height = 96;
    t.width = 128;
    t.image_id = 31;
    t.more_chunks = true;
    Command cmd = makeTransmit(alloc, t, nullptr, 0);
    CmdGuard cmd_guard(&cmd, alloc);
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, Limits::direct(), &loading));
    LoadingGuard loading_guard(&loading, alloc);

    /* Read our remaining chunks */
    {
        size_t pos = 0;
        while (pos < vec.len) {
            size_t size = vec.len - pos;
            if (size > 1024) size = 1024;
            ASSERT_TRUE(Error::none == loading.addData(alloc, vec.data + pos, size));
            pos += size;
            if (size < 1024) break;
        }
    }

    /* Complete */
    Image img;
    ASSERT_TRUE(Error::none == loading.complete(alloc, &img));
    ImageGuard img_guard(&img, alloc);
    ASSERT_TRUE(img.compression == Transmission::Compression::none);
}

TEST(kitty_graphics_image, image_load_temporary_file_without_correct_path) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    Vector vec(alloc, B64_PARTS(b64_rgb_20x15));
    tmp_dir.writeFile("image.data", vec.data, vec.len);

    char buf[1200];
    char sub[1200];
    tmp_dir.join("image.data", sub, sizeof sub);
    const size_t path_len = realPathOf(alloc, sub, buf, sizeof buf);
    ASSERT_TRUE(path_len > 0);

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::temporary_file;
    t.compression = Transmission::Compression::none;
    t.width = 20;
    t.height = 15;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, buf, path_len);
    CmdGuard cmd_guard(&cmd, alloc);

    char dir_path_buf[1200];
    const size_t dir_len = realPathOf(alloc, tmp_dir.path, dir_path_buf, sizeof dir_path_buf);
    ASSERT_TRUE(dir_len > 0);
    LoadingImage loading;
    ASSERT_TRUE(Error::TemporaryFileNotNamedCorrectly ==
                LoadingImage::init(alloc, &cmd, Limits::allWithTempDir(dir_path_buf, dir_len),
                                   &loading));

    /* Temporary file should still be there */
    ASSERT_TRUE(tmp_dir.access(buf));
}

TEST(kitty_graphics_image, image_load_temporary_file_outside_directory_prefix_is_rejected) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    tmp_dir.createDir("temp");
    tmp_dir.createDir("temp-suffix");

    const char *filename = "tty-graphics-protocol-image.data";
    Vector vec(alloc, B64_PARTS(b64_rgb_20x15));
    tmp_dir.writeFile("temp-suffix\\tty-graphics-protocol-image.data", vec.data, vec.len);

    char trusted_dir[1200];
    char trusted_path_buf[1200];
    tmp_dir.join("temp", trusted_dir, sizeof trusted_dir);
    const size_t trusted_len = realPathOf(alloc, trusted_dir, trusted_path_buf,
                                          sizeof trusted_path_buf);
    ASSERT_TRUE(trusted_len > 0);

    char outside_file[1200];
    char outside_path_buf[1200];
    tmp_dir.join("temp-suffix\\tty-graphics-protocol-image.data", outside_file,
                 sizeof outside_file);
    const size_t outside_len = realPathOf(alloc, outside_file, outside_path_buf,
                                          sizeof outside_path_buf);
    ASSERT_TRUE(outside_len > 0);

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::temporary_file;
    t.compression = Transmission::Compression::none;
    t.width = 20;
    t.height = 15;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, outside_path_buf, outside_len);
    CmdGuard cmd_guard(&cmd, alloc);
    LoadingImage loading;
    ASSERT_TRUE(Error::TemporaryFileNotInTempDir ==
                LoadingImage::init(alloc, &cmd,
                                   Limits::allWithTempDir(trusted_path_buf, trusted_len),
                                   &loading));

    /* Rejection must happen before temporary-file cleanup is armed. */
    ASSERT_TRUE(tmp_dir.access(outside_file));
    (void)filename;
}

TEST(kitty_graphics_image, image_load_rgb_not_compressed_temporary_file) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    Vector vec(alloc, B64_PARTS(b64_rgb_20x15));
    tmp_dir.writeFile("tty-graphics-protocol-image.data", vec.data, vec.len);

    char sub[1200];
    char buf[1200];
    tmp_dir.join("tty-graphics-protocol-image.data", sub, sizeof sub);
    const size_t path_len = realPathOf(alloc, sub, buf, sizeof buf);
    ASSERT_TRUE(path_len > 0);

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::temporary_file;
    t.compression = Transmission::Compression::none;
    t.width = 20;
    t.height = 15;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, buf, path_len);
    CmdGuard cmd_guard(&cmd, alloc);

    char dir_path_buf[1200];
    const size_t dir_len = realPathOf(alloc, tmp_dir.path, dir_path_buf, sizeof dir_path_buf);
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(
                                   alloc, &cmd, Limits::allWithTempDir(dir_path_buf, dir_len),
                                   &loading));
    LoadingGuard loading_guard(&loading, alloc);
    Image img;
    ASSERT_TRUE(Error::none == loading.complete(alloc, &img));
    ImageGuard img_guard(&img, alloc);
    ASSERT_TRUE(img.compression == Transmission::Compression::none);

    /* Temporary file should be gone */
    ASSERT_TRUE(!tmp_dir.access(buf));
}

TEST(kitty_graphics_image, image_load_rgb_not_compressed_regular_file) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    Vector vec(alloc, B64_PARTS(b64_rgb_20x15));
    tmp_dir.writeFile("image.data", vec.data, vec.len);

    char sub[1200];
    char buf[1200];
    tmp_dir.join("image.data", sub, sizeof sub);
    const size_t path_len = realPathOf(alloc, sub, buf, sizeof buf);
    ASSERT_TRUE(path_len > 0);

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::file;
    t.compression = Transmission::Compression::none;
    t.width = 20;
    t.height = 15;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, buf, path_len);
    CmdGuard cmd_guard(&cmd, alloc);

    char dir_path_buf[1200];
    const size_t dir_len = realPathOf(alloc, tmp_dir.path, dir_path_buf, sizeof dir_path_buf);
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(
                                   alloc, &cmd, Limits::allWithTempDir(dir_path_buf, dir_len),
                                   &loading));
    LoadingGuard loading_guard(&loading, alloc);
    Image img;
    ASSERT_TRUE(Error::none == loading.complete(alloc, &img));
    ImageGuard img_guard(&img, alloc);
    ASSERT_TRUE(img.compression == Transmission::Compression::none);
    ASSERT_TRUE(tmp_dir.access(buf));
}

TEST(kitty_graphics_image, image_load_regular_file_size_reads_exactly_requested_bytes) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    static const uint8_t file_bytes[] = {1, 2, 3, 4, 5, 6};
    tmp_dir.writeFile("image.data", file_bytes, sizeof file_bytes);

    char sub[1200];
    char path_buf[1200];
    tmp_dir.join("image.data", sub, sizeof sub);
    const size_t path_len = realPathOf(alloc, sub, path_buf, sizeof path_buf);
    ASSERT_TRUE(path_len > 0);

    struct Case {
        uint32_t offset;
        uint8_t expected[3];
    };
    static const Case cases[] = {
        {0, {1, 2, 3}},
        {3, {4, 5, 6}},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i += 1) {
        const Case &c = cases[i];
        Transmission t;
        t.format = Transmission::Format::rgb;
        t.medium = Transmission::Medium::file;
        t.width = 1;
        t.height = 1;
        t.size = 3;
        t.offset = c.offset;
        t.image_id = 31;
        Command cmd = makeTransmit(alloc, t, path_buf, path_len);
        CmdGuard cmd_guard(&cmd, alloc);

        Limits limits;
        limits.file = true;
        limits.temporary_file_enabled = false;
        limits.shared_memory = false;
        LoadingImage loading;
        ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, limits, &loading));
        LoadingGuard loading_guard(&loading, alloc);
        Image img;
        ASSERT_TRUE(Error::none == loading.complete(alloc, &img));
        ImageGuard img_guard(&img, alloc);

        ASSERT_TRUE(img.data.complete_len == 3);
        ASSERT_TRUE(memcmp(c.expected, img.data.complete, 3) == 0);
    }
}

TEST(kitty_graphics_image, image_load_regular_file_size_rejects_short_data) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    static const uint8_t file_bytes[] = {1, 2};
    tmp_dir.writeFile("image.data", file_bytes, sizeof file_bytes);

    char sub[1200];
    char path_buf[1200];
    tmp_dir.join("image.data", sub, sizeof sub);
    const size_t path_len = realPathOf(alloc, sub, path_buf, sizeof path_buf);
    ASSERT_TRUE(path_len > 0);

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::file;
    t.width = 1;
    t.height = 1;
    t.size = 3;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, path_buf, path_len);
    CmdGuard cmd_guard(&cmd, alloc);

    Limits limits;
    limits.file = true;
    limits.temporary_file_enabled = false;
    limits.shared_memory = false;
    LoadingImage loading;
    ASSERT_TRUE(Error::InvalidData == LoadingImage::init(alloc, &cmd, limits, &loading));
}

TEST(kitty_graphics_image, image_load_rgb_not_compressed_relative_regular_file) {
    const zigstd::Allocator alloc = testAlloc();

    /* Wisp: upstream builds the relative path from Zig's
     * `.zig-cache/tmp/<sub_path>`, which is where testing.tmpDir places
     * its directory relative to the cwd. The port's TmpDir lives under
     * %TEMP%, so the relative case is exercised with a directory created
     * under the current working directory instead. The assertion — a
     * relative path loads like any other — is upstream's. */
    static const char *const dir_name = "wisp-kitty-gfx-rel";
    static const char *const rel_path = "wisp-kitty-gfx-rel/image.data";
    _mkdir(dir_name);
    {
        Vector vec(alloc, B64_PARTS(b64_rgb_20x15));
        FILE *f = fopen(rel_path, "wb");
        ASSERT_TRUE(f != nullptr);
        fwrite(vec.data, 1, vec.len, f);
        fclose(f);
    }

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::file;
    t.compression = Transmission::Compression::none;
    t.width = 20;
    t.height = 15;
    t.image_id = 31;
    Command cmd = makeTransmitZ(alloc, t, rel_path);
    CmdGuard cmd_guard(&cmd, alloc);

    Limits limits;
    limits.file = true;
    limits.temporary_file_enabled = false;
    limits.shared_memory = false;
    LoadingImage loading;
    const Error init_err = LoadingImage::init(alloc, &cmd, limits, &loading);
    if (init_err == Error::none) {
        LoadingGuard loading_guard(&loading, alloc);
        Image img;
        const Error complete_err = loading.complete(alloc, &img);
        if (complete_err == Error::none) {
            ImageGuard img_guard(&img, alloc);
            ASSERT_TRUE(img.compression == Transmission::Compression::none);
        } else {
            remove(rel_path);
            _rmdir(dir_name);
            ASSERT_TRUE(complete_err == Error::none);
        }
    }
    remove(rel_path);
    _rmdir(dir_name);
    ASSERT_TRUE(init_err == Error::none);
}

TEST(kitty_graphics_image, image_load_blocklist_applies_to_opened_file_after_symlink_swap) {
    /* Wisp: upstream returns error.SkipZigTest on Windows
     * (`if (builtin.os.tag == .windows) return error.SkipZigTest`). The
     * POSIX blocklist it exercises (/proc, /sys, /dev) is not reachable
     * in this port, so there is nothing to assert. */
}

TEST(kitty_graphics_image, image_load_windows_UNC_path_is_rejected_before_open) {
    const zigstd::Allocator alloc = testAlloc();

    /* Opening a UNC path would resolve the host name and authenticate to
     * it over SMB, so the rejection has to happen on the raw path. The
     * host name is in the reserved .invalid TLD so it can never resolve. */
    static const char *const paths[] = {
        "\\\\nonexistent-host.invalid\\share\\tty-graphics-protocol-image.data",
        "//nonexistent-host.invalid/share/tty-graphics-protocol-image.data",
        "\\\\?\\UNC\\nonexistent-host.invalid\\share\\image.data",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i += 1) {
        Transmission t;
        t.format = Transmission::Format::rgb;
        t.medium = Transmission::Medium::file;
        t.compression = Transmission::Compression::none;
        t.width = 20;
        t.height = 15;
        t.image_id = 31;
        Command cmd = makeTransmitZ(alloc, t, paths[i]);
        CmdGuard cmd_guard(&cmd, alloc);

        Limits limits;
        limits.file = true;
        limits.temporary_file_enabled = false;
        limits.shared_memory = false;
        LoadingImage loading;
        ASSERT_TRUE(Error::InvalidData == LoadingImage::init(alloc, &cmd, limits, &loading));
    }
}

TEST(kitty_graphics_image, image_load_windows_device_namespace_paths_are_rejected_before_open) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    const char *filename = "tty-graphics-protocol-image.data";
    Vector vec(alloc, B64_PARTS(b64_rgb_20x15));
    tmp_dir.writeFile(filename, vec.data, vec.len);

    char dir_buf[1200];
    const size_t dir_len = realPathOf(alloc, tmp_dir.path, dir_buf, sizeof dir_buf);
    ASSERT_TRUE(dir_len > 0);
    char sub[1200];
    char path_buf[1200];
    tmp_dir.join(filename, sub, sizeof sub);
    const size_t real_len = realPathOf(alloc, sub, path_buf, sizeof path_buf);
    ASSERT_TRUE(real_len > 0);

    /* The verbatim, local device and NT namespace spellings all resolve
     * to the same real file, so rejecting them proves the check runs on
     * the raw path rather than on the canonical one. */
    static const char *const prefixes[] = {"\\\\?\\", "\\\\.\\", "\\??\\"};
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p += 1) {
        char path[1300];
        snprintf(path, sizeof path, "%s%.*s", prefixes[p], (int)real_len, path_buf);

        static const Transmission::Medium mediums[] = {Transmission::Medium::file,
                                                       Transmission::Medium::temporary_file};
        for (size_t m = 0; m < sizeof(mediums) / sizeof(mediums[0]); m += 1) {
            Transmission t;
            t.format = Transmission::Format::rgb;
            t.medium = mediums[m];
            t.compression = Transmission::Compression::none;
            t.width = 20;
            t.height = 15;
            t.image_id = 31;
            Command cmd = makeTransmitZ(alloc, t, path);
            CmdGuard cmd_guard(&cmd, alloc);
            LoadingImage loading;
            ASSERT_TRUE(Error::InvalidData ==
                        LoadingImage::init(alloc, &cmd, Limits::allWithTempDir(dir_buf, dir_len),
                                           &loading));
        }
    }

    /* A named pipe reached through the local device namespace. */
    {
        Transmission t;
        t.format = Transmission::Format::rgb;
        t.medium = Transmission::Medium::file;
        t.compression = Transmission::Compression::none;
        t.width = 20;
        t.height = 15;
        t.image_id = 31;
        Command cmd = makeTransmitZ(alloc, t, "\\\\.\\pipe\\ghostty-kitty-graphics-test");
        CmdGuard cmd_guard(&cmd, alloc);
        LoadingImage loading;
        ASSERT_TRUE(Error::InvalidData ==
                    LoadingImage::init(alloc, &cmd, Limits::allWithTempDir(dir_buf, dir_len),
                                       &loading));
    }

    /* Nothing above reached the temporary file deletion. */
    ASSERT_TRUE(tmp_dir.accessSub(filename));
}

TEST(kitty_graphics_image, image_load_windows_reserved_device_names_are_rejected_before_open) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    char dir_buf[1200];
    const size_t dir_len = realPathOf(alloc, tmp_dir.path, dir_buf, sizeof dir_buf);
    ASSERT_TRUE(dir_len > 0);

    static const char *const names[] = {"CON", "NUL.data", "com1", "LPT9.png", "AUX", "PRN."};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i += 1) {
        char path[1300];
        snprintf(path, sizeof path, "%.*s\\%s", (int)dir_len, dir_buf, names[i]);

        Transmission t;
        t.format = Transmission::Format::rgb;
        t.medium = Transmission::Medium::file;
        t.compression = Transmission::Compression::none;
        t.width = 20;
        t.height = 15;
        t.image_id = 31;
        Command cmd = makeTransmitZ(alloc, t, path);
        CmdGuard cmd_guard(&cmd, alloc);

        Limits limits;
        limits.file = true;
        limits.temporary_file_enabled = false;
        limits.shared_memory = false;
        LoadingImage loading;
        ASSERT_TRUE(Error::InvalidData == LoadingImage::init(alloc, &cmd, limits, &loading));
    }
}

TEST(kitty_graphics_image,
     image_load_windows_local_file_accepted_in_forward_slash_and_upper_case_spellings) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    const char *filename = "image.data";
    Vector vec(alloc, B64_PARTS(b64_rgb_20x15));
    tmp_dir.writeFile(filename, vec.data, vec.len);

    char sub[1200];
    char path_buf[1200];
    tmp_dir.join(filename, sub, sizeof sub);
    const size_t real_len = realPathOf(alloc, sub, path_buf, sizeof path_buf);
    ASSERT_TRUE(real_len > 0);

    /* Both spellings open the same file; the canonical path the checks
     * see is the backslash, on-disk case form either way. */
    char forward[1200];
    memcpy(forward, path_buf, real_len);
    forward[real_len] = 0;
    for (size_t i = 0; i < real_len; i += 1)
        if (forward[i] == '\\') forward[i] = '/';
    char upper[1200];
    memcpy(upper, path_buf, real_len);
    upper[real_len] = 0;
    for (size_t i = 0; i < real_len; i += 1)
        if (upper[i] >= 'a' && upper[i] <= 'z') upper[i] = (char)(upper[i] - 32);

    const char *const spellings[] = {forward, upper};
    for (size_t i = 0; i < 2; i += 1) {
        Transmission t;
        t.format = Transmission::Format::rgb;
        t.medium = Transmission::Medium::file;
        t.compression = Transmission::Compression::none;
        t.width = 20;
        t.height = 15;
        t.image_id = 31;
        Command cmd = makeTransmitZ(alloc, t, spellings[i]);
        CmdGuard cmd_guard(&cmd, alloc);

        Limits limits;
        limits.file = true;
        limits.temporary_file_enabled = false;
        limits.shared_memory = false;
        LoadingImage loading;
        ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, limits, &loading));
        LoadingGuard loading_guard(&loading, alloc);
        Image img;
        ASSERT_TRUE(Error::none == loading.complete(alloc, &img));
        ImageGuard img_guard(&img, alloc);
        ASSERT_TRUE(img.compression == Transmission::Compression::none);
    }

    ASSERT_TRUE(tmp_dir.accessSub(filename));
}

TEST(kitty_graphics_image, image_load_windows_temporary_file_with_differently_spelled_directory) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    const char *filename = "tty-graphics-protocol-image.data";
    Vector vec(alloc, B64_PARTS(b64_rgb_20x15));
    tmp_dir.writeFile(filename, vec.data, vec.len);

    char dir_buf[1200];
    const size_t dir_len = realPathOf(alloc, tmp_dir.path, dir_buf, sizeof dir_buf);
    ASSERT_TRUE(dir_len > 0);
    char sub[1200];
    char path_buf[1200];
    tmp_dir.join(filename, sub, sizeof sub);
    const size_t real_len = realPathOf(alloc, sub, path_buf, sizeof path_buf);
    ASSERT_TRUE(real_len > 0);

    /* Hosts hand over GetTempPath output, which regularly differs from
     * the canonical path in case (`C:\WINDOWS\TEMP\`), so the directory
     * prefix check only passes through the real-path fallback. */
    char dir_upper[1200];
    memcpy(dir_upper, dir_buf, dir_len);
    dir_upper[dir_len] = 0;
    for (size_t i = 0; i < dir_len; i += 1)
        if (dir_upper[i] >= 'a' && dir_upper[i] <= 'z') dir_upper[i] = (char)(dir_upper[i] - 32);
    char path_upper[1200];
    memcpy(path_upper, path_buf, real_len);
    path_upper[real_len] = 0;
    for (size_t i = 0; i < real_len; i += 1)
        if (path_upper[i] >= 'a' && path_upper[i] <= 'z')
            path_upper[i] = (char)(path_upper[i] - 32);
    ASSERT_TRUE(!gi::isPathInDir(dir_upper, dir_len, path_buf, real_len) ||
                (dir_len == dir_len && memcmp(dir_upper, dir_buf, dir_len) == 0));

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::temporary_file;
    t.compression = Transmission::Compression::none;
    t.width = 20;
    t.height = 15;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, path_upper, real_len);
    CmdGuard cmd_guard(&cmd, alloc);
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(
                                   alloc, &cmd, Limits::allWithTempDir(dir_upper, dir_len),
                                   &loading));
    LoadingGuard loading_guard(&loading, alloc);
    Image img;
    ASSERT_TRUE(Error::none == loading.complete(alloc, &img));
    ImageGuard img_guard(&img, alloc);
    ASSERT_TRUE(img.compression == Transmission::Compression::none);

    /* Temporary file should be gone */
    ASSERT_TRUE(!tmp_dir.accessSub(filename));
}

TEST(kitty_graphics_image,
     image_load_windows_canonical_path_check_accepts_a_local_file_opened_through_a_device_spelling) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    const char *filename = "image.data";
    tmp_dir.writeFile(filename, "safe", 4);

    char sub[1200];
    char path_buf[1200];
    tmp_dir.join(filename, sub, sizeof sub);
    const size_t real_len = realPathOf(alloc, sub, path_buf, sizeof path_buf);
    ASSERT_TRUE(real_len > 0);

    /* readFile refuses this spelling before the open, but the post-open
     * check works from the canonical path and so does not care how the
     * file was reached: `\\.\C:\...` canonicalizes back to `C:\...`. */
    char device_path[1300];
    snprintf(device_path, sizeof device_path, "\\\\.\\%.*s", (int)real_len, path_buf);
    const gi::file_io::Handle file =
        gi::file_io::openFile(alloc, device_path, strlen(device_path));
    ASSERT_TRUE(file != gi::file_io::invalid_handle);

    uint8_t *canon_buf = alloc.alloc(gi::max_path_bytes);
    const char *canon = nullptr;
    size_t canon_len = 0;
    const Error err = LoadingImage::validatedFilePath(alloc, file, (char *)canon_buf,
                                                      gi::max_path_bytes, &canon, &canon_len);
    gi::file_io::closeFile(file);
    const bool ok = err == Error::none && canon_len == real_len &&
                    memcmp(canon, path_buf, real_len) == 0;
    alloc.free(canon_buf, gi::max_path_bytes);
    ASSERT_TRUE(ok);
}

TEST(kitty_graphics_image,
     image_load_windows_canonical_path_check_rejects_a_file_reached_through_a_UNC_share) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    const char *filename = "image.data";
    tmp_dir.writeFile(filename, "safe", 4);

    char sub[1200];
    char path_buf[1200];
    tmp_dir.join(filename, sub, sizeof sub);
    const size_t real_len = realPathOf(alloc, sub, path_buf, sizeof path_buf);
    ASSERT_TRUE(real_len > 0);
    ASSERT_TRUE(kwin::isDriveAbsolute(path_buf, real_len));

    /* Reach the same local file through the loopback administrative
     * share, which is how a junction or symlink into a share would look
     * to the post-open check. The share needs the server service and an
     * administrative token, so the test is skipped when the open fails. */
    char unc_path[1300];
    snprintf(unc_path, sizeof unc_path, "\\\\localhost\\%c$\\%.*s", path_buf[0],
             (int)(real_len - 3), path_buf + 3);
    const gi::file_io::Handle file = gi::file_io::openFile(alloc, unc_path, strlen(unc_path));
    if (file == gi::file_io::invalid_handle) return; /* Wisp: error.SkipZigTest */

    uint8_t *canon_buf = alloc.alloc(gi::max_path_bytes);
    const char *canon = nullptr;
    size_t canon_len = 0;
    const Error err = LoadingImage::validatedFilePath(alloc, file, (char *)canon_buf,
                                                      gi::max_path_bytes, &canon, &canon_len);
    gi::file_io::closeFile(file);
    alloc.free(canon_buf, gi::max_path_bytes);
    ASSERT_TRUE(Error::NotDriveAbsolute == err);
}

TEST(kitty_graphics_image, image_load_png_not_compressed_regular_file) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    Vector vec(alloc, B64_PARTS(b64_png_50x76));
    tmp_dir.writeFile("tty-graphics-protocol-image.data", vec.data, vec.len);

    char sub[1200];
    char buf[1200];
    tmp_dir.join("tty-graphics-protocol-image.data", sub, sizeof sub);
    const size_t path_len = realPathOf(alloc, sub, buf, sizeof buf);
    ASSERT_TRUE(path_len > 0);

    Transmission t;
    t.format = Transmission::Format::png;
    t.medium = Transmission::Medium::file;
    t.compression = Transmission::Compression::none;
    t.width = 0;
    t.height = 0;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, buf, path_len);
    CmdGuard cmd_guard(&cmd, alloc);

    char dir_path_buf[1200];
    const size_t dir_len = realPathOf(alloc, tmp_dir.path, dir_path_buf, sizeof dir_path_buf);
    LoadingImage loading;
    const Error err = LoadingImage::init(alloc, &cmd,
                                         Limits::allWithTempDir(dir_path_buf, dir_len), &loading);

    /* Wisp: upstream skips this test when `sys.decode_png == null`, then
     * asserts `loading.complete` succeeds with `img.compression == .none`
     * and `img.format == .rgba`. The port leaves the decoder hook null
     * (see src/terminal/sys.hpp), which is upstream's own `.lib`
     * configuration, so LoadingImage.init fails up front with
     * error.UnsupportedMedium — the "save a lot of memory/effort
     * buffering the data but failing up front" branch. The file-still-
     * present assertion below is upstream's. */
    ASSERT_TRUE(Error::UnsupportedMedium == err);
    ASSERT_TRUE(tmp_dir.access(buf));
}

TEST(kitty_graphics_image, image_load_png_rejects_oversized_decoder_allocation) {
    struct S {
        static sysns::DecodeError decode(zigstd::Allocator alloc, const uint8_t *data,
                                         size_t data_len, sysns::Image *out) {
            (void)data;
            (void)data_len;
            uint8_t *buf = alloc.allocT<uint8_t>(gi::max_size + 1);
            if (buf == nullptr) return sysns::DecodeError::OutOfMemory;
            out->width = 1;
            out->height = 1;
            out->data = buf;
            out->data_len = gi::max_size + 1;
            return sysns::DecodeError::none;
        }
    };

    const sysns::DecodePngFn original_decode_png = sysns::decode_png();
    sysns::decode_png() = &S::decode;

    /* Fail any allocation which reaches the underlying allocator. The size
     * limiter should reject the decoder's request before it gets that far. */
    zigstd::FailingAllocator failing(zigstd::testing_allocator(), 0);
    const zigstd::Allocator alloc = failing.allocator();

    LoadingImage loading;
    loading.image.format = Transmission::Format::png;
    loading.quiet = Command::Quiet::no;
    loading.temporary_directory = nullptr;

    Image img;
    const Error err = loading.complete(alloc, &img);
    loading.deinit(alloc);
    sysns::decode_png() = original_decode_png;

    ASSERT_TRUE(Error::InvalidData == err);
    ASSERT_TRUE(!failing.has_induced_failure);
}

TEST(kitty_graphics_image, image_load_png_rejects_oversized_Wuffs_image_before_allocation) {
    const zigstd::Allocator alloc = testAlloc();
    Vector vec(alloc, B64_PARTS(b64_png_50x76));

    /* Turn the small test PNG into a 32768x32767 image. Its decoded RGBA
     * size is just under Wuffs' 4 GiB package limit but over Kitty's 400 MiB
     * limit, which previously allowed the large allocation to happen first.
     *
     * Wisp: upstream also rewrites the IHDR CRC32 at data[29..33] so that
     * Wuffs accepts the mutated header. The port has no Crc32 and no
     * decoder reads these bytes with the hook null, so that step is
     * omitted. */
    ASSERT_TRUE(vec.len >= 33);
    vec.data[16] = 0x00;
    vec.data[17] = 0x00;
    vec.data[18] = 0x80;
    vec.data[19] = 0x00;
    vec.data[20] = 0x00;
    vec.data[21] = 0x00;
    vec.data[22] = 0x7F;
    vec.data[23] = 0xFF;

    Transmission t;
    t.format = Transmission::Format::png;
    t.medium = Transmission::Medium::direct;
    Command cmd = makeTransmit(alloc, t, (const char *)vec.data, vec.len);
    CmdGuard cmd_guard(&cmd, alloc);
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, Limits::direct(), &loading));
    LoadingGuard loading_guard(&loading, alloc);

    Image img;
    /* Wisp: upstream skips when `sys.decode_png == null` and otherwise
     * asserts error.InvalidData, raised by the LimitedAllocator rejecting
     * the decoder's oversized buffer. With the hook left null, decodePng
     * returns error.UnsupportedFormat before any decoding happens. */
    ASSERT_TRUE(Error::UnsupportedFormat == loading.complete(alloc, &img));
}

TEST(kitty_graphics_image, limits_direct_medium_always_allowed) {
    const zigstd::Allocator alloc = testAlloc();

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::direct;
    t.width = 1;
    t.height = 1;
    t.image_id = 31;
    Command cmd = makeTransmitZ(alloc, t, "AAAA");
    CmdGuard cmd_guard(&cmd, alloc);

    /* Direct medium should work even with the most restrictive limits */
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, Limits::direct(), &loading));
    LoadingGuard loading_guard(&loading, alloc);
}

TEST(kitty_graphics_image, limits_file_medium_blocked_by_limits) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    Vector vec(alloc, B64_PARTS(b64_rgb_20x15));
    tmp_dir.writeFile("image.data", vec.data, vec.len);

    char sub[1200];
    char buf[1200];
    tmp_dir.join("image.data", sub, sizeof sub);
    const size_t path_len = realPathOf(alloc, sub, buf, sizeof buf);
    ASSERT_TRUE(path_len > 0);

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::file;
    t.compression = Transmission::Compression::none;
    t.width = 20;
    t.height = 15;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, buf, path_len);
    CmdGuard cmd_guard(&cmd, alloc);
    LoadingImage loading;
    ASSERT_TRUE(Error::UnsupportedMedium ==
                LoadingImage::init(alloc, &cmd, Limits::direct(), &loading));
}

TEST(kitty_graphics_image, limits_file_medium_allowed_by_limits) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    Vector vec(alloc, B64_PARTS(b64_rgb_20x15));
    tmp_dir.writeFile("image.data", vec.data, vec.len);

    char sub[1200];
    char buf[1200];
    tmp_dir.join("image.data", sub, sizeof sub);
    const size_t path_len = realPathOf(alloc, sub, buf, sizeof buf);
    ASSERT_TRUE(path_len > 0);

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::file;
    t.compression = Transmission::Compression::none;
    t.width = 20;
    t.height = 15;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, buf, path_len);
    CmdGuard cmd_guard(&cmd, alloc);

    Limits limits;
    limits.file = true;
    limits.temporary_file_enabled = false;
    limits.shared_memory = false;
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, limits, &loading));
    LoadingGuard loading_guard(&loading, alloc);
}

TEST(kitty_graphics_image, limits_temporary_file_medium_blocked_by_limits) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    Vector vec(alloc, B64_PARTS(b64_rgb_20x15));
    tmp_dir.writeFile("tty-graphics-protocol-image.data", vec.data, vec.len);

    char sub[1200];
    char buf[1200];
    tmp_dir.join("tty-graphics-protocol-image.data", sub, sizeof sub);
    const size_t path_len = realPathOf(alloc, sub, buf, sizeof buf);
    ASSERT_TRUE(path_len > 0);

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::temporary_file;
    t.compression = Transmission::Compression::none;
    t.width = 20;
    t.height = 15;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, buf, path_len);
    CmdGuard cmd_guard(&cmd, alloc);

    Limits limits;
    limits.file = true;
    limits.temporary_file_enabled = false;
    limits.shared_memory = true;
    LoadingImage loading;
    ASSERT_TRUE(Error::UnsupportedMedium == LoadingImage::init(alloc, &cmd, limits, &loading));

    /* File should still exist since we blocked before reading */
    ASSERT_TRUE(tmp_dir.accessSub("tty-graphics-protocol-image.data"));
}

TEST(kitty_graphics_image, limits_temporary_file_medium_allowed_by_limits) {
    const zigstd::Allocator alloc = testAlloc();

    TmpDir tmp_dir;
    Vector vec(alloc, B64_PARTS(b64_rgb_20x15));
    tmp_dir.writeFile("tty-graphics-protocol-image.data", vec.data, vec.len);

    char sub[1200];
    char buf[1200];
    tmp_dir.join("tty-graphics-protocol-image.data", sub, sizeof sub);
    const size_t path_len = realPathOf(alloc, sub, buf, sizeof buf);
    ASSERT_TRUE(path_len > 0);

    Transmission t;
    t.format = Transmission::Format::rgb;
    t.medium = Transmission::Medium::temporary_file;
    t.compression = Transmission::Compression::none;
    t.width = 20;
    t.height = 15;
    t.image_id = 31;
    Command cmd = makeTransmit(alloc, t, buf, path_len);
    CmdGuard cmd_guard(&cmd, alloc);

    char dir_path_buf[1200];
    const size_t dir_len = realPathOf(alloc, tmp_dir.path, dir_path_buf, sizeof dir_path_buf);
    Limits limits;
    limits.file = false;
    limits.temporary_file_enabled = true;
    limits.temporary_file_directory = dir_path_buf;
    limits.temporary_file_directory_len = dir_len;
    limits.shared_memory = false;
    LoadingImage loading;
    ASSERT_TRUE(Error::none == LoadingImage::init(alloc, &cmd, limits, &loading));
    LoadingGuard loading_guard(&loading, alloc);
}

TEST(kitty_graphics_image, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
