/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibCore/DirIterator.h>
#include <LibGUI/Painter.h>
#include <LibGfx/Font.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/LayoutBlock.h>
#include <LibWeb/Layout/LayoutText.h>
#include <ctype.h>

namespace Web {

LayoutText::LayoutText(const Text& text)
    : LayoutNode(&text)
{
    set_inline(true);
}

LayoutText::~LayoutText()
{
}

static bool is_all_whitespace(const String& string)
{
    for (size_t i = 0; i < string.length(); ++i) {
        if (!isspace(string[i]))
            return false;
    }
    return true;
}

const String& LayoutText::text_for_style(const StyleProperties& style) const
{
    static String one_space = " ";
    if (is_all_whitespace(node().data())) {
        if (style.string_or_fallback(CSS::PropertyID::WhiteSpace, "normal") == "normal")
            return one_space;
    }
    return node().data();
}

void LayoutText::render_fragment(RenderingContext& context, const LineBoxFragment& fragment) const
{
    auto& painter = context.painter();
    painter.set_font(style().font());

    auto background_color = style().property(CSS::PropertyID::BackgroundColor);
    if (background_color.has_value() && background_color.value()->is_color())
        painter.fill_rect(enclosing_int_rect(fragment.rect()), background_color.value()->to_color(document()));

    auto color = style().color_or_fallback(CSS::PropertyID::Color, document(), context.palette().base_text());
    auto text_decoration = style().string_or_fallback(CSS::PropertyID::TextDecoration, "none");

    if (document().inspected_node() == &node())
        context.painter().draw_rect(enclosing_int_rect(fragment.rect()), Color::Magenta);

    bool is_underline = text_decoration == "underline";
    if (is_underline)
        painter.draw_line(enclosing_int_rect(fragment.rect()).bottom_left().translated(0, 1), enclosing_int_rect(fragment.rect()).bottom_right().translated(0, 1), color);

    auto text = m_text_for_rendering;
    auto text_transform = style().string_or_fallback(CSS::PropertyID::TextTransform, "none");
    if (text_transform == "uppercase")
        text = m_text_for_rendering.to_uppercase();
    if (text_transform == "lowercase")
        text = m_text_for_rendering.to_lowercase();

    painter.draw_text(enclosing_int_rect(fragment.rect()), text.substring_view(fragment.start(), fragment.length()), Gfx::TextAlignment::TopLeft, color);
}

template<typename Callback>
void LayoutText::for_each_chunk(Callback callback, bool do_wrap_lines, bool do_wrap_breaks) const
{
    Utf8View view(m_text_for_rendering);
    if (view.is_empty())
        return;

    auto start_of_chunk = view.begin();

    auto commit_chunk = [&](auto it, bool has_breaking_newline) {
        int start = view.byte_offset_of(start_of_chunk);
        int length = view.byte_offset_of(it) - view.byte_offset_of(start_of_chunk);

        if (has_breaking_newline || length > 0) {
            callback(view.substring_view(start, length), start, length, has_breaking_newline);
        }

        start_of_chunk = it;
    };

    bool last_was_space = isspace(*view.begin());
    bool last_was_newline = false;
    for (auto it = view.begin(); it != view.end();) {
        if (last_was_newline) {
            last_was_newline = false;
            commit_chunk(it, true);
        }
        if (do_wrap_breaks && *it == '\n') {
            last_was_newline = true;
            commit_chunk(it, false);
        }
        if (do_wrap_lines) {
            bool is_space = isspace(*it);
            if (is_space != last_was_space) {
                last_was_space = is_space;
                commit_chunk(it, false);
            }
        }
        ++it;
    }
    if (last_was_newline)
        commit_chunk(view.end(), true);
    if (start_of_chunk != view.end())
        commit_chunk(view.end(), false);
}

void LayoutText::split_into_lines_by_rules(LayoutBlock& container, bool do_collapse, bool do_wrap_lines, bool do_wrap_breaks)
{
    auto& font = style().font();
    float space_width = font.glyph_width(' ') + font.glyph_spacing();

    auto& line_boxes = container.line_boxes();
    if (line_boxes.is_empty())
        line_boxes.append(LineBox());
    float available_width = container.width() - line_boxes.last().width();

    // Collapse whitespace into single spaces
    if (do_collapse) {
        auto utf8_view = Utf8View(node().data());
        StringBuilder builder(node().data().length());
        for (auto it = utf8_view.begin(); it != utf8_view.end(); ++it) {
            if (!isspace(*it)) {
                builder.append(utf8_view.as_string().characters_without_null_termination() + utf8_view.byte_offset_of(it), it.codepoint_length_in_bytes());
            } else {
                builder.append(' ');
                auto prev = it;
                while (it != utf8_view.end() && isspace(*it)) {
                    prev = it;
                    ++it;
                }
                it = prev;
            }
        }
        m_text_for_rendering = builder.to_string();
    } else {
        m_text_for_rendering = node().data();
    }

    // do_wrap_lines  => chunks_are_words
    // !do_wrap_lines => chunks_are_lines
    struct Chunk {
        Utf8View view;
        int start;
        int length;
        bool is_break;
    };
    Vector<Chunk> chunks;

    for_each_chunk([&](const Utf8View& view, int start, int length, bool is_break) {
        chunks.append({ Utf8View(view), start, length, is_break });
    },
        do_wrap_lines, do_wrap_breaks);

    for (size_t i = 0; i < chunks.size(); ++i) {
        auto& chunk = chunks[i];

        float chunk_width;
        bool need_collapse = false;
        if (do_wrap_lines) {
            bool need_collapse = do_collapse && isspace(*chunk.view.begin());

            if (need_collapse)
                chunk_width = space_width;
            else
                chunk_width = font.width(chunk.view) + font.glyph_spacing();

            if (line_boxes.last().width() > 0 && chunk_width > available_width) {
                line_boxes.append(LineBox());
                available_width = container.width();
            }
            if (need_collapse & line_boxes.last().fragments().is_empty())
                continue;
        } else {
            chunk_width = font.width(chunk.view);
        }

        line_boxes.last().add_fragment(*this, chunk.start, need_collapse ? 1 : chunk.length, chunk_width, font.glyph_height());
        available_width -= chunk_width;

        if (do_wrap_lines) {
            if (available_width < 0) {
                line_boxes.append(LineBox());
                available_width = container.width();
            }
        }

        if (do_wrap_breaks) {
            if (chunk.is_break) {
                line_boxes.append(LineBox());
                available_width = container.width();
            }
        }
    }
}

void LayoutText::split_into_lines(LayoutBlock& container)
{
    bool do_collapse = true;
    bool do_wrap_lines = true;
    bool do_wrap_breaks = false;
    auto white_space_prop = style().string_or_fallback(CSS::PropertyID::WhiteSpace, "normal");

    if (white_space_prop == "nowrap") {
        do_collapse = true;
        do_wrap_lines = false;
        do_wrap_breaks = false;
    } else if (white_space_prop == "pre") {
        do_collapse = false;
        do_wrap_lines = false;
        do_wrap_breaks = true;
    } else if (white_space_prop == "pre-line") {
        do_collapse = true;
        do_wrap_lines = true;
        do_wrap_breaks = true;
    } else if (white_space_prop == "pre-wrap") {
        do_collapse = false;
        do_wrap_lines = true;
        do_wrap_breaks = true;
    }

    split_into_lines_by_rules(container, do_collapse, do_wrap_lines, do_wrap_breaks);
}

}
