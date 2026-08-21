/*
 * mini_html5.h — WHATWG HTML5 parsing pipeline (tokenizer + tree construction).
 *
 * A faithful, from-scratch implementation of the WHATWG HTML Standard's
 * "parsing HTML documents" pipeline, replacing the legacy single-pass
 * tolerant parser (mini_dom_parse_html) with the real algorithm:
 *
 *   - Spec tokenizer state machine (data, tag-open, tag-name, before-attr,
 *     attribute-name, after-attr-name, before-attr-value, attribute-value
 *     (dq/sq/unquoted), after-attr-value, self-closing-start, bogus-comment,
 *     markup-declaration-open, comment-start/end, doctype, RAWTEXT, RCDATA,
 *     character-reference).
 *   - Tree construction with insertion modes (INITIAL, BEFORE_HTML,
 *     BEFORE_HEAD, IN_HEAD, AFTER_HEAD, IN_BODY, IN_TABLE, IN_TABLE_BODY,
 *     IN_ROW, IN_CELL, IN_SELECT, AFTER_BODY, AFTER_AFTER_BODY) and the
 *     open-elements stack + active-formatting-elements list.
 *   - Adoption Agency Algorithm (AAA) for mis-nested formatting elements,
 *     e.g. <b>x<p>y</b>z and <p><div></p>.
 *   - Implicit / auto-closing rules: <p> closed by block-level starts;
 *     <li>/<dd>/<dt>/<option>/<tr>/<td>/<th>/<tbody>/<tfoot>/<thead> closed by
 *     a sibling of the same family; <head>/<html>/<body>/<table> inferred.
 *   - Table foster-parenting: table-internal elements generated in the wrong
 *     context are fostered to the location *before* the table.
 *   - Streaming: a parser is fed chunks via mini_html5_feed(); the DOM tree is
 *     built incrementally as bytes arrive (edge-of-network partial docs).
 *
 * The parser builds directly into a MiniDocument using the mini_node_* tree
 * API, so it composes with the existing layout/render pipeline without a
 * separate IR. One-shot callers use mini_html5_parse(); the legacy
 * mini_dom_parse_html() is reimplemented as a thin wrapper over this.
 *
 * Reference: https://html.spec.whatwg.org/ (sections 13.2.5 tokenizer,
 * 13.2.6 tree construction, 13.2.4.3 AAA).
 */
#ifndef MINI_HTML5_H
#define MINI_HTML5_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct MiniDocument;

/* Opaque parser state (tokenizer + open-elements stack + AFE list + mode). */
typedef struct MiniHtml5Parser MiniHtml5Parser;

/* Create a parser bound to a live document. Initializes the html/head/body
   skeleton if the document is empty (mirrors the spec's "before html" steps).
   The parser owns no nodes; it mutates the document tree in place. */
MiniHtml5Parser *mini_html5_init(struct MiniDocument *doc);

/* Feed a (possibly partial) chunk of the input byte stream. Builds DOM nodes
   for whatever complete tokens fall within the chunk and buffers the trailing
   partial token for the next feed. Safe to call many times (streaming). Returns
   0 on success, -1 on allocation failure. */
int mini_html5_feed(MiniHtml5Parser *p, const char *data, size_t len);

/* Finish parsing: flush buffered characters, run the "after body / after
   after body" close steps so the tree is in a terminal state. Must be called
   once at EOF. Returns 0 on success. */
int mini_html5_finish(MiniHtml5Parser *p);

/* Release parser state (not the document tree). */
void mini_html5_destroy(MiniHtml5Parser *p);

/* Convenience one-shot: feed the whole string + finish. Replaces the legacy
   mini_dom_parse_html contract (appends to doc->body). */
void mini_html5_parse(struct MiniDocument *doc, const char *html);

/* Self-test: parses a battery of malformed/edge-case documents and asserts
   the resulting tree shape (AAA, foster-parenting, auto-close). Returns 0
   when every assertion holds. */
int mini_html5_selftest(void);

#ifdef __cplusplus
}
#endif
#endif /* MINI_HTML5_H */
