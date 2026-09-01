from pathlib import Path


source = Path("web/js/app.js").read_text()
ensure_auth = source[source.index("async function ensureAuth"):source.index("async function signOut")]
show_page = source[source.index("function showPage"):source.index("items.forEach", source.index("function showPage"))]
bootstrap = source[source.index("api('/config')"):]

# The shell must remain hidden until the first requested page has finished
# rendering; otherwise the static sidebar placeholders flash behind the UI.
assert "document.body.classList.remove('auth-pending')" not in ensure_auth
assert "return render()" in show_page
assert "await showPage(pageFromLocation(),false)" in bootstrap
assert "document.body.classList.remove('auth-pending')" in bootstrap

# A failed first render must not leave the green Connecting placeholder behind.
assert "markStartupUnavailable" in source
assert "markStartupUnavailable()" in bootstrap

print("startup state contract: ok")
