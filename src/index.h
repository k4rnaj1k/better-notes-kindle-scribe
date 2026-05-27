#pragma once
#include "strokes.h"

#include <string>
#include <vector>

namespace bn {

struct IndexEntry {
    std::string id;          // folder name
    std::string title;
    std::string path;        // absolute
    uint32_t    updated_ms = 0;
    bool        is_markdown = false;
};

class NotesIndex {
public:
    bool open(const std::string &root);          // scans root, builds list
    const std::vector<IndexEntry> &entries() const { return entries_; }
    const std::string &root() const { return root_; }

    // Allocate a fresh note directory for `title`, persist a starter note.json,
    // and return its IndexEntry.
    IndexEntry create_note(const std::string &title, TemplateId tmpl);

    // Allocate a fresh markdown file.
    IndexEntry create_markdown(const std::string &title);

    // Refresh updated_ms / title for one note after save.
    void touch(const std::string &id, const std::string &title);

    bool save() const;
private:
    std::string root_;
    std::vector<IndexEntry> entries_;
};

} // namespace bn
