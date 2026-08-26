#!/usr/bin/env python3
import unittest

from generate_capy_docs import detail_html, read_signature_data


class DocumentationGeneratorTests(unittest.TestCase):
    def test_reads_call_boundary_from_generated_signature(self):
        data = 'struct DocCapySignature { std::string_view page, declaration, boundary; };\n'
        data += '\t{"hosted", "function hosted() bool", "host"},\n'
        data += '\t{"pure", "function pure() bool", "lib"},\n'
        signatures = read_signature_data(data)
        self.assertEqual(signatures["hosted"], [("function hosted() bool", "host")])
        self.assertEqual(signatures["pure"], [("function pure() bool", "lib")])

    def test_renders_badge_on_api_title_row(self):
        page = {"kind": "api", "title": "hosted", "label": "hosted", "source_page": "hosted", "call_boundary": "host", "capy_sig_lines": [], "content_html": "", "param_html": [], "returns_html": "", "errors_html": "", "notes_html": [], "warnings_html": [], "example_error": "", "examples": [], "guide_examples": [], "see": []}
        html = detail_html(page)
        self.assertIn('<div class="doc-title"><h2>hosted</h2><span class="call-boundary host-call">host call</span></div>', html)

    def test_rejects_api_without_call_boundary(self):
        page = {"kind": "api", "title": "missing", "label": "missing", "source_page": "missing", "call_boundary": "", "capy_sig_lines": [], "content_html": "", "param_html": [], "returns_html": "", "errors_html": "", "notes_html": [], "warnings_html": [], "example_error": "", "examples": [], "guide_examples": [], "see": []}
        with self.assertRaises(ValueError):
            detail_html(page)


if __name__ == "__main__":
    unittest.main()
