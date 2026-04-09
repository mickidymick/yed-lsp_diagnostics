#include <string>
#include <map>
#include <vector>

using namespace std;

#include "json.hpp"
using json = nlohmann::json;

extern "C" {
#include <yed/plugin.h>
}

struct Diagnostic {
    string message;
    int    line;
    int    byte_start;
    int    byte_end;
    int    severity;
};


static yed_plugin *Self;

static map<string, vector<Diagnostic> > diagnostics;

static void update_buffer() {
    yed_buffer *buff = yed_get_or_create_special_rdonly_buffer("*lsp-diagnostics");
    buff->flags &= ~BUFF_RD_ONLY;
    yed_buff_clear_no_undo(buff);

    int count = 0;

    for (const auto &it : diagnostics) {
        const auto &path  = it.first;
        const auto &diags = it.second;

        for (const auto &d : diags) {
            int        row  = yed_buff_n_lines(buff);
            yed_line  *line = yed_buff_get_line(buff, row);
            yed_glyph *git  = NULL;
            char       s[4096];

            {
                relative_path_if_subtree(path.c_str(), s);
                yed_glyph_traverse(s, git) {
                    yed_append_to_line_no_undo(buff, row, git);
                }
                yed_append_to_line_no_undo(buff, row, GLYPH(":"));
            }
            {
                snprintf(s, sizeof(s), "%d", d.line);
                yed_glyph_traverse(s, git) {
                    yed_append_to_line_no_undo(buff, row, git);
                }
                yed_append_to_line_no_undo(buff, row, GLYPH(":"));
            }

            {
                snprintf(s, sizeof(s), "%d", yed_line_idx_to_col(line, d.byte_start));
                yed_glyph_traverse(s, git) {
                    yed_append_to_line_no_undo(buff, row, git);
                }
                yed_append_to_line_no_undo(buff, row, GLYPH(":"));
                yed_append_to_line_no_undo(buff, row, GLYPH(" "));
                yed_append_to_line_no_undo(buff, row, GLYPH(" "));
                yed_append_to_line_no_undo(buff, row, GLYPH(" "));
                yed_append_to_line_no_undo(buff, row, GLYPH(" "));
            }
            {
                yed_glyph_traverse(d.message.c_str(), git) {
                    yed_append_to_line_no_undo(buff, row, git);
                }
            }

            yed_buffer_add_line_no_undo(buff);

            count += 1;
        }
    }

    if (count > 0) {
        yed_buff_delete_line_no_undo(buff, yed_buff_n_lines(buff));
    }

    buff->flags |= BUFF_RD_ONLY;
}

static void pmsg(yed_event *event) {
    if (strcmp(event->plugin_message.plugin_id, "lsp") != 0
    ||  strcmp(event->plugin_message.message_id, "textDocument/publishDiagnostics") != 0) {
        return;
    }

    try {
        auto j = json::parse(event->plugin_message.string_data);

        const string &uri = j["params"]["uri"];

        string path = uri;
        if (path.rfind("file://", 0) == 0) {
            path.erase(0, 7);
        }

        auto search = diagnostics.find(path);

        if (search != diagnostics.end()) {
            search->second.clear();
        }

        for (const auto &diag : j["params"]["diagnostics"]) {
            Diagnostic d;

            d.message    = diag["message"];
            d.message    = d.message.substr(0, d.message.find("\n"));
            d.line       = diag["range"]["start"]["line"];
            d.byte_start = diag["range"]["start"]["character"];

            int end_line = diag["range"]["end"]["line"];
            if (end_line == d.line) {
                d.byte_end  = diag["range"]["end"]["character"];
                d.byte_end -= 1;
            } else {
                d.byte_end = -1;
            }

            d.line += 1;

            if (diag.contains("severity")) {
                d.severity = diag["severity"];
            } else {
                d.severity = 1;
            }

            diagnostics[path].push_back(std::move(d));
        }

        update_buffer();
        yed_force_update();
    } catch (...) {}
}

static yed_attrs get_err_attrs(void) {
    yed_attrs active;
    yed_attrs a;
    yed_attrs red;
    float     brightness;

    active = yed_active_style_get_active();

    if (ATTR_FG_KIND(active.flags) != ATTR_KIND_RGB
    ||  ATTR_BG_KIND(active.flags) != ATTR_KIND_RGB) {

        return yed_parse_attrs("&active.bg &red.fg swap");
    }

    a   = active;
    red = yed_active_style_get_red();

    brightness = ((float)(RGB_32_r(active.fg) + RGB_32_g(active.fg) + RGB_32_b(active.fg)) / 3) / 255.0f;
    a.fg       = RGB_32(RGB_32_r(red.fg) / 2 + (u32)(brightness * 0x7f),
                        RGB_32_g(red.fg) / 2 + (u32)(brightness * 0x7f),
                        RGB_32_b(red.fg) / 2 + (u32)(brightness * 0x7f));

    brightness = ((float)(RGB_32_r(active.bg) + RGB_32_g(active.bg) + RGB_32_b(active.bg)) / 3) / 255.0f;
    a.bg       = RGB_32(RGB_32_r(red.fg) / 2 + (u32)(brightness * 0x7f),
                        RGB_32_g(red.fg) / 2 + (u32)(brightness * 0x7f),
                        RGB_32_b(red.fg) / 2 + (u32)(brightness * 0x7f));

    return a;
}

static vector<Diagnostic> *diags_for_buffer(yed_buffer *buffer) {
    vector<Diagnostic> *buff_diags_p = NULL;

    decltype(diagnostics)::iterator search;

    if (buffer->path != NULL) {
        search = diagnostics.find(buffer->path);
        if (search != diagnostics.end()) {
            buff_diags_p = &search->second;
        }
    } else {
        search = diagnostics.find(buffer->name);
        if (search != diagnostics.end()) {
            buff_diags_p = &search->second;
        }
    }

    return buff_diags_p;
}

static void linedraw(yed_event *event) {
    if (event->frame->buffer == NULL) { return; }

    vector<Diagnostic> *buff_diags_p = diags_for_buffer(event->frame->buffer);
    if (buff_diags_p == NULL) { return; }

    vector<Diagnostic> &buff_diags = *buff_diags_p;

    yed_attrs attrs = get_err_attrs();
    attrs.flags |= ATTR_UNDERLINE;

    for (const auto &diag : buff_diags) {
        if (event->row == diag.line) {
            yed_line *line = yed_buff_get_line(event->frame->buffer, event->row);
            if (line == NULL) { continue; }

            int start_col = yed_line_idx_to_col(line, diag.byte_start);
            int end_col   = diag.byte_end < 0 ? line->visual_width : yed_line_idx_to_col(line, diag.byte_end);

            for (int c = start_col; c <= end_col; c += 1) {
                yed_eline_combine_col_attrs(event, c, &attrs);
            }
        }
    }
}

static void update(yed_event *event) {
    const char *show;
    int         when_cursor;
    int         er;
    int         eg;
    int         eb;
    int         ar;
    int         ag;
    int         ab;

    if (event->frame->buffer == NULL) { return; }

    show = yed_get_var("lsp-diagnostics-show-message");

    if (!yed_var_is_truthy("lsp-diagnostics-show-message")) { return; }
    if (strcmp(show, "never") == 0) { return; }

    if (strcmp(show, "cursor") == 0) {
        when_cursor = 1;
    } else {
        when_cursor = 0;
    }

    vector<Diagnostic> *buff_diags_p = diags_for_buffer(event->frame->buffer);
    if (buff_diags_p == NULL) { return; }

    vector<Diagnostic> &buff_diags = *buff_diags_p;

    if (buff_diags.size() == 0) { return; }

    yed_attrs b_attrs = event->frame == ys->active_frame ? yed_active_style_get_active() : yed_active_style_get_inactive();

    for (int i = 1; i <= event->frame->height; i += 1) {
        int row = event->frame->buffer_y_offset + i;

        if (when_cursor && event->frame->cursor_line != row) { continue; }

        for (const auto &diag : buff_diags) {
            if (row == diag.line) {
                yed_line *line = yed_buff_get_line(event->frame->buffer, row);
                if (line == NULL) { continue; }

                int start_col = yed_line_idx_to_col(line, diag.byte_start);
                int end_col   = diag.byte_end < 0 ? line->visual_width : yed_line_idx_to_col(line, diag.byte_end);

                if (start_col > line->visual_width) {
                    int r = event->frame->left + line->visual_width - event->frame->buffer_x_offset;
                    if (r <= event->frame->left + event->frame->width - 1) {
                        yed_set_cursor(event->frame->top + i - 1, r);
                        yed_attrs attrs = get_err_attrs();
                        yed_combine_attrs(&b_attrs, &attrs);
                        attrs.flags |= ATTR_UNDERLINE;
                        yed_set_attr(attrs);
                        yed_screen_print_n(" ", 1);
                    }
                }

                if (when_cursor) {
                    if (diag.byte_start) {
                        if (start_col <= line->visual_width) {
                            if (event->frame->cursor_col < start_col || event->frame->cursor_col > end_col) {
                                continue;
                            }
                        } else {
                            if (event->frame->cursor_col <= line->visual_width) {
                                continue;
                            }
                        }
                    }
                }

                yed_attrs attrs = get_err_attrs();

                string chopped = " ● ";

                chopped += diag.message;
                chopped += " ";

                int space_avail = 1 + event->frame->width - (line->visual_width + 8 - event->frame->buffer_x_offset);
                if (yed_get_string_width(chopped.c_str()) > space_avail) {
                    space_avail -= 3;
                    do {
                        chopped.pop_back();
                    } while (chopped.size() && yed_get_string_width(chopped.c_str()) > space_avail);
                    chopped += "...";
                }

                yed_set_attr(attrs);

                int        l    = event->frame->left + line->visual_width + 8 - event->frame->buffer_x_offset - 1;
                float      p = 0.005;
                yed_glyph *git  = NULL;

                auto step_attr = [&]() {
                    if (ATTR_BG_KIND(attrs.flags)   == ATTR_KIND_RGB
                    &&  ATTR_BG_KIND(b_attrs.flags) == ATTR_KIND_RGB) {

                        er = (int)((1.0 - p) * RGB_32_r(attrs.bg));
                        eg = (int)((1.0 - p) * RGB_32_g(attrs.bg));
                        eb = (int)((1.0 - p) * RGB_32_b(attrs.bg));
                        ar = (int)(p * RGB_32_r(b_attrs.bg));
                        ag = (int)(p * RGB_32_g(b_attrs.bg));
                        ab = (int)(p * RGB_32_b(b_attrs.bg));
                        attrs.bg = RGB_32(er + ar, eg + ag, eb + ab);
                        yed_set_attr(attrs);

                        p *= 1.05;
                        if (p > 1.0) { p = 1.0; }
                    }
                };

                yed_glyph_traverse(chopped.c_str(), git) {
                    step_attr();
                    yed_set_cursor(event->frame->top + i - 1, l);
                    yed_screen_print_single_cell_glyph_over(git);
                    l += yed_get_glyph_width(git);
                }

                for (; l <= event->frame->left + event->frame->width - 1; l += 1) {
                    step_attr();
                    yed_set_cursor(event->frame->top + i - 1, l);
                    yed_screen_print_over(" ");
                }

                break;
            }
        }
    }
}

static void mod(yed_event *event) {
    if (event->buffer == NULL) { return; }

    vector<Diagnostic> *buff_diags_p = diags_for_buffer(event->buffer);
    if (buff_diags_p == NULL) { return; }

    vector<Diagnostic> &buff_diags = *buff_diags_p;

    switch (event->buff_mod_event) {
        case BUFF_MOD_INSERT_LINE:
            for (auto &diag : buff_diags) {
                if (event->row <= diag.line) {
                    diag.line += 1;
                }
            }
            update_buffer();
            break;

        case BUFF_MOD_DELETE_LINE: {
again_line:;
            size_t idx = 0;
            for (auto &diag : buff_diags) {
                if (event->row == diag.line) {
                    buff_diags.erase(buff_diags.begin() + idx);
                    goto again_line;
                } else if (event->row < diag.line) {
                    diag.line -= 1;
                }
                idx += 1;
            }
            update_buffer();
            break;
        }

        case BUFF_MOD_CLEAR:
            buff_diags.clear();
            update_buffer();
            break;

        default: {
            bool did_change = false;
again_range:;
            size_t idx = 0;
            for (auto &diag : buff_diags) {
                if (event->row == diag.line) {
                    yed_line *line = yed_buff_get_line(event->buffer, event->row);
                    if (line == NULL) { continue; }

                    int start_col = yed_line_idx_to_col(line, diag.byte_start);
                    int end_col   = yed_line_idx_to_col(line, diag.byte_end);

                    if (event->buff_mod_event == BUFF_MOD_INSERT_INTO_LINE && event->col <= start_col) {
                        diag.byte_start += 1; /* @bad: this is not strictly correct (multi-byte glyph insertions) */
                        diag.byte_end   += 1;
                        did_change = true;
                    } else if (event->buff_mod_event == BUFF_MOD_DELETE_FROM_LINE && event->col < start_col) {
                        diag.byte_start -= 1; /* @bad: this is not strictly correct (multi-byte glyph deletions) */
                        diag.byte_end   -= 1;
                        did_change = true;
                    } else if (event->col >= start_col && event->col <= end_col) {
                        buff_diags.erase(buff_diags.begin() + idx);
                        did_change = true;
                        goto again_range;
                    }
                }
                idx += 1;
            }
            if (did_change) {
                update_buffer();
            }
            break;
        }
    }
}

static void key(yed_event *event) {
    yed_buffer *buff = yed_get_or_create_special_rdonly_buffer("*lsp-diagnostics");

    if (ys->interactive_command  != NULL
    ||  ys->active_frame         == NULL
    ||  ys->active_frame->buffer != buff
    ||  event->key               != ENTER) {

        return;
    }

    size_t idx = ys->active_frame->cursor_line - 1;
    size_t i   = 0;
    for (const auto &it : diagnostics) {
        const auto &path  = it.first;
        const auto &diags = it.second;

        for (const auto &d : diags) {
            if (i == idx) {
                int col = 1;

                yed_line *line = yed_buff_get_line(buff, ys->active_frame->cursor_line);
                if (line != NULL) {
                    col = yed_line_idx_to_col(line, d.byte_start);
                }

                YEXE("special-buffer-prepare-jump-focus", (char*)path.c_str());
                YEXE("buffer", (char*)path.c_str());
                yed_set_cursor_within_frame(ys->active_frame, d.line, col);

                goto out;
            }
            i += 1;
        }
    }

    out:;


    event->cancel = 1;
}

static void unload(yed_plugin *self) {}

extern "C"
int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();

    Self = self;

    map<void(*)(yed_event*), vector<yed_event_kind_t> > event_handlers = {
        { pmsg,     { EVENT_PLUGIN_MESSAGE    } },
        { linedraw, { EVENT_LINE_PRE_DRAW     } },
        { update,   { EVENT_FRAME_POST_UPDATE } },
        { mod,      { EVENT_BUFFER_POST_MOD   } },
        { key,      { EVENT_KEY_PRESSED       } },
    };

    map<const char*, const char*> vars = {
        { "lsp-diagnostics-show-message", "always" },
    };

    map<const char*, void(*)(int, char**)> cmds = {
    };

    for (auto &pair : event_handlers) {
        for (auto evt : pair.second) {
            yed_event_handler h;
            h.kind = evt;
            h.fn   = pair.first;
            yed_plugin_add_event_handler(self, h);
        }
    }

    for (auto &pair : vars) {
        if (!yed_get_var(pair.first)) { yed_set_var(pair.first, pair.second); }
    }

    for (auto &pair : cmds) {
        yed_plugin_set_command(self, pair.first, pair.second);
    }

    yed_plugin_set_unload_fn(self, unload);

    return 0;
}
