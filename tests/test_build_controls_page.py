"""The players' controls page carries every row of the docs/CONTROLS.md tables and no repo links."""
from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import build_controls_page  # noqa: E402

CONTROLS = (Path(__file__).resolve().parents[1] / 'docs' / 'CONTROLS.md').read_text(encoding='utf-8')


class ControlsPageTests(unittest.TestCase):
    def test_every_table_row_is_on_the_page(self):
        body = build_controls_page.controls_body(CONTROLS)
        rows = [l for l in body.splitlines() if l.startswith('|') and not re.match(r'^\|[-| :]+\|$', l)]
        page = build_controls_page.build(CONTROLS, '0.1.0')
        self.assertEqual(page.count('<tr>'), len(rows))
        self.assertGreater(len(rows), 30)
        for heading in ('Left controller', 'Right controller', 'Viewer mode', 'Keyboard', 'Chase camera'):
            self.assertIn(f'<h2>{heading}', page)
        self.assertIn('<title>SM2VR controls (0.1.0)</title>', page)
        self.assertEqual(page.count('<h1>'), 1)

    def test_inline_markup(self):
        html = build_controls_page.to_html('Text with `code`, **bold** and [a doc](DEVELOPING.md).\n')
        self.assertEqual(html, '<p>Text with <code>code</code>, <strong>bold</strong> and a doc.</p>')
        self.assertNotIn('href', build_controls_page.build(CONTROLS))

    def test_headings(self):
        self.assertEqual(build_controls_page.to_html('## Two\n\n### Three\n'), '<h2>Two</h2>\n<h3>Three</h3>')

    def test_missing_title_is_an_error(self):
        with self.assertRaises(SystemExit):
            build_controls_page.controls_body('# no controls here\n\n## Left controller\n')


if __name__ == '__main__':
    unittest.main()
