#include "index.h"
#include "json.h"
#include "note_io.h"

#include <algorithm>
#include <sys/stat.h>

namespace bn {

namespace {

bool ends_with(const std::string &s, const std::string &suf) {
    return s.size() >= suf.size() &&
           s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

uint32_t mtime_ms(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (uint32_t)(st.st_mtime * 1000ULL);
}

std::string unique_id(const std::string &root, const std::string &base) {
    if (!path_exists(root + "/" + base)) return base;
    for (int i = 2; i < 9999; ++i) {
        std::string cand = base + "-" + std::to_string(i);
        if (!path_exists(root + "/" + cand)) return cand;
    }
    return base + "-x";
}

} // namespace

bool NotesIndex::open(const std::string &root) {
    root_ = root;
    if (!ensure_dir(root_)) return false;
    entries_.clear();
    for (auto &name : list_dir(root_)) {
        std::string full = root_ + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            std::string nj = full + "/note.json";
            if (!path_exists(nj)) continue;
            std::string raw; json::Value v;
            std::string title = name;
            if (read_file(nj, raw) && json::parse(raw, v))
                title = v.str("title", name);
            entries_.push_back({name, title, full,
                                mtime_ms(nj), false});
        } else if (S_ISREG(st.st_mode) && ends_with(name, ".md")) {
            entries_.push_back({name, name, full,
                                mtime_ms(full), true});
        }
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const IndexEntry &a, const IndexEntry &b){
                  return a.updated_ms > b.updated_ms;
              });
    return true;
}

IndexEntry NotesIndex::create_note(const std::string &title, TemplateId tmpl) {
    std::string id = unique_id(root_, slugify(title));
    std::string dir = root_ + "/" + id;
    ensure_dir(dir);
    Note n;
    n.id = id;
    n.title = title;
    n.default_template = tmpl;
    n.pages.emplace_back();
    n.pages.back().tmpl = tmpl;
    save_note(dir, n);
    IndexEntry e{id, title, dir, now_ms(), false};
    entries_.insert(entries_.begin(), e);
    return e;
}

IndexEntry NotesIndex::create_markdown(const std::string &title) {
    std::string base = slugify(title);
    if (!ends_with(base, ".md")) base += ".md";
    std::string path = root_ + "/" + base;
    if (!path_exists(path))
        write_file(path, "# " + title + "\n");
    IndexEntry e{base, base, path, now_ms(), true};
    entries_.insert(entries_.begin(), e);
    return e;
}

void NotesIndex::touch(const std::string &id, const std::string &title) {
    for (auto &e : entries_) {
        if (e.id == id) {
            e.title = title;
            e.updated_ms = now_ms();
            break;
        }
    }
}

bool NotesIndex::save() const {
    json::Value arr = json::Value::make_arr();
    for (auto &e : entries_) {
        json::Value o = json::Value::make_obj();
        o.obj.emplace("id",      json::Value::make_str(e.id));
        o.obj.emplace("title",   json::Value::make_str(e.title));
        o.obj.emplace("updated", json::Value::make_num(e.updated_ms));
        o.obj.emplace("md",      json::Value::make_bool(e.is_markdown));
        arr.arr.push_back(std::move(o));
    }
    return write_file(root_ + "/_index.json", json::serialize(arr));
}

} // namespace bn
