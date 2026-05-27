project('betternotes', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++17', 'warning_level=2'])

gtk2       = dependency('gtk+-2.0')
pangocairo = dependency('pangocairo')

# Tesseract is optional; the OCR module compiles out cleanly when missing.
tesseract  = dependency('tesseract', required: false)
lept       = dependency('lept',      required: false)

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
)

deps = [gtk2, pangocairo]
cpp_args = ['-static-libstdc++']
if tesseract.found() and lept.found()
  deps += [tesseract, lept]
  cpp_args += ['-DBN_HAVE_TESSERACT=1']
endif

executable('betternotes',
  sources,
  dependencies: deps,
  cpp_args: cpp_args,
  link_args: ['-static-libstdc++', '-lpthread'],
  install: true)
