(function () {
  var INSTALL = "git clone https://github.com/thehumanworks/tny && cd tny && make";

  function applyTheme(theme) {
    var root = document.documentElement;
    root.classList.remove("light", "dark");
    if (theme === "light" || theme === "dark") root.classList.add(theme);
    try {
      localStorage.setItem("theme", theme);
    } catch (e) {}
  }

  try {
    var saved = localStorage.getItem("theme");
    if (saved === "light" || saved === "dark") applyTheme(saved);
  } catch (e) {}

  function copyText(text, done) {
    function fallback() {
      var ta = document.createElement("textarea");
      ta.value = text;
      ta.setAttribute("readonly", "");
      ta.style.position = "fixed";
      ta.style.left = "-9999px";
      document.body.appendChild(ta);
      ta.select();
      try {
        document.execCommand("copy");
      } catch (e) {}
      document.body.removeChild(ta);
      if (done) done();
    }
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(done).catch(fallback);
    } else {
      fallback();
    }
  }

  function flash(el, label) {
    if (!el) return;
    var prev = el.textContent;
    el.textContent = label || "copied";
    el.classList.add("show");
    window.setTimeout(function () {
      el.textContent = prev;
      el.classList.remove("show");
    }, 1200);
  }

  document.querySelectorAll("[data-copy], [data-site-install]").forEach(function (btn) {
    btn.addEventListener("click", function () {
      var text = btn.getAttribute("data-copy") || INSTALL;
      copyText(text, function () {
        var live = btn.querySelector("[data-copy-status]");
        flash(live, "copied");
      });
    });
  });

  var drawer = document.getElementById("docs-drawer");
  var openBtn = document.getElementById("docs-open");
  var closeBtn = document.getElementById("docs-close");
  var backdrop = document.getElementById("docs-backdrop");

  function setDrawer(open) {
    if (!drawer) return;
    drawer.classList.toggle("open", open);
    drawer.setAttribute("aria-hidden", open ? "false" : "true");
    if (openBtn) openBtn.setAttribute("aria-expanded", open ? "true" : "false");
  }

  if (openBtn) openBtn.addEventListener("click", function () { setDrawer(true); });
  if (closeBtn) closeBtn.addEventListener("click", function () { setDrawer(false); });
  if (backdrop) backdrop.addEventListener("click", function () { setDrawer(false); });
  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") setDrawer(false);
  });

  var hint = document.querySelector("[data-scroll]");
  if (hint) {
    hint.addEventListener("click", function () {
      var id = hint.getAttribute("data-scroll");
      var el = id ? document.getElementById(id) : null;
      if (el) el.scrollIntoView({ behavior: "smooth", block: "start" });
    });
  }
})();
