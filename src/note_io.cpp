#include "note_io.h"
#include "canvas.h"
#include "json.h"
#include "templates.h"

#include <cairo/cairo.h>
#include <cstdio>

namespace bn {

namespace {

std::string page_name(int idx) {
    char buf[16]; std::snprintf(buf, sizeof(buf), "page-%03d", idx + 1);
    return buf;
}

json::Value rect_to_json(const Rect &r) {
    json::Value a = json::Value::make_arr();
    a.arr.push_back(json::Value::make_num(r.x));
    a.arr.push_back(json::Value::make_num(r.y));
    a.arr.push_back(json::Value::make_num(r.w));
    a.arr.push_back(json::Value::make_num(r.h));
    return a;
}

Rect rect_from_json(const json::Value &v) {
    Rect r;
    if (v.type != json::Type::Array || v.arr.size() < 4) return r;
    r.x = v.arr[0].n; r.y = v.arr[1].n;
    r.w = v.arr[2].n; r.h = v.arr[3].n;
    return r;
}

json::Value stroke_to_json(const Stroke &s) {
    json::Value o = json::Value::make_obj();
    o.obj.emplace("tool",     json::Value::make_str(s.tool == Tool::Eraser ? "eraser" : "pen"));
    o.obj.emplace("pen_type", json::Value::make_str(s.pen_type == PenType::Pen ? "pen" : "pencil"));
    o.obj.emplace("width",    json::Value::make_num(s.width));
    json::Value pts = json::Value::make_arr();
    pts.arr.reserve(s.pts.size());
    for (auto &p : s.pts) {
        json::Value pt = json::Value::make_arr();
        pt.arr.push_back(json::Value::make_num(p.x));
        pt.arr.push_back(json::Value::make_num(p.y));
        pt.arr.push_back(json::Value::make_num(p.pressure));
        pt.arr.push_back(json::Value::make_num(p.t_ms));
        pts.arr.push_back(std::move(pt));
    }
    o.obj.emplace("pts", std::move(pts));
    return o;
}

Stroke stroke_from_json(const json::Value &v) {
    Stroke s;
    s.tool     = v.str("tool") == "eraser" ? Tool::Eraser : Tool::Pen;
    s.pen_type = v.str("pen_type") == "pen" ? PenType::Pen : PenType::Pencil;
    s.width    = v.num("width", 1.4);
    auto *pts = v.get("pts");
    if (pts && pts->type == json::Type::Array) {
        s.pts.reserve(pts->arr.size());
        for (auto &p : pts->arr) {
            if (p.type != json::Type::Array || p.arr.size() < 2) continue;
            Point pt;
            pt.x = p.arr[0].n;
            pt.y = p.arr[1].n;
            pt.pressure = p.arr.size() > 2 ? (float)p.arr[2].n : 1.0f;
            pt.t_ms     = p.arr.size() > 3 ? (uint32_t)p.arr[3].n : 0;
            s.pts.push_back(pt);
        }
    }
    return s;
}

json::Value tags_to_json(const std::vector<std::string> &tags) {
    json::Value a = json::Value::make_arr();
    a.arr.reserve(tags.size());
    for (auto &t : tags) a.arr.push_back(json::Value::make_str(t));
    return a;
}

void tags_from_json(const json::Value &v, std::vector<std::string> &out) {
    auto *a = v.get("tags");
    if (!a || a->type != json::Type::Array) return;
    for (auto &tv : a->arr)
        if (tv.type == json::Type::String && !tv.s.empty()) out.push_back(tv.s);
}

bool save_page(const std::string &path, const Page &p) {
    json::Value o = json::Value::make_obj();
    o.obj.emplace("template", json::Value::make_str(template_name(p.tmpl)));
    if (!p.bg_image.empty())
        o.obj.emplace("bg_image", json::Value::make_str(p.bg_image));
    json::Value arr = json::Value::make_arr();
    arr.arr.reserve(p.strokes.size());
    for (auto &s : p.strokes) arr.arr.push_back(stroke_to_json(s));
    o.obj.emplace("strokes", std::move(arr));
    if (!p.tags.empty()) o.obj.emplace("tags", tags_to_json(p.tags));
    return write_file(path, json::serialize(o));
}

bool load_page(const std::string &path, Page &p) {
    std::string raw;
    if (!read_file(path, raw)) return false;
    json::Value v;
    if (!json::parse(raw, v)) return false;
    p.tmpl = template_from_name(v.str("template", "blank"));
    p.bg_image = v.str("bg_image", "");
    auto *arr = v.get("strokes");
    if (arr && arr->type == json::Type::Array) {
        p.strokes.reserve(arr->arr.size());
        for (auto &sv : arr->arr) p.strokes.push_back(stroke_from_json(sv));
    }
    tags_from_json(v, p.tags);
    return true;
}

} // namespace

bool save_note(const std::string &dir, const Note &n) {
    if (!ensure_dir(dir)) return false;

    json::Value root = json::Value::make_obj();
    root.obj.emplace("id",    json::Value::make_str(n.id));
    root.obj.emplace("title", json::Value::make_str(n.title));
    root.obj.emplace("template",
                     json::Value::make_str(template_name(n.default_template)));
    if (!n.default_bg_image.empty())
        root.obj.emplace("bg_image", json::Value::make_str(n.default_bg_image));
    if (!n.tags.empty()) root.obj.emplace("tags", tags_to_json(n.tags));

    json::Value pages = json::Value::make_arr();
    for (size_t i = 0; i < n.pages.size(); ++i)
        pages.arr.push_back(json::Value::make_str(page_name((int)i)));
    root.obj.emplace("pages", std::move(pages));

    json::Value links = json::Value::make_arr();
    for (auto &l : n.links) {
        json::Value lo = json::Value::make_obj();
        lo.obj.emplace("page",   json::Value::make_num(l.page));
        lo.obj.emplace("rect",   rect_to_json(l.rect));
        lo.obj.emplace("target", json::Value::make_str(l.target));
        links.arr.push_back(std::move(lo));
    }
    root.obj.emplace("links", std::move(links));

    if (!n.ocr_text.empty()) {
        json::Value ot = json::Value::make_obj();
        for (size_t i = 0; i < n.ocr_text.size(); ++i) {
            if (n.ocr_text[i].empty()) continue;
            char k[8]; std::snprintf(k, sizeof(k), "%zu", i + 1);
            ot.obj.emplace(k, json::Value::make_str(n.ocr_text[i]));
        }
        root.obj.emplace("ocr_text", std::move(ot));
    }

    if (!write_file(dir + "/note.json", json::serialize(root))) return false;

    for (size_t i = 0; i < n.pages.size(); ++i) {
        std::string pp = dir + "/" + page_name((int)i) + ".json";
        if (!save_page(pp, n.pages[i])) return false;
    }
    return true;
}

bool load_note(const std::string &dir, Note &out) {
    std::string raw;
    if (!read_file(dir + "/note.json", raw)) return false;
    json::Value root;
    if (!json::parse(raw, root)) return false;

    out.id    = root.str("id");
    out.title = root.str("title");
    out.default_template = template_from_name(root.str("template", "blank"));
    out.default_bg_image = root.str("bg_image", "");
    tags_from_json(root, out.tags);

    out.pages.clear();
    auto *pages = root.get("pages");
    if (pages && pages->type == json::Type::Array) {
        out.pages.resize(pages->arr.size());
        for (size_t i = 0; i < pages->arr.size(); ++i) {
            std::string pp = dir + "/" + pages->arr[i].s + ".json";
            load_page(pp, out.pages[i]);
        }
    }
    if (out.pages.empty()) {
        out.pages.emplace_back();
        out.pages.back().tmpl = out.default_template;
        out.pages.back().bg_image = out.default_bg_image;
    }

    out.links.clear();
    auto *links = root.get("links");
    if (links && links->type == json::Type::Array) {
        for (auto &lv : links->arr) {
            Link l;
            l.page   = (int)lv.num("page", 0);
            l.target = lv.str("target");
            auto *r = lv.get("rect");
            if (r) l.rect = rect_from_json(*r);
            out.links.push_back(std::move(l));
        }
    }

    auto *ot = root.get("ocr_text");
    if (ot && ot->type == json::Type::Object) {
        for (auto &kv : ot->obj) {
            int idx = std::atoi(kv.first.c_str()) - 1;
            if (idx >= 0) {
                if ((int)out.ocr_text.size() <= idx)
                    out.ocr_text.resize(idx + 1);
                out.ocr_text[idx] = kv.second.s;
            }
        }
    }

    out.dirty = false;
    return true;
}

bool save_page_snapshot(const std::string &dir, int page_index,
                        int w, int h, const Note &n) {
    if (page_index < 0 || page_index >= (int)n.pages.size()) return false;
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    canvas_render_page(cr, n.pages[page_index], (double)w, (double)h);
    cairo_destroy(cr);
    std::string out = dir + "/" + page_name(page_index) + ".png";
    cairo_status_t st = cairo_surface_write_to_png(surf, out.c_str());
    cairo_surface_destroy(surf);
    return st == CAIRO_STATUS_SUCCESS;
}

} // namespace bn
