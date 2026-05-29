#pragma once
#include "strokes.h"

#include <string>
#include <vector>

namespace bn {

struct IndexEntry {
    std::string id;          // path relative to vault root (e.g. "work/meeting.md")
    std::string title;       // display title (filename or note.title)
    std::string path;        // absolute filesystem path
    uint32_t    updated_ms = 0;
    bool        is_markdown = false;  // plain .md file (Obsidian-style)
    bool        is_folder   = false;  // child directory at the current level
    // Notebook-level tags (native notes only), read from note.json for display.
    std::vector<std::string> tags;
};

// Treats the notes_dir as a vault root. open(root, subdir="") shows the
// entries at that subdirectory level (folders + .md files + native note dirs
// containing note.json). Obsidian vaults work out of the box: drop a vault
// into notes_dir and every .md becomes a viewable/editable note. Nested
// directories are navigable through the file browser.
class NotesIndex {
public:
    bool open(const std::string &root);                // scans root, lists root-level entries
    bool open(const std::string &root, const std::string &subdir);

    const std::vector<IndexEntry> &entries() const { return entries_; }
    const std::string &root()    const { return root_; }
    const std::string &subdir()  const { return subdir_; }

    // Recursive: returns ALL .md files and note-dirs anywhere under root.
    // Used by the link resolver for [[wiki-style]] links.
    std::vector<IndexEntry> walk_vault() const;

    // Resolve a wiki-link target ("MyNote" or "folder/MyNote") against the
    // vault. Tries (in order): exact filename match, slugified match,
    // case-insensitive match. Returns absolute path or "" if not found.
    std::string resolve_link(const std::string &target) const;

    // Allocate a fresh note directory inside the current subdir.
    IndexEntry create_note(const std::string &title, TemplateId tmpl);

    // Allocate a fresh markdown file inside the current subdir.
    IndexEntry create_markdown(const std::string &title);

    // Create a subfolder inside the current subdir. Returns the new entry (or
    // an existing one if the folder already existed). Re-open() afterwards.
    IndexEntry create_folder(const std::string &name);

    // Move the entry with this id into dest_dir (vault-relative, "" = root),
    // keeping its on-disk basename. Refuses to move a folder into itself or a
    // descendant, or to clobber an existing name. Re-open() afterwards.
    bool move_entry(const std::string &id, const std::string &dest_dir);

    // Refresh updated_ms / title for one note after save.
    void touch(const std::string &id, const std::string &title);

    // Delete the entry with this id from disk (recursively for note dirs).
    // Caller should re-open() afterwards to refresh the listing.
    bool remove_entry(const std::string &id);

    // Rename the entry. Markdown files are renamed on disk (slugified +
    // ".md"); native notes keep their directory but get the new title written
    // into note.json. Folders are left untouched. Re-open() afterwards.
    bool rename_entry(const std::string &id, const std::string &new_title);

    bool save() const;
private:
    void scan_dir(const std::string &abs_dir, const std::string &rel_prefix);

    std::string root_;
    std::string subdir_;   // "" = root, otherwise path relative to root_
    std::vector<IndexEntry> entries_;
};

} // namespace bn
