#!/usr/bin/env bash
# Assembles the thesis (front matter, Chapters 1 to 6, references) into docs/thesis/thesis.docx, with a
# page break before each part; the joined Markdown goes to build/thesis.md.
# The faculty's format comes from its Word template: pass it as REFERENCE_DOCX, or apply it in Word.
#   docs/thesis/build.sh     env: PANDOC=pandoc  REFERENCE_DOCX=<faculty template .docx>  WORD=0 (no Word pass)
set -euo pipefail
cd "$(dirname "$0")"
pandoc=${PANDOC:-pandoc}
mkdir -p build
page_break=$'\n\n```{=openxml}\n<w:p><w:r><w:br w:type="page"/></w:r></w:p>\n```\n\n'
{
  cat front_matter.md
  for f in ch1_introduction.md ch2_background.md ch3_design.md ch4_implementation.md ch5_evaluation.md \
    ch6_conclusion.md references.md; do
    printf '%s' "$page_break"
    cat "$f"
  done
} >build/thesis.md
# the chapters start lists right under a sentence, as GitHub renders them
# the contents are a Word field placed after the front matter (front_matter.md), filled in by Word;
# reference.docx is pandoc's default plus a centred "Title page" style
"$pandoc" build/thesis.md -f markdown+lists_without_preceding_blankline-implicit_figures --resource-path=. \
  --reference-doc="${REFERENCE_DOCX:-reference.docx}" -o thesis.docx
echo "thesis.docx ($(du -h thesis.docx | cut -f1))"
# Word (Windows, through WSL) fills the contents field and exports the PDF; WORD=0 skips it
if [ "${WORD:-1}" != 0 ] && command -v powershell.exe >/dev/null; then
  win=/mnt/c/Users/Public
  cp thesis.docx word_export.ps1 "$win/"
  timeout 300 powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'C:\Users\Public\word_export.ps1' \
    -Docx 'C:\Users\Public\thesis.docx' -Pdf 'C:\Users\Public\thesis.pdf' | tr -d '\r'
  mv "$win/thesis.docx" "$win/thesis.pdf" .
  rm -f "$win/word_export.ps1"
  echo "thesis.pdf ($(du -h thesis.pdf | cut -f1))"
fi
