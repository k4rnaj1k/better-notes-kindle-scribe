project('betternotes', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++17', 'warning_level=2'])

gtk2       = dependency('gtk+-2.0')
pangocairo = dependency('pangocairo')
cairo      = dependency('cairo')

# Tesseract is optional; the OCR module compiles out cleanly when missing.
# Use static: true so pkg-config returns Libs.private (libpng, zlib, etc.)
# that the static .a archives need on the link line.
tesseract  = dependency('tesseract', required: false, static: true)
lept       = dependency('lept',      required: false, static: true)

# Cairo on the stock Kindle sysroot ships 1.16+ headers but an older
# libcairo.so that's missing cairo_tag_begin/end. Link-probe the symbol
# so pdf_export.cpp can fall back when the runtime lib lacks them.
cpp = meson.get_compiler('cpp')
has_cairo_tag = cpp.has_function('cairo_tag_begin', dependencies: cairo)

sources = files(
  'src/main.cpp',
  'src/app.cpp',
  'src/pen.cpp',
  'src/canvas.cpp',
  'src/strokes.cpp',
  'src/note_io.cpp',
  'src/index.cpp',
  'src/templates.cpp',
  'src/pages.cpp',
  'src/tools.cpp',
  'src/lasso.cpp',
  'src/links.cpp',
  'src/keyboard.cpp',
  'src/markdown.cpp',
  'src/pdf_export.cpp',
  'src/toolbar.cpp',
  'src/filebrowser.cpp',
  'src/util.cpp',
  'src/ocr.cpp',
  'src/json.cpp',
  'src/inkfb.cpp',
)

deps = [gtk2, pangocairo]
cpp_args = ['-static-libstdc++']
if tesseract.found() and lept.found()
  deps += [tesseract, lept]
  cpp_args += ['-DBN_HAVE_TESSERACT=1']
endif
if has_cairo_tag
  cpp_args += ['-DBN_HAVE_CAIRO_TAG=1']
endif

executable('betternotes',
  sources,
  dependencies: deps,
  cpp_args: cpp_args,
  link_args: ['-static-libstdc++', '-lpthread'],
  install: true)
