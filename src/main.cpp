// BetterNotes — note-taking app for jailbroken Kindle Scribe.
//
// Entry point. Parses command-line flags, hands control to bn::App which
// runs the GTK2 + Cairo UI and the pen / OCR worker threads.

#include "app.h"
#include "util.h"

#include <cstring>

int main(int argc, char *argv[]) {
    auto &app = bn::App::instance();

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--notes-dir") == 0)
            app.set_notes_dir(argv[++i]);
        else if (std::strcmp(argv[i], "--tessdata") == 0)
            app.set_tessdata_dir(argv[++i]);
    }
    return app.run(argc, argv);
}
