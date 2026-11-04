"""MkDocs hooks for the documentation site (mkdocs.yml).

- The home page is the landing page (docs/overrides/home.html); the repository README.md becomes the Overview page.
  docs/README.md (the index for browsing on GitHub) is left out, the site navigation replaces it.
- Relative links are written for GitHub. A link to a file the site carries is kept (made relative to the page); a link
  to anything else in the repository (sources, LICENSE, CHANGELOG.md, docs/superpowers/) becomes a GitHub URL.
"""

import posixpath
import re
from pathlib import Path

from mkdocs.structure.files import File

ROOT = Path(__file__).resolve().parent.parent
BLOB = "https://github.com/t3hk0d3/esp32-p4-otbr/blob/master/"
# ](target) and src="target", target without a scheme, an anchor-only form or an absolute path
HOME = "---\ntemplate: home.html\ntitle: Thread border router for your whole home\nhide: [navigation, toc]\n---\n"
LINK = re.compile(r'(\]\(|src=")(?![a-z][a-z0-9+.-]*:|#|/)([^)"\s#]+)')


def on_files(files, config):
    readme = files.get_file_from_path("README.md")
    if readme is not None:
        files.remove(readme)
    files.append(File.generated(config, "index.md", content=HOME))
    files.append(
        File.generated(
            config,
            "overview.md",
            content=(ROOT / "README.md").read_text(encoding="utf-8"),
        )
    )
    return files


def on_page_markdown(markdown, page, config, files):
    # the page's place in the repository: overview.md stands for the root README.md
    repo_dir = (
        ""
        if page.file.src_uri == "overview.md"
        else posixpath.dirname("docs/" + page.file.src_uri)
    )
    site_dir = posixpath.dirname(page.file.src_uri)

    def rewrite(match):
        target = posixpath.normpath(posixpath.join(repo_dir, match.group(2)))
        if (
            target.startswith("docs/")
            and files.get_file_from_path(target[len("docs/") :]) is not None
        ):
            return match.group(1) + posixpath.relpath(
                target[len("docs/") :], site_dir or "."
            )
        return match.group(1) + BLOB + target

    return LINK.sub(rewrite, markdown)
