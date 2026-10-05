/* Transliterated from Ghostty src/datastruct/intrusive_linked_list.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 */

#pragma once
#ifndef WISP_DATASTRUCT_INTRUSIVE_LINKED_LIST_HPP
#define WISP_DATASTRUCT_INTRUSIVE_LINKED_LIST_HPP

namespace wisp {
namespace datastruct {

/* An intrusive doubly-linked list. The type T must have a "next" and "prev"
 * field pointing to itself.
 *
 * This is an adaptation of the DoublyLinkedList from the Zig standard
 * library, which is MIT licensed. I've removed functionality that I don't
 * need. */
template <typename T>
struct IntrusiveDoublyLinkedList {
    /* The type of the node in the list. This makes it easy to get the
     * node type from the list type. */
    typedef T Node;

    Node *first; /* = null */
    Node *last;  /* = null */

    IntrusiveDoublyLinkedList() : first(nullptr), last(nullptr) {}

    /* Insert a new node after an existing one. */
    void insertAfter(Node *node, Node *new_node) {
        new_node->prev = node;
        if (Node *next_node = node->next) {
            /* Intermediate node. */
            new_node->next = next_node;
            next_node->prev = new_node;
        } else {
            /* Last element of the list. */
            new_node->next = nullptr;
            last = new_node;
        }
        node->next = new_node;
    }

    /* Insert a new node before an existing one. */
    void insertBefore(Node *node, Node *new_node) {
        new_node->next = node;
        if (Node *prev_node = node->prev) {
            /* Intermediate node. */
            new_node->prev = prev_node;
            prev_node->next = new_node;
        } else {
            /* First element of the list. */
            new_node->prev = nullptr;
            first = new_node;
        }
        node->prev = new_node;
    }

    /* Insert a new node at the end of the list. */
    void append(Node *new_node) {
        if (last) {
            /* Insert after last. */
            insertAfter(last, new_node);
        } else {
            /* Empty list. */
            prepend(new_node);
        }
    }

    /* Insert a new node at the beginning of the list. */
    void prepend(Node *new_node) {
        if (first) {
            /* Insert before first. */
            insertBefore(first, new_node);
        } else {
            /* Empty list. */
            first = new_node;
            last = new_node;
            new_node->prev = nullptr;
            new_node->next = nullptr;
        }
    }

    /* Remove a node from the list. */
    void remove(Node *node) {
        if (Node *prev_node = node->prev) {
            /* Intermediate node. */
            prev_node->next = node->next;
        } else {
            /* First element of the list. */
            first = node->next;
        }

        if (Node *next_node = node->next) {
            /* Intermediate node. */
            next_node->prev = node->prev;
        } else {
            /* Last element of the list. */
            last = node->prev;
        }
    }

    /* Remove and return the last node in the list. */
    Node *pop() {
        Node *l = last;
        if (!l) return nullptr;
        remove(l);
        return l;
    }

    /* Remove and return the first node in the list. */
    Node *popFirst() {
        Node *f = first;
        if (!f) return nullptr;
        remove(f);
        return f;
    }
};

} /* namespace datastruct */
} /* namespace wisp */

#endif /* WISP_DATASTRUCT_INTRUSIVE_LINKED_LIST_HPP */
