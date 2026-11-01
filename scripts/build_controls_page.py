"""Builds the player's controls page (docs/CONTROLS.html in a release) from docs/CONTROLS.md, so
the tables players read are the ones the code is checked against.

Usage: python -I scripts/build_controls_page.py [--source docs/CONTROLS.md] --out build/installer/CONTROLS.html
Standard library only (the release's embeddable Python runs it too)."""
import argparse
import html
from pathlib import Path
import re

STYLE = '''body{font-family:Segoe UI,Arial,sans-serif;max-width:960px;margin:2em auto;padding:0 1em;color:#222;background:#fafafa}
h1{font-size:1.6em}h2{font-size:1.25em;margin-top:1.6em;border-bottom:1px solid #ccc}
table{border-collapse:collapse;width:100%;margin:.6em 0}th,td{border:1px solid #ccc;padding:.35em .6em;text-align:left;vertical-align:top}
th{background:#eee}code{background:#eee;padding:0 .25em;border-radius:3px}'''


def controls_body(markdown):
    """Everything under the document's "# Controls" title line; the page supplies its own title."""
    match = re.search(r'^# Controls[^\n]*\n(.*)', markdown, re.M | re.S)
    if not match:
        raise SystemExit('the controls doc has no "# Controls" title')
    return match.group(1)


def inline(text):
    out = html.escape(text, quote=False)
    out = re.sub(r'`([^`]+)`', r'<code>\1</code>', out)
    out = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', out)
    # Links point into the repo, which players don't have: keep the words, drop the target.
    out = re.sub(r'\[([^\]]+)\]\([^)]+\)', r'\1', out)
    return out


def cells(line):
    return [c.strip() for c in line.strip().strip('|').split('|')]


def to_html(markdown):
    parts, paragraph, lines, i = [], [], markdown.splitlines(), 0

    def flush():
        if paragraph:
            parts.append('<p>' + inline(' '.join(paragraph)) + '</p>')
            paragraph.clear()

    while i < len(lines):
        line = lines[i]
        if line.startswith('### '):
            flush()
            parts.append('<h3>' + inline(line[4:]) + '</h3>')
        elif line.startswith('## '):
            flush()
            parts.append('<h2>' + inline(line[3:]) + '</h2>')
        elif line.startswith('|') and i + 1 < len(lines) and re.match(r'^\|[-| :]+\|$', lines[i + 1].strip()):
            flush()
            rows = ['<tr>' + ''.join(f'<th>{inline(c)}</th>' for c in cells(line)) + '</tr>']
            i += 2
            while i < len(lines) and lines[i].startswith('|'):
                rows.append('<tr>' + ''.join(f'<td>{inline(c)}</td>' for c in cells(lines[i])) + '</tr>')
                i += 1
            parts.append('<table>' + ''.join(rows) + '</table>')
            continue
        elif not line.strip():
            flush()
        else:
            paragraph.append(line.strip())
        i += 1
    flush()
    return '\n'.join(parts)


def build(markdown, version=''):
    title = 'SM2VR controls' + (f' ({version})' if version else '')
    return (f'<!doctype html>\n<html><head><meta charset="utf-8"><title>{html.escape(title)}</title>'
            f'<style>{STYLE}</style></head><body>\n<h1>{html.escape(title)}</h1>\n'
            f'{to_html(controls_body(markdown))}\n</body></html>\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'docs' / 'CONTROLS.md')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--version', default='')
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(build(args.source.read_text(encoding='utf-8'), args.version), encoding='utf-8')


if __name__ == '__main__':
    main()
