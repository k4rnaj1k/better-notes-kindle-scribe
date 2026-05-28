#include "index.h"
#include "json.h"
#include "note_io.h"

#include <algorithm>
#include <cctype>
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

std::string unique_id(const std::string &dir, const std::string &base) {
    if (!path_exists(dir + "/" + base)) return base;
    for (int i = 2; i < 9999; ++i) {
        std::string cand = base + "-" + std::to_string(i);
        if (!path_exists(dir + "/" + cand)) return cand;
    }
    return base + "-x";
}

std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string basename_no_ext(const std::string &p) {
    auto slash = p.find_last_of('/');
    std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    auto dot = name.find_last_of('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

std::string join_rel(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + "/" + b;
}

bool is_native_note_dir(const std::string &abs) {
    return path_exists(abs + "/note.json");
}

bool is_hidden(const std::string &name) {
    // Skip hidden dotfiles and Obsidian's metadata folders so the vault
    // browses cleanly.
    if (name.empty() || name[0] == '.') return true;
    if (name == "_index.json") return true;
    return false;
}

} // namespace

bool NotesIndex::open(const std::string &root) {
    return open(root, "");
}

bool NotesIndex::open(const std::string &root, const std::string &subdir) {
    root_   = root;
    subdir_ = subdir;
    if (!ensure_dir(root_)) return false;

    std::string abs = subdir.empty() ? root_ : root_ + "/" + subdir;
    if (!path_exists(abs)) abs = root_, subdir_.clear();

    entries_.clear();
    for (auto &name : list_dir(abs)) {
        if (is_hidden(name)) continue;
        std::string full = abs + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;

        std::string rel = join_rel(subdir_, name);
        if (S_ISDIR(st.st_mode)) {
            if (is_native_note_dir(full)) {
                // Treat as a single note (the legacy betternotes format).
                std::string title = name;
                std::string raw; json::Value v;
                if (read_file(full + "/note.json", raw) && json::parse(raw, v))
                    title = v.str("title", name);
                entries_.push_back({rel, title, full,
                                    mtime_ms(full + "/note.json"),
                                    false, false});
            } else {
                // Browseable folder (Obsidian-style or arbitrary subfolder).
                entries_.push_back({rel, name, full,
                                    mtime_ms(full), false, true});
            }
        } else if (S_ISREG(st.st_mode) && ends_with(lower(name), ".md")) {
            entries_.push_back({rel, name, full,
                                mtime_ms(full), true, false});
        }
    }
    // Folders first (alphabetical), then notes by most-recent first —
    // matches Obsidian's default sort.
    std::sort(entries_.begin(), entries_.end(),
              [](const IndexEntry &a, const IndexEntry &b){
                  if (a.is_folder != b.is_folder) return a.is_folder;
                  if (a.is_folder) return lower(a.title) < lower(b.title);
                  return a.updated_ms > b.updated_ms;
              });
    return true;
}

void NotesIndex::scan_dir(const std::string &abs_dir,
                          const std::string &rel_prefix) {
    for (auto &name : list_dir(abs_dir)) {
        if (is_hidden(name)) continue;
        std::string full = abs_dir + "/" + name;
        std::string rel  = join_rel(rel_prefix, name);
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (is_native_note_dir(full)) {
                std::string title = name;
                std::string raw; json::Value v;
                if (read_file(full + "/note.json", raw) && json::parse(raw, v))
                    title = v.str("title", name);
                entries_.push_back({rel, title, full,
                                    mtime_ms(full + "/note.json"),
                                    false, false});
            } else {
                scan_dir(full, rel);
            }
        } else if (S_ISREG(st.st_mode) && ends_with(lower(name), ".md")) {
            entries_.push_back({rel, name, full,
                                mtime_ms(full), true, false});
        }
    }
}

std::vector<IndexEntry> NotesIndex::walk_vault() const {
    NotesIndex tmp;
    tmp.root_ = root_;
    tmp.scan_dir(root_, "");
    return tmp.entries_;
}

std::string NotesIndex::resolve_link(const std::string &target) const {
    if (target.empty()) return "";
    auto entries = walk_vault();
    std::string t       = target;
    std::string t_lower = lower(t);
    std::string t_slug  = slugify(t);

    // Pass 1: exact relative-path match (with or without .md suffix)
    for (auto &e : entries) {
        if (e.id == t || e.id == t + ".md") return e.path;
    }
    // Pass 2: basename match (case-insensitive, with optional .md)
    for (auto &e : entries) {
        std::string base = basename_no_ext(e.id);
        if (lower(base) == t_lower) return e.path;
    }
    // Pass 3: slug match
    for (auto &e : entries) {
        if (slugify(basename_no_ext(e.id)) == t_slug) return e.path;
    }
    return "";
}

IndexEntry NotesIndex::create_note(const std::string &title, TemplateId tmpl) {
    std::string abs = subdir_.empty() ? root_ : root_ + "/" + subdir_;
    ensure_dir(abs);
    std::string id_base = slugify(title);
    std::string id = unique_id(abs, id_base);
    std::string dir = abs + "/" + id;
    ensure_dir(dir);

    Note n;
    n.id = id;
    n.title = title;
    n.default_template = tmpl;
    n.pages.emplace_back();
    n.pages.back().tmpl = tmpl;
    save_note(dir, n);

    std::string rel = join_rel(subdir_, id);
    IndexEntry e{rel, title, dir, now_ms(), false, false};
    entries_.insert(entries_.begin(), e);
    return e;
}

IndexEntry NotesIndex::create_markdown(const std::string &title) {
    std::string abs = subdir_.empty() ? root_ : root_ + "/" + subdir_;
    ensure_dir(abs);
    std::string base = slugify(title);
    if (!ends_with(base, ".md")) base += ".md";
    std::string path = abs + "/" + base;
    if (!path_exists(path))
        write_file(path, "# " + title + "\n");
    std::string rel = join_rel(subdir_, base);
    IndexEntry e{rel, base, path, now_ms(), true, false};
    entries_.insert(entries_.begin(), e);
    return e;
}

bool NotesIndex::remove_entry(const std::string &id) {
    for (auto &e : entries_) {
        if (e.id == id) {
            if (e.is_folder) return false;   // don't recursively nuke folders
            return remove_path(e.path);
        }
    }
    return false;
}

bool NotesIndex::rename_entry(const std::string &id,
                              const std::string &new_title) {
    if (new_title.empty()) return false;
    for (auto &e : entries_) {
        if (e.id != id) continue;
        if (e.is_folder) return false;
        if (e.is_markdown) {
            std::string base = slugify(new_title);
            if (base.empty()) return false;
            if (!ends_with(base, ".md")) base += ".md";
            auto slash = e.path.find_last_of('/');
            std::string dirpart =
                (slash == std::string::npos) ? "" : e.path.substr(0, slash);
            std::string newpath = dirpart.empty() ? base : dirpart + "/" + base;
            if (newpath == e.path) return true;
            if (path_exists(newpath)) return false;   // refuse to clobber
            return rename_path(e.path, newpath);
        }
        // Native note: change the title inside note.json; keep the dir slug.
        std::string nj = e.path + "/note.json";
        std::string raw; json::Value v;
        if (read_file(nj, raw) && json::parse(raw, v)) {
            v.obj["title"] = json::Value::make_str(new_title);
            return write_file(nj, json::serialize(v));
        }
        return false;
    }
    return false;
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
    // Persisted index is now optional metadata — the filesystem is the
    // source of truth. We still drop a snapshot for diagnostics.
    json::Value arr = json::Value::make_arr();
    for (auto &e : entries_) {
        json::Value o = json::Value::make_obj();
        o.obj.emplace("id",      json::Value::make_str(e.id));
        o.obj.emplace("title",   json::Value::make_str(e.title));
        o.obj.emplace("updated", json::Value::make_num(e.updated_ms));
        o.obj.emplace("md",      json::Value::make_bool(e.is_markdown));
        o.obj.emplace("dir",     json::Value::make_bool(e.is_folder));
        arr.arr.push_back(std::move(o));
    }
    return write_file(root_ + "/_index.json", json::serialize(arr));
}

} // namespace bn
