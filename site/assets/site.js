// The site's only script. Every page reads fine without it: it adds the
// copy buttons, heading link marks, search and a few conveniences.
(function () {
  'use strict';
  var $$ = function (sel, root) { return [].slice.call((root || document).querySelectorAll(sel)); };
  var el = function (tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };

  // the rail marks the section being read
  var spy = $$('aside.toc a[data-s]');
  if (spy.length && window.IntersectionObserver) {
    var io = new IntersectionObserver(function (es) {
      es.forEach(function (e) {
        if (!e.isIntersecting) return;
        spy.forEach(function (a) { a.classList.toggle('on', a.dataset.s === e.target.id); });
      });
    }, { rootMargin: '-15% 0px -75% 0px' });
    $$('article h2[id]').forEach(function (h) { io.observe(h); });
  }

  // a link mark on every section heading
  $$('article h2[id], article h3[id]').forEach(function (h) {
    var a = el('a', 'anchor', '#');
    a.href = '#' + h.id;
    a.setAttribute('aria-label', 'Link to this section');
    h.appendChild(a);
  });

  // code blocks: a tint on shell prompts and comments, and a copy button
  // that leaves the prompt out
  var SHELL = /language-(sh|shell|bash|zsh|console)\b/;
  function copyText(text, done) {
    var fallback = function () {
      var ta = el('textarea');
      ta.value = text;
      ta.setAttribute('readonly', '');
      ta.style.position = 'fixed';
      ta.style.opacity = '0';
      document.body.appendChild(ta);
      ta.select();
      var ok = false;
      try { ok = document.execCommand('copy'); } catch (e) {}
      document.body.removeChild(ta);
      done(ok);
    };
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(function () { done(true); }, fallback);
    } else {
      fallback();
    }
  }
  $$('article pre').forEach(function (pre) {
    var code = pre.querySelector('code') || pre;
    var text = code.textContent.replace(/\n$/, '');
    var plain = text.split('\n').map(function (l) { return l.replace(/^\$ /, ''); }).join('\n');
    if (SHELL.test(code.className)) {
      code.textContent = '';
      text.split('\n').forEach(function (line, i) {
        if (i) code.appendChild(document.createTextNode('\n'));
        var m = /^\$ /.exec(line);
        if (m) { code.appendChild(el('span', 'sh-p', '$ ')); line = line.slice(2); }
        if (/^\s*#/.test(line)) code.appendChild(el('span', 'sh-c', line));
        else code.appendChild(document.createTextNode(line));
      });
    }
    var box = el('div', 'codeblock');
    pre.parentNode.insertBefore(box, pre);
    box.appendChild(pre);
    var btn = el('button', 'copy', 'Copy');
    btn.type = 'button';
    btn.setAttribute('aria-label', 'Copy this code');
    btn.addEventListener('click', function () {
      copyText(plain, function (ok) {
        btn.textContent = ok ? 'Copied' : 'Select it';
        btn.classList.toggle('done', ok);
        if (!ok) {
          var r = document.createRange();
          r.selectNodeContents(code);
          var s = window.getSelection();
          s.removeAllRanges();
          s.addRange(r);
        }
        setTimeout(function () { btn.textContent = 'Copy'; btn.classList.remove('done'); }, 1600);
      });
    });
    box.appendChild(btn);
  });

  // frame comparisons: one pane at a time behind a switch. The arrow
  // keys move between panes while the switch has focus
  $$('.cmp').forEach(function (box) {
    var panes = $$('.cmp-pane', box);
    if (panes.length < 2) return;
    var bar = el('div', 'cmp-switch');
    bar.setAttribute('role', 'group');
    bar.setAttribute('aria-label', 'Which picture to show');
    var btns = panes.map(function (p, i) {
      var b = el('button', null, p.dataset.name);
      b.type = 'button';
      b.addEventListener('click', function () { pick(i); });
      bar.appendChild(b);
      return b;
    });
    function pick(i) {
      panes.forEach(function (p, j) { p.classList.toggle('show', i === j); });
      btns.forEach(function (b, j) { b.setAttribute('aria-pressed', i === j ? 'true' : 'false'); });
    }
    bar.addEventListener('keydown', function (e) {
      var d = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0;
      if (!d) return;
      var i = (btns.indexOf(document.activeElement) + d + btns.length) % btns.length;
      btns[i].focus();
      pick(i);
      e.preventDefault();
    });
    // a hidden pane never scrolls into view, so its picture loads when
    // the box comes near and a flick doesn't flash
    var ready = function () { $$('.cmp-pane img', box).forEach(function (im) { im.loading = 'eager'; }); };
    if (window.IntersectionObserver) {
      var near = new IntersectionObserver(function (es, o) {
        if (es.some(function (e) { return e.isIntersecting; })) { ready(); o.disconnect(); }
      }, { rootMargin: '600px 0px' });
      near.observe(box);
    } else ready();
    box.insertBefore(bar, panes[0]);
    box.classList.add('on');
    var first = 0;
    panes.forEach(function (p, i) { if (p.hasAttribute('data-first')) first = i; });
    pick(first);
  });

  // a table wider than its box pins its first column, when that column
  // leaves at least half the box for the rest
  function pinTables() {
    $$('.tbl').forEach(function (box) {
      box.classList.remove('pin');
      var first = box.querySelector('th, td');
      var wide = box.scrollWidth > box.clientWidth + 1;
      if (wide && first && first.offsetWidth < box.clientWidth * 0.5) box.classList.add('pin');
      if (wide) box.tabIndex = 0; else box.removeAttribute('tabindex');
    });
  }
  pinTables();
  var pinTimer;
  window.addEventListener('resize', function () { clearTimeout(pinTimer); pinTimer = setTimeout(pinTables, 150); });

  // menus close on a click elsewhere, on Escape and after a choice
  var menus = $$('header.site details');
  function closeMenus(except) {
    menus.forEach(function (d) { if (d !== except) d.removeAttribute('open'); });
  }
  menus.forEach(function (d) {
    d.addEventListener('toggle', function () { if (d.open) closeMenus(d); });
  });
  document.addEventListener('click', function (e) {
    var inside = e.target.closest && e.target.closest('header.site details');
    if (!inside) closeMenus();
    else if (e.target.closest('a')) closeMenus();
  });

  // search: the index loads on first focus, one record per section
  var wrap = document.querySelector('.search');
  var input = document.getElementById('q');
  var panel = document.getElementById('q-results');
  if (wrap && input && panel) {
    wrap.hidden = false;
    var index = null, loading = false;
    var norm = function (s) { return s.toLowerCase().replace(/[‘’]/g, "'").replace(/[“”]/g, '"'); };
    var load = function () {
      if (index || loading) return;
      loading = true;
      var s = document.createElement('script');
      s.src = 'assets/search-index.js';
      s.onload = function () {
        index = (window.YAH265_SEARCH || []).map(function (r) {
          return { r: r, h: norm(r.h), t: norm(r.t), x: norm(r.x) };
        });
        render();
      };
      s.onerror = function () { loading = false; };
      document.head.appendChild(s);
    };
    var count = function (hay, word) {
      var n = 0, i = -1;
      while (n < 5 && (i = hay.indexOf(word, i + 1)) !== -1) n++;
      return n;
    };
    var find = function (q) {
      var words = q.split(/\s+/).filter(Boolean);
      var hits = [];
      index.forEach(function (e) {
        var score = 0;
        for (var i = 0; i < words.length; i++) {
          var w = words[i];
          var inH = e.h.indexOf(w) !== -1, inT = e.t.indexOf(w) !== -1, n = count(e.x, w);
          if (!inH && !inT && !n) return;
          score += (inH ? 10 : 0) + (inT ? 3 : 0) + n;
        }
        if (words.length > 1) {
          if (e.h.indexOf(q) !== -1) score += 20;
          if (e.x.indexOf(q) !== -1) score += 8;
        }
        hits.push({ e: e, score: score });
      });
      hits.sort(function (a, b) { return b.score - a.score; });
      return hits.slice(0, 8);
    };
    var snippet = function (e, q) {
      var words = q.split(/\s+/).filter(Boolean);
      var at = e.x.indexOf(q);
      if (at === -1) at = e.x.indexOf(words[0]);
      if (at === -1) at = 0;
      var from = Math.max(0, at - 50);
      var text = e.r.x.slice(from, from + 150).trim();
      return (from ? '…' : '') + text + (from + 150 < e.r.x.length ? '…' : '');
    };
    var close = function () { panel.hidden = true; };
    var render = function () {
      var q = norm(input.value).trim();
      if (!q || !index) { close(); return; }
      panel.textContent = '';
      var hits = find(q);
      if (!hits.length) {
        panel.appendChild(el('p', 'none', 'Nothing matches “' + input.value.trim() + '”.'));
      }
      hits.forEach(function (hit) {
        var r = hit.e.r;
        var a = el('a');
        a.href = r.p + (r.a ? '#' + r.a : '');
        if (r.h !== r.t) a.appendChild(el('span', 'where', r.t));
        a.appendChild(el('b', '', r.h));
        a.appendChild(el('span', 'snip', snippet(hit.e, q)));
        panel.appendChild(a);
      });
      panel.hidden = false;
    };
    input.addEventListener('focus', function () { load(); closeMenus(); render(); });
    input.addEventListener('input', render);
    input.addEventListener('keydown', function (e) {
      var first = panel.querySelector('a');
      if (e.key === 'Escape') { close(); input.blur(); }
      else if (e.key === 'Enter' && first && !panel.hidden) { first.click(); }
      else if (e.key === 'ArrowDown' && first && !panel.hidden) { e.preventDefault(); first.focus(); }
    });
    panel.addEventListener('keydown', function (e) {
      var links = $$('a', panel), i = links.indexOf(document.activeElement);
      if (e.key === 'ArrowDown' && i + 1 < links.length) { e.preventDefault(); links[i + 1].focus(); }
      else if (e.key === 'ArrowUp') { e.preventDefault(); (i > 0 ? links[i - 1] : input).focus(); }
      else if (e.key === 'Escape') { close(); input.focus(); }
    });
    panel.addEventListener('click', function (e) { if (e.target.closest('a')) close(); });
    document.addEventListener('click', function (e) { if (!wrap.contains(e.target)) close(); });
  }

  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape') closeMenus();
    if (e.key !== '/' || e.metaKey || e.ctrlKey || e.altKey || !input) return;
    var t = e.target;
    if (t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName))) return;
    e.preventDefault();
    input.focus();
    input.select();
  });
})();
