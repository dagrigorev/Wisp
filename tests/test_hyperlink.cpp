/* Tests for src/terminal/hyperlink.hpp and the page hyperlink operations.
 *
 * Related to Ghostty src/terminal/hyperlink.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * The interesting behavior here is identity: two OSC 8 runs to the same URI
 * are different links unless they say otherwise, and the strings a link owns
 * have to go back to the page when its last cell lets go.
 */

#include "test_helpers.h"
#include "page.hpp"

using namespace wisp::terminal;

struct PageBuf {
    uint64_t words[16384];
    uint8_t *base() { return reinterpret_cast<uint8_t *>(words); }
    PageBuf() { memset(words, 0, sizeof(words)); }
};

static bool uri_is(Page *p, CellCountInt x, CellCountInt y, const char *want) {
    const uint8_t *uri = nullptr;
    size_t len = 0;
    if (!page_get_cell_hyperlink(p, x, y, &uri, &len)) return false;
    if (len != strlen(want)) return false;
    return memcmp(uri, want, len) == 0;
}

/* ─── attaching ──────────────────────────────────────────────────────────── */

TEST(hyperlink, attach_and_read_back) {
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    const char *uri = "https://example.com/a";
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 0, uri, strlen(uri), NULL, 0, 1));

    ASSERT_TRUE(uri_is(&p, 0, 0, uri));
    ASSERT_TRUE(p.get_cell(0, 0)->hyperlink());
    ASSERT_TRUE(p.get_row(0)->hyperlink());
}

TEST(hyperlink, unlinked_cell_reports_nothing) {
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    const uint8_t *uri = nullptr;
    size_t len = 0;
    ASSERT_FALSE(page_get_cell_hyperlink(&p, 3, 3, &uri, &len));
    ASSERT_FALSE(p.get_cell(3, 3)->hyperlink());
}

TEST(hyperlink, empty_uri_is_refused) {
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    ASSERT_FALSE(page_set_cell_hyperlink(&p, 0, 0, "", 0, NULL, 0, 1));
    ASSERT_FALSE(p.get_cell(0, 0)->hyperlink());
}

/* ─── identity ───────────────────────────────────────────────────────────── */

TEST(hyperlink, one_run_over_many_cells_shares_an_entry) {
    /* A single OSC 8 sequence covering a span of text must intern once, not
     * once per cell — that is the whole point of the set. */
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    const char *uri = "https://example.com/shared";
    for (CellCountInt x = 0; x < 10; x++) {
        ASSERT_TRUE(page_set_cell_hyperlink(&p, x, 0, uri, strlen(uri), NULL, 0, 7));
    }

    ASSERT_EQ(p.hyperlink_set.count(), 1);

    for (CellCountInt x = 0; x < 10; x++) {
        ASSERT_TRUE(uri_is(&p, x, 0, uri));
    }
}

TEST(hyperlink, same_uri_with_different_implicit_ids_stays_distinct) {
    /* Two separate OSC 8 runs to the same URI are different links. The
     * implicit counter is what keeps them apart, so that hovering one does
     * not highlight the other. */
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    const char *uri = "https://example.com/same";
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 0, uri, strlen(uri), NULL, 0, 1));
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 1, 0, uri, strlen(uri), NULL, 0, 2));

    ASSERT_EQ(p.hyperlink_set.count(), 2);
    ASSERT_TRUE(uri_is(&p, 0, 0, uri));
    ASSERT_TRUE(uri_is(&p, 1, 0, uri));
}

TEST(hyperlink, same_explicit_id_is_one_link) {
    /* An explicit ID means "these are the same link", which is how a link
     * split across lines stays one link. */
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    const char *uri = "https://example.com/x";
    const char *id = "link-1";

    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 0, uri, strlen(uri), id, strlen(id), 1));
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 1, uri, strlen(uri), id, strlen(id), 2));

    /* Same explicit ID, so one entry despite different implicit counters. */
    ASSERT_EQ(p.hyperlink_set.count(), 1);
    ASSERT_TRUE(uri_is(&p, 0, 0, uri));
    ASSERT_TRUE(uri_is(&p, 0, 1, uri));
}

TEST(hyperlink, different_explicit_ids_are_different_links) {
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    const char *uri = "https://example.com/x";
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 0, uri, strlen(uri), "a", 1, 1));
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 1, 0, uri, strlen(uri), "b", 1, 1));

    ASSERT_EQ(p.hyperlink_set.count(), 2);
}

TEST(hyperlink, explicit_and_implicit_do_not_collide) {
    /* The kind is mixed into the hash first, so an explicit ID cannot be
     * confused with an implicit counter that shares its bytes. */
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    const char *uri = "https://example.com/y";
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 0, uri, strlen(uri), NULL, 0, 1));
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 1, 0, uri, strlen(uri), "\x01", 1, 0));

    ASSERT_EQ(p.hyperlink_set.count(), 2);
}

TEST(hyperlink, different_uris_are_different_links) {
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 0, "https://a.test", 14, NULL, 0, 1));
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 1, 0, "https://b.test", 14, NULL, 0, 1));

    ASSERT_EQ(p.hyperlink_set.count(), 2);
    ASSERT_TRUE(uri_is(&p, 0, 0, "https://a.test"));
    ASSERT_TRUE(uri_is(&p, 1, 0, "https://b.test"));
}

/* ─── releasing ──────────────────────────────────────────────────────────── */

TEST(hyperlink, clearing_the_last_cell_releases_the_link) {
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    const char *uri = "https://example.com/z";
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 0, uri, strlen(uri), NULL, 0, 1));
    ASSERT_EQ(p.hyperlink_set.count(), 1);

    page_clear_cell_hyperlink(&p, 0, 0);

    ASSERT_EQ(p.hyperlink_set.count(), 0);
    ASSERT_FALSE(p.get_cell(0, 0)->hyperlink());

    const uint8_t *got = nullptr;
    size_t len = 0;
    ASSERT_FALSE(page_get_cell_hyperlink(&p, 0, 0, &got, &len));
}

TEST(hyperlink, link_survives_until_its_last_cell_lets_go) {
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    const char *uri = "https://example.com/multi";
    for (CellCountInt x = 0; x < 5; x++) {
        ASSERT_TRUE(page_set_cell_hyperlink(&p, x, 0, uri, strlen(uri), NULL, 0, 3));
    }
    ASSERT_EQ(p.hyperlink_set.count(), 1);

    for (CellCountInt x = 0; x < 4; x++) {
        page_clear_cell_hyperlink(&p, x, 0);
        /* Still held by the cells that remain. */
        ASSERT_EQ(p.hyperlink_set.count(), 1);
        ASSERT_TRUE(uri_is(&p, 4, 0, uri));
    }

    page_clear_cell_hyperlink(&p, 4, 0);
    ASSERT_EQ(p.hyperlink_set.count(), 0);
}

TEST(hyperlink, clearing_an_unlinked_cell_is_harmless) {
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    page_clear_cell_hyperlink(&p, 2, 2);
    ASSERT_EQ(p.hyperlink_set.count(), 0);
}

TEST(hyperlink, released_link_keeps_its_strings_for_resurrection) {
    /* Releasing a link does not immediately hand its bytes back. The entry
     * stays dead-but-resurrectable, holding its strings, so re-adding the
     * same link reuses both rather than reallocating. Storage is reclaimed
     * when the entry is finally reaped, not when its last cell lets go. */
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    const char *uri = "https://example.com/reclaimed";
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 0, uri, strlen(uri), NULL, 0, 1));
    const size_t while_live = p.string_alloc.used_bytes(p.memory);
    ASSERT_TRUE(while_live > 0);

    page_clear_cell_hyperlink(&p, 0, 0);
    ASSERT_EQ(p.hyperlink_set.count(), 0);

    /* Still held, pending reaping. */
    ASSERT_EQ(p.string_alloc.used_bytes(p.memory), while_live);

    /* Re-adding resurrects the entry and allocates nothing further. */
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 0, uri, strlen(uri), NULL, 0, 1));
    ASSERT_EQ(p.string_alloc.used_bytes(p.memory), while_live);
    ASSERT_EQ(p.hyperlink_set.count(), 1);
    ASSERT_TRUE(uri_is(&p, 0, 0, uri));
}

TEST(hyperlink, cycling_through_links_does_not_exhaust_string_storage) {
    /* The real property behind deferred reclamation: a page that churns
     * through distinct links must not run its string storage down, because
     * reaping a dead entry frees the bytes it was holding. */
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    char uri[64];
    for (int i = 0; i < 200; i++) {
        _snprintf(uri, sizeof(uri), "https://example.com/cycle/%d", i);
        if (!page_set_cell_hyperlink(&p, 0, 0, uri, strlen(uri), NULL,
                                     0, (OffsetInt)i)) {
            /* Refusing is acceptable; silently corrupting is not. */
            break;
        }
        ASSERT_TRUE(uri_is(&p, 0, 0, uri));
        page_clear_cell_hyperlink(&p, 0, 0);
    }

    /* Whatever happened, the structures are still coherent. */
    ASSERT_TRUE(p.hyperlink_set.check_integrity(p.memory));
    ASSERT_TRUE(p.hyperlink_set.check_reachable(p.memory));
    ASSERT_TRUE(p.string_alloc.used_bytes(p.memory) <= p.string_alloc.capacity_bytes());
}

TEST(hyperlink, duplicate_attach_does_not_leak_strings) {
    /* Attaching an already-interned link allocates strings, finds the
     * existing entry, and must release what it just allocated rather than
     * stranding it for the page's lifetime. */
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    const char *uri = "https://example.com/dup";
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 0, uri, strlen(uri), NULL, 0, 1));

    const size_t after_first = p.string_alloc.used_bytes(p.memory);

    for (CellCountInt x = 1; x < 8; x++) {
        ASSERT_TRUE(page_set_cell_hyperlink(&p, x, 0, uri, strlen(uri), NULL, 0, 1));
    }

    /* Seven more cells, same link, no additional string bytes. */
    ASSERT_EQ(p.string_alloc.used_bytes(p.memory), after_first);
    ASSERT_EQ(p.hyperlink_set.count(), 1);
}

/* ─── relocation ─────────────────────────────────────────────────────────── */

TEST(hyperlink, links_survive_page_relocation) {
    /* URIs are slices into page memory, not pointers, so a page that moves
     * keeps its links. */
    PageBuf src;
    PageBuf dst;

    Capacity cap(20, 8);
    Page p = Page::init(src.base(), cap);

    const char *uri = "https://example.com/moved";
    for (CellCountInt x = 0; x < 5; x++) {
        ASSERT_TRUE(page_set_cell_hyperlink(&p, x, 0, uri, strlen(uri), NULL, 0, 1));
    }

    memcpy(dst.base(), src.base(), p.size);
    Page moved = p;
    moved.relocate(dst.base());
    memset(src.base(), 0xCD, p.size);

    for (CellCountInt x = 0; x < 5; x++) {
        ASSERT_TRUE(uri_is(&moved, x, 0, uri));
    }
    ASSERT_EQ(moved.hyperlink_set.count(), 1);
}

/* ─── exhaustion ─────────────────────────────────────────────────────────── */

TEST(hyperlink, running_out_is_reported_not_ignored) {
    /* Both the string storage and the map are finite. Running out must be
     * reported so the text still renders without a link, rather than
     * corrupting either structure. */
    PageBuf buf;
    Capacity cap(40, 20);
    Page p = Page::init(buf.base(), cap);

    char uri[64];
    bool refused = false;
    int attached = 0;

    for (int i = 0; i < 400; i++) {
        const CellCountInt x = (CellCountInt)(i % cap.cols);
        const CellCountInt y = (CellCountInt)(i / cap.cols);
        if (y >= cap.rows) break;

        _snprintf(uri, sizeof(uri), "https://example.com/%d", i);
        if (!page_set_cell_hyperlink(&p, x, y, uri, strlen(uri), NULL, 0,
                                     (OffsetInt)i)) {
            refused = true;
            break;
        }
        attached++;
    }

    ASSERT_TRUE(refused);
    ASSERT_TRUE(attached > 0);

    /* Everything attached before the refusal is still intact. */
    for (int i = 0; i < attached; i++) {
        const CellCountInt x = (CellCountInt)(i % cap.cols);
        const CellCountInt y = (CellCountInt)(i / cap.cols);
        _snprintf(uri, sizeof(uri), "https://example.com/%d", i);
        ASSERT_TRUE(uri_is(&p, x, y, uri));
    }
}
